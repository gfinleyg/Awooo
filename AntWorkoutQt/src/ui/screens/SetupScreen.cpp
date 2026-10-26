#include "SetupScreen.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "AnalysisCache.hpp"
#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AwoParser.hpp"
#include "CadenceWorkout.hpp"
#include "CalibratePowerWindow.hpp"
#include "DeviceDialogs.hpp"
#include "Dialogs.hpp"
#include "FtpTest.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "LongVLabels.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutHelpers.hpp"
#include "WorkoutSelectDialog.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

// the text styles (laid out for 1920x1200; other screen sizes are scaled, see AppLayout)
const QString kXlText = QStringLiteral("font: 900 24pt 'Orbitron';");
const QString kLgText = QStringLiteral("font: 900 18pt 'Orbitron';");
const QString kMdText = QStringLiteral("font: 900 16pt 'Orbitron';");
const QString kXlBtnText = QStringLiteral("font: 24pt 'Segoe UI Black';");
const QString kSmBtnText = QStringLiteral("font: 10pt 'Segoe UI Black';");
const QString kSelectedLabel = QStringLiteral("background-color:#9fc5d1;");

// the selected mode is the disabled button (clicking it again does nothing): bright blue, a thick black border. The
// others look like normal raised buttons that light up on hover (the text's navy is its own color, not the window
// gradient's #28275e, so a theme can recolor them apart)
const QString kModeButtonStyle = QStringLiteral(R"(QPushButton {color: #28285e; border-style: outset; border-width: 2px; border-color: #808080; padding: 4px;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #9fc5d1);
                                border-radius: 15px; font: bold 34px 'Segoe UI Black';}
                            QPushButton:hover {border-color: black;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #1CB5E0);}
                            QPushButton:pressed {border-style: inset;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);}
                            QPushButton:disabled {color: black; border-style: solid; border-width: 2px; border-color: black;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);})");

QString buttonStyle(bool on, const QString& font, bool disabledLook) {
    QString style = QStringLiteral(
                        "QPushButton {color: #000001; background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 "
                        "#1CB5E0, stop: 1 #dadbde);                                 border-style: %1; border-width: 2px; "
                        "border-color: %2; border-radius: 15px; padding: 4px;")
                        .arg(on ? QStringLiteral("solid") : QStringLiteral("outset"),
                             on ? QStringLiteral("white") : QStringLiteral("black")) +
                    font +
                    QStringLiteral("}QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, "
                                   "stop: 0 #1CB5E0, stop: 1 #1f15e2);}");
    if (disabledLook) {
        style += QStringLiteral(" QPushButton:disabled {color: grey;background-color: qlineargradient(x1: 0, y1: 0, "
                                "x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d);}");
    }
    return style;
}

QString textStyle(bool on) {
    return QStringLiteral("QLineEdit{border-style:solid; border-color:%1; border-width: 2px;                                 "
                          "background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1);")
               .arg(on ? QStringLiteral("white") : QStringLiteral("black")) +
           kMdText + u'}';
}

// a stylesheet set only when it's a change: every stylesheet goes through the theme and re-polishes the widget, and
// setupBlink sets the same ones again every 300 ms
void setSheetOnce(QWidget* widget, const QString& sheet) {
    if (theme::originalSheet(widget) != sheet) {
        theme::setSheet(widget, sheet);
    }
}

// keeps its space while hidden
QSizePolicy retainSize() {
    QSizePolicy policy;
    policy.setRetainSizeWhenHidden(true);
    return policy;
}

QLabel* selectedLabel(int width, int height) {
    auto* label = new QLabel;
    label->setMinimumSize(width, height);
    label->setAlignment(Qt::AlignCenter);
    theme::setSheet(label, kSelectedLabel);
    label->setSizePolicy(retainSize());
    label->hide();
    return label;
}

QString typeOf(const QString& line) {
    return line.split(u' ', Qt::SkipEmptyParts).value(0);
}

bool isPowerType(const QString& type) {
    return type == u"11" || type == u"17";
}

// a whole number setting from the saved settings, in range
std::optional<int> savedInt(const QJsonObject& settings, const QString& key, int low, int high, bool highIncluded) {
    const QJsonValue value = settings.value(key);
    if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble())) {
        return std::nullopt;
    }
    const int number = value.toInt();
    if (number < low || (highIncluded ? number > high : number >= high)) {
        return std::nullopt;
    }
    return number;
}

}  // namespace

SetupScreen::SetupScreen(ant::AntBridge* bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge), timer_(new QTimer(this)), blinkTimer_(new QTimer(this)) {
    setWindowTitle(QStringLiteral("Awooo Workout Setup"));
    btnBlinkOn_ = buttonStyle(true, kXlBtnText, true);
    btnBlinkOff_ = buttonStyle(false, kXlBtnText, true);
    tinyBtnBlinkOn_ = buttonStyle(true, kSmBtnText, false);
    tinyBtnBlinkOff_ = buttonStyle(false, kSmBtnText, false);
    txtBlinkOn_ = textStyle(true);
    txtBlinkOff_ = textStyle(false);

    AppState& state = appState();
    state.workoutList.clear();
    state.workoutFilename.clear();
    state.workoutFilenameShort.clear();
    state.workoutTitle.clear();
    state.workoutMessages.clear();

    // ---- The devices column ----
    introductoryLayout_ = new QGridLayout(this);
    initializationWidget_ = new QWidget;
    initializationWidget_->setMaximumWidth(800);
    auto* init = new QGridLayout(initializationWidget_);
    auto* requiredHeader = new QLabel(QStringLiteral("  Required Power Device:  "));
    requiredHeader->setMaximumHeight(60);
    theme::setSheet(requiredHeader, kXlText);
    init->addWidget(requiredHeader, 0, 0, 1, 2, Qt::AlignHCenter);
    powerButton_ = new QPushButton(QStringLiteral("Power Device"));
    connect(powerButton_, &QPushButton::clicked, this, [this] { selectDevices(0); });
    theme::setSheet(powerButton_, btnBlinkOff_);
    powerButton_->setEnabled(false);
    init->addWidget(powerButton_, 1, 0, 1, 2);
    selectedPowerLabel_ = new QLabel;
    selectedPowerLabel_->setMaximumHeight(30);
    selectedPowerLabel_->setMinimumWidth(782);
    selectedPowerLabel_->setAlignment(Qt::AlignCenter);
    theme::setSheet(selectedPowerLabel_, kSelectedLabel);
    selectedPowerLabel_->setSizePolicy(retainSize());
    selectedPowerLabel_->hide();
    // an FE-C trainer as the power device gets a Max Watts box beneath it, the top of the free mode Watts slider; in
    // workout mode the same box is the Power Drive Cutoff instead (see updateMaxWattsMode)
    auto* selectedPowerLayout = new QGridLayout;
    selectedPowerLayout->addWidget(selectedPowerLabel_, 0, 0, 1, 2);
    // the label and box side by side, centered under the power device
    auto* maxWattsLayout = new QHBoxLayout;
    maxWattsLayout->addStretch();
    maxWattsLabel_ = new QLabel(QStringLiteral("Max Watts:"));
    maxWattsLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    maxWattsLabel_->setSizePolicy(retainSize());
    maxWattsLabel_->hide();
    maxWattsLayout->addWidget(maxWattsLabel_);
    textMaxWatts_ = new QLineEdit(QString::number(state.maxWatts));
    textMaxWatts_->setAlignment(Qt::AlignCenter);
    theme::setSheet(textMaxWatts_, kLgText);
    textMaxWatts_->setMaxLength(4);
    textMaxWatts_->setFixedSize(100, 40);
    connect(textMaxWatts_, &QLineEdit::editingFinished, this, &SetupScreen::textMaxWattsValidator);
    textMaxWatts_->setSizePolicy(retainSize());
    textMaxWatts_->hide();
    maxWattsLayout->addWidget(textMaxWatts_);
    maxWattsLayout->addStretch();
    selectedPowerLayout->addLayout(maxWattsLayout, 1, 0, 1, 2);
    init->addLayout(selectedPowerLayout, 2, 0, 1, 2);
    // ("HR" rather than "Heart Rate" so it fits this column on one line)
    auto* optionalHeader = new QLabel(QStringLiteral("Optional HR/Cadence/Speed Device(s):"));
    optionalHeader->setMaximumHeight(60);
    theme::setSheet(optionalHeader, kXlText);
    init->addWidget(optionalHeader, 3, 0, 1, 2, Qt::AlignHCenter);
    hrmButton_ = new QPushButton(QStringLiteral("Heart Rate Monitor"));
    connect(hrmButton_, &QPushButton::clicked, this, [this] { selectDevices(1); });
    theme::setSheet(hrmButton_, btnBlinkOff_);
    hrmButton_->setEnabled(false);
    init->addWidget(hrmButton_, 4, 0);
    speedCadenceButton_ = new QPushButton(QStringLiteral("Speed/Cadence Device"));
    connect(speedCadenceButton_, &QPushButton::clicked, this, [this] { selectDevices(2); });
    theme::setSheet(speedCadenceButton_, btnBlinkOff_);
    speedCadenceButton_->setEnabled(false);
    init->addWidget(speedCadenceButton_, 4, 1);
    selectedHrmLabel_ = selectedLabel(386, 30);
    init->addWidget(selectedHrmLabel_, 5, 0);
    auto* speedCadenceLayout = new QGridLayout;
    selectedSpeedCadenceLabel_ = selectedLabel(386, 30);
    speedCadenceLayout->addWidget(selectedSpeedCadenceLabel_, 0, 0, 1, 2);
    selectedCadenceLabel_ = selectedLabel(190, 30);
    speedCadenceLayout->addWidget(selectedCadenceLabel_, 0, 0, 1, 1);
    selectedSpeedLabel_ = selectedLabel(190, 30);
    speedCadenceLayout->addWidget(selectedSpeedLabel_, 0, 1, 1, 1);
    init->addLayout(speedCadenceLayout, 5, 1);
    initStatusDisplay_ = new QLabel;
    theme::setSheet(initStatusDisplay_, kLgText + QStringLiteral("margin-top: 30px"));
    initStatusDisplay_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    // (rich text: the warnings under the status lines are red)
    initStatusDisplay_->setTextFormat(Qt::RichText);
    initStatusLines_ = {QStringLiteral("Scanning for ANT+ devices,"), QStringLiteral("hang on for a bit...")};
    renderInitStatus();
    init->addWidget(initStatusDisplay_, 6, 0, 3, 2, Qt::AlignHCenter);
    connectButton_ = new QPushButton(QStringLiteral("Connect Selected Devices"));
    connect(connectButton_, &QPushButton::clicked, this, &SetupScreen::connectSelectedDevices);
    connectButton_->setMaximumWidth(866);
    theme::setSheet(connectButton_, btnBlinkOff_);
    connectButton_->setEnabled(false);
    init->addWidget(connectButton_, 11, 0, 1, 2);
    restartScanButton_ = new QPushButton(QStringLiteral("Disconnect and Rescan"));
    connect(restartScanButton_, &QPushButton::clicked, this, &SetupScreen::disconnectAndRescan);
    restartScanButton_->setMaximumWidth(430);
    theme::setSheet(restartScanButton_, btnBlinkOff_);
    restartScanButton_->setEnabled(false);
    init->addWidget(restartScanButton_, 12, 0, 1, 1);
    calibratePowerButton_ = new QPushButton(QStringLiteral("Calibrate Power"));
    connect(calibratePowerButton_, &QPushButton::clicked, this, &SetupScreen::calibratePower);
    calibratePowerButton_->setMaximumWidth(430);
    theme::setSheet(calibratePowerButton_, btnBlinkOff_);
    calibratePowerButton_->setEnabled(false);
    init->addWidget(calibratePowerButton_, 12, 1, 1, 1);
    // the session's rides forgotten, after a Yes; greyed out with none
    clearSessionButton_ = new QPushButton(QStringLiteral("Clear Session"));
    connect(clearSessionButton_, &QPushButton::clicked, this, &SetupScreen::confirmClearSession);
    clearSessionButton_->setMaximumWidth(866);
    theme::setSheet(clearSessionButton_, btnBlinkOff_);
    init->addWidget(clearSessionButton_, 13, 0, 1, 2);
    updateClearSessionButton();
    // the Connect/Rescan/Calibrate/Clear Session buttons at the bottom of the column; the column is pinned to the
    // center column's height (see eventFilter) so their bottoms line up
    init->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding), 9, 0, 2, 2);
    introductoryLayout_->addWidget(initializationWidget_, 0, 0, 14, 1, Qt::AlignTop);
    // (its own event filter keeps it as tall as the settings column: the layout's height fitting leaves it be)
    layout::setSkipHeight(initializationWidget_);

    // ---- The settings column ----
    centerInitWidget_ = new QWidget;
    auto* settingsLayout = new QGridLayout(centerInitWidget_);
    // a header over a box, its value and a Set button each, kept short so the mode buttons below fit
    constexpr int headerHeight = 34;
    constexpr int rowHeight = 40;
    auto metric = [&](int row, const QString& header, QLineEdit*& field, int maxLength, QLabel*& display,
                      const QString& shown, QPushButton*& set, const QString& setText, bool fieldBlinkStyle) {
        auto* headerLabel = new QLabel(header);
        headerLabel->setFixedHeight(headerHeight);
        headerLabel->setAlignment(Qt::AlignCenter);
        theme::setSheet(headerLabel, kMdText + kSelectedLabel);
        settingsLayout->addWidget(headerLabel, row, 0, 1, 3);
        field = new QLineEdit(QStringLiteral("0"));
        field->setAlignment(Qt::AlignCenter);
        theme::setSheet(field, fieldBlinkStyle ? txtBlinkOff_ : kMdText);
        field->setMaxLength(maxLength);
        field->setFixedSize(100, rowHeight);
        settingsLayout->addWidget(field, row + 1, 0, Qt::AlignCenter);
        display = new QLabel(shown);
        display->setAlignment(Qt::AlignCenter);
        theme::setSheet(display, kMdText);
        display->setMaximumSize(180, rowHeight);
        settingsLayout->addWidget(display, row + 1, 1);
        set = new QPushButton(setText);
        theme::setSheet(set, tinyBtnBlinkOff_);
        set->setFixedSize(100, rowHeight);
        settingsLayout->addWidget(set, row + 1, 2, Qt::AlignCenter);
    };
    metric(0, QStringLiteral("Wheel Circumference"), textWheelC_, 5, displayWheelC_, QStringLiteral("0m"), setWheelC_,
           QStringLiteral("Set Wheel\ncirc. (m)"), false);
    connect(textWheelC_, &QLineEdit::editingFinished, this, &SetupScreen::textWheelValidator);
    connect(setWheelC_, &QPushButton::clicked, this, &SetupScreen::setWheel);
    metric(2, QStringLiteral("FTP"), textFtp_, 3, displayFtp_, QStringLiteral("0 Watts"), setFtp_,
           QStringLiteral("Set\nnew FTP"), false);
    connect(textFtp_, &QLineEdit::editingFinished, this, &SetupScreen::textFtpValidator);
    connect(setFtp_, &QPushButton::clicked, this, &SetupScreen::setFtpValue);
    metric(4, QStringLiteral("Max HR"), textMaxHr_, 3, displayMaxHr_, QStringLiteral("0 BPM"), setMaxHr_,
           QStringLiteral("Set new\nMax HR"), false);
    connect(textMaxHr_, &QLineEdit::editingFinished, this, &SetupScreen::textMaxHrValidator);
    connect(setMaxHr_, &QPushButton::clicked, this, &SetupScreen::setMaxHrValue);
    // optional (0 is fine): with it the synopsis shows power to weight; in lbs or kg (the settings menu's
    // Measurements), converted when that changes
    metric(6, QStringLiteral("Weight (optional)"), textWeight_, 5, displayWeight_, QStringLiteral("0 lbs"), setWeight_,
           QStringLiteral("Set new\nWeight"), true);
    connect(textWeight_, &QLineEdit::editingFinished, this, &SetupScreen::textWeightValidator);
    connect(setWeight_, &QPushButton::clicked, this, &SetupScreen::setWeightValue);

    // a button a mode, top to bottom: free mode, the FTP tests, Musical Cadence Mode, workout mode
    modeButtons_ = {{new QPushButton(QStringLiteral("Free Mode")), QStringLiteral("free")},
                    {new QPushButton(QStringLiteral("20m FTP Test")), QStringLiteral("20m")},
                    {new QPushButton(QStringLiteral("1h FTP Test")), QStringLiteral("1h")},
                    {new QPushButton(QStringLiteral("10W delta FTP Ramp")), QStringLiteral("ramp10")},
                    {new QPushButton(QStringLiteral("20W delta FTP Ramp")), QStringLiteral("ramp20")},
                    {new QPushButton(QStringLiteral("Musical Cadence Mode")), QStringLiteral("cadence")},
                    {new QPushButton(QStringLiteral("Workout Mode")), QStringLiteral("workout")}};
    int modeRow = 9;
    for (const auto& [button, mode] : modeButtons_) {
        button->setProperty("awoMode", mode);
        button->setEnabled(mode != u"workout");
        theme::setSheet(button, kModeButtonStyle);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, &SetupScreen::modeClicked);
        settingsLayout->addWidget(button, modeRow++, 0, 1, 3);
    }
    workoutFileButton_ = new QPushButton(QStringLiteral("Select a workout File"));
    workoutFileButton_->setSizePolicy(retainSize());
    workoutFileButton_->setMaximumWidth(520);
    connect(workoutFileButton_, &QPushButton::clicked, this, &SetupScreen::openWorkoutDialog);
    settingsLayout->addWidget(workoutFileButton_, 16, 0, 1, 3);
    workoutFileSelected_ = new QLabel(QStringLiteral("Workout not set"));
    workoutFileSelected_->setSizePolicy(retainSize());
    workoutFileSelected_->setMaximumWidth(520);
    workoutFileSelected_->setMinimumHeight(60);
    workoutFileSelected_->setAlignment(Qt::AlignCenter);
    // (workout titles can be long)
    workoutFileSelected_->setWordWrap(true);
    theme::setSheet(workoutFileSelected_, kMdText + kSelectedLabel);
    settingsLayout->addWidget(workoutFileSelected_, 17, 0, 1, 3);
    workoutInfo_ = new QLabel;
    workoutInfo_->setMaximumWidth(520);
    workoutInfo_->setMinimumHeight(60);
    workoutInfo_->setSizePolicy(retainSize());
    workoutInfo_->setAlignment(Qt::AlignCenter);
    theme::setSheet(workoutInfo_, kMdText + kSelectedLabel);
    settingsLayout->addWidget(workoutInfo_, 18, 0, 1, 3);
    centerInitWidget_->setFixedWidth(522);
    introductoryLayout_->addWidget(centerInitWidget_, 0, 1, 14, 1, Qt::AlignTop);
    centerInitWidget_->installEventFilter(this);

    startButton_ = new QPushButton(QStringLiteral("START"));
    startButton_->setEnabled(false);
    connect(startButton_, &QPushButton::clicked, this, &SetupScreen::showHudWindow);
    startButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    introductoryLayout_->addWidget(startButton_, 15, 0, 1, 2);

    auto* workoutDisplayLayout = new QVBoxLayout;
    workoutDisplayLabel_ = new LongVLabels(state.ftpRanges);
    workoutDisplayLabel_->setFixedHeight(900);
    workoutDisplayLayout->addWidget(workoutDisplayLabel_);
    introductoryLayout_->addLayout(workoutDisplayLayout, 0, 2, 14, 1, Qt::AlignHCenter);
    workoutDisplayLabel_->hide();

    // keys for the buttons (Ctrl shows them)
    for (const auto& [button, key] :
         {std::pair{powerButton_, Qt::Key_P}, std::pair{hrmButton_, Qt::Key_H}, std::pair{speedCadenceButton_, Qt::Key_C},
          std::pair{connectButton_, Qt::Key_N}, std::pair{restartScanButton_, Qt::Key_R},
          std::pair{calibratePowerButton_, Qt::Key_L}, std::pair{clearSessionButton_, Qt::Key_X},
          std::pair{setWheelC_, Qt::Key_W}, std::pair{setFtp_, Qt::Key_F}, std::pair{setMaxHr_, Qt::Key_M},
          std::pair{setWeight_, Qt::Key_G}, std::pair{workoutFileButton_, Qt::Key_O},
          std::pair{startButton_, Qt::Key_S}}) {
        keys::add(button, key, QKeySequence(key).toString());
    }
    // the modes, top to bottom
    for (qsizetype i = 0; i < modeButtons_.size(); ++i) {
        keys::add(modeButtons_[i].first, Qt::Key_F1 + static_cast<int>(i), QStringLiteral("F%1").arg(i + 1));
    }
    // Tab goes round just the text boxes, and Enter in one presses its Set button (Max Watts has none: Enter, or
    // leaving it, applies it)
    keys::tabCycle({textMaxWatts_, textWheelC_, textFtp_, textMaxHr_, textWeight_}, textWheelC_);
    for (const auto& [field, button] : {std::pair{textWheelC_, setWheelC_}, std::pair{textFtp_, setFtp_},
                                        std::pair{textMaxHr_, setMaxHr_}, std::pair{textWeight_, setWeight_}}) {
        keys::Options options;
        options.field = field;
        keys::add(button, keys::kEnter, options);
    }

    // the saved wheel circumference, FTP and max heart rate: anything missing or out of range has to be entered,
    // each checked on its own so one bad value doesn't throw away the others
    const QJsonObject saved = settings::load();
    const QJsonValue wheel = saved.value(QStringLiteral("wheelCircumference"));
    if (wheel.isDouble() && 0.5 <= wheel.toDouble() && wheel.toDouble() < 3) {
        state.wheelCircumference = wheel.toDouble();
        wheelText_ = py::str(wheel.toDouble());
    }
    if (const auto ftp = savedInt(saved, QStringLiteral("ftp"), 20, 450, false)) {
        state.ftp = *ftp;
    }
    // the power zones, even with no FTP set (a ramp FTP test can be ridden without one)
    getFtpRanges();
    if (const auto maxHr = savedInt(saved, QStringLiteral("maxHr"), 80, 250, false)) {
        state.maxHr = *maxHr;
    }
    if (const auto maxWatts = savedInt(saved, QStringLiteral("maxWatts"), kMinMaxWatts, ant::kTargetPowerMaxWatts, true)) {
        state.maxWatts = *maxWatts;
    }
    // nothing (valid) saved yet starts at 120% of FTP
    if (const auto cutoff =
            savedInt(saved, QStringLiteral("powerDriveCutoff"), kMinPowerDriveCutoff, ant::kTargetPowerMaxWatts, true)) {
        state.powerDriveCutoff = *cutoff;
    } else {
        defaultPowerDriveCutoff();
    }
    updateMaxWattsMode();
    layout::setText(displayWheelC_, wheelText_ + u'm');
    layout::setText(displayFtp_, QString::number(state.ftp) + QStringLiteral(" Watts"));
    layout::setText(displayMaxHr_, QString::number(state.maxHr) + QStringLiteral(" BPM"));
    // the rider's weight and the unit it's shown in, again whenever they change
    Units::current().load();
    showWeight();
    connect(&Units::current(), &Units::changed, this, &SetupScreen::showWeight);

    // the bridge says the moment a device is found, connected or disconnected, or there's news; the timer is only a
    // fallback, should a signal ever be missed
    if (bridge_) {
        connect(bridge_, &ant::AntBridge::deviceDiscovered, this, [this] { initAntScanConnect(); });
        connect(bridge_, &ant::AntBridge::devicesConnected, this, &SetupScreen::initAntScanConnect);
        connect(bridge_, &ant::AntBridge::devicesDisconnected, this, &SetupScreen::initAntScanConnect);
        connect(bridge_, &ant::AntBridge::statusMessage, this, [this] { initAntScanConnect(); });
        connect(bridge_, &ant::AntBridge::errorOccurred, this, [this] { initAntScanConnect(); });
        connect(bridge_, &ant::AntBridge::batteryChanged, this, &SetupScreen::updateBatteryWarnings);
    }
    updateBatteryWarnings();
    connect(timer_, &QTimer::timeout, this, &SetupScreen::initAntScanConnect);
    timer_->setInterval(2000);
    timer_->start();
    // the blinking's own quicker timer
    connect(blinkTimer_, &QTimer::timeout, this, &SetupScreen::setupBlink);
    blinkTimer_->setInterval(300);
    blinkTimer_->start();
}

QPushButton* SetupScreen::modeButton(const QString& mode) const {
    for (const auto& [button, buttonMode] : modeButtons_) {
        if (buttonMode == mode) {
            return button;
        }
    }
    return nullptr;
}

// ---- Following the bridge ----

void SetupScreen::initAntScanConnect() {
    // (during a ride the bridge keeps signalling: the device flags the workout screen was built from stay put)
    if (stopped_ || !bridge_) {
        return;
    }
    powerButtonBlink_ = hrButtonBlink_ = spdCadButtonBlink_ = connectButtonBlink_ = false;
    wheelCTextBlink_ = wheelCButtonBlink_ = maxHrTextBlink_ = maxHrButtonBlink_ = false;
    ftpTextBlink_ = ftpButtonBlink_ = workoutSelectBlink_ = startButtonBlink_ = disconnectButtonBlink_ = false;
    QString statusString;
    pwrSelected_ = hrmSelected_ = spdCadSelected_ = spdSelected_ = cadSelected_ = false;
    pwrFound_ = hrmFound_ = spdCadFound_ = spdFound_ = cadFound_ = false;
    if (const auto error = bridge_->takeError()) {
        setInitStatus(error->trimmed());
    }
    if (bridge_->isConnectRequested()) {
        setInitStatus(QStringLiteral("Connecting...."));
        powerButton_->setEnabled(false);
        hrmButton_->setEnabled(false);
        speedCadenceButton_->setEnabled(false);
        connectButton_->setEnabled(false);
        scanning_ = false;
    }
    getFoundDevices();
    // (changes after this are handled in checkForSelectedScanned)
    getSelectedDevices();
    // the clear cases picked, and connected once every slot is
    autoSelectAndConnect();

    // the labels of devices no longer picked hidden
    if (!hrmSelected_) {
        layout::setText(selectedHrmLabel_, QString());
        selectedHrmLabel_->hide();
        if (!hrmFound_) {
            hrmButton_->setEnabled(false);
        }
    }
    if (!pwrSelected_) {
        layout::setText(selectedPowerLabel_, QString());
        selectedPowerLabel_->hide();
        showMaxWatts(false);
        if (!pwrFound_) {
            powerButton_->setEnabled(false);
        }
    }
    if (!spdCadSelected_) {
        layout::setText(selectedSpeedCadenceLabel_, QString());
        selectedSpeedCadenceLabel_->hide();
    }
    if (!cadSelected_) {
        layout::setText(selectedCadenceLabel_, QString());
        selectedCadenceLabel_->hide();
    }
    if (!spdSelected_) {
        layout::setText(selectedSpeedLabel_, QString());
        selectedSpeedLabel_->hide();
    }
    if (!spdCadSelected_ && !spdSelected_ && !cadSelected_ && !spdCadFound_ && !spdFound_ && !cadFound_) {
        speedCadenceButton_->setEnabled(false);
    }

    // the status text, and which buttons blink
    bool mismatch = false;
    QStringList selectedLines, unselectedLines;
    for (const QString& device : selectedText_) {
        if (detectedText_.contains(device)) {
            selectedLines.append(device + QStringLiteral(" - Selected/Found"));
        } else {
            selectedLines.append(device + QStringLiteral(" - Selected"));
            mismatch = true;
        }
    }
    // found devices that can still be picked (their slot not taken)
    for (const QString& device : detectedText_) {
        if (selectedText_.contains(device)) {
            continue;
        }
        const QString kind = device.split(u' ').value(0);
        if ((kind == u"Power" || kind == u"Stationary") && !pwrSelected_) {
            powerButtonBlink_ = true;
            unselectedLines.append(device);
        } else if (kind == u"Heart" && !hrmSelected_) {
            hrButtonBlink_ = true;
            unselectedLines.append(device);
        } else if (kind == u"Speed/Cadence" && !(spdCadSelected_ || cadSelected_ || spdSelected_)) {
            spdCadButtonBlink_ = true;
            unselectedLines.append(device);
        } else if (kind == u"Cadence" && !(spdCadSelected_ || cadSelected_)) {
            spdCadButtonBlink_ = true;
            unselectedLines.append(device);
        } else if (kind == u"Speed" && !(spdCadSelected_ || spdSelected_)) {
            spdCadButtonBlink_ = true;
            unselectedLines.append(device);
        }
    }
    // (only while still scanning)
    if (scanning_) {
        if (mismatch) {
            statusString += QStringLiteral("Not all selected devices have been detected yet.");
        }
        if (!selectedLines.isEmpty()) {
            statusString += u'\n' + selectedLines.join(u'\n');
        }
        if (unselectedLines.size() > 1) {
            statusString += QStringLiteral("\n\nUnselected Scanned Devices:\n") + unselectedLines.join(u'\n');
        } else if (unselectedLines.size() == 1) {
            statusString += QStringLiteral("\n\nUnselected Scanned Device:\n") + unselectedLines.front();
        }
        // no usable stick yet: say so (and that plugging one in is enough) rather than claim to be scanning
        if (const auto waiting = bridge_->waitingForStick()) {
            setInitStatus(*waiting);
        } else {
            const QString status = statusString.trimmed();
            setInitStatus(status.isEmpty() ? QStringLiteral("Scanning for ANT+ devices,\nhang on for a bit...") : status);
        }
        checkForSelectedScanned();
    }

    if (bridge_->isReady()) {
        setInitStatus(QStringLiteral("Connected Succesfully"));
        getFoundDevices();
        getSelectedDevices();
        // the devices in use, from the bridge's connection as it is (a previous workout's can't leak into this one)
        const QSet<ant::Role> roles = bridge_->selectedRoles();
        AppState& state = appState();
        state.antHRM = roles.contains(ant::Role::Hrm);
        state.antSpeedCadence = roles.contains(ant::Role::SpeedCadence);
        state.antCadence = roles.contains(ant::Role::Cadence);
        state.antSpeed = roles.contains(ant::Role::Speed);
    }
    // (calibrating needs a connected power device to talk to)
    calibratePowerButton_->setEnabled(bridge_->isReady() && bridge_->selectedDevice(ant::Role::Power).has_value());
    if (bridge_->isReady() && bridge_->isConnectRequested()) {
        restartScanButton_->setEnabled(true);
        if (hrButtonBlink_ || spdCadButtonBlink_) {
            disconnectButtonBlink_ = true;
        }
    }
    if (displayWheelC_->text() == u"0m") {
        wheelCTextBlink_ = wheelCButtonBlink_ = true;
    }
    if (displayMaxHr_->text() == u"0 BPM") {
        maxHrTextBlink_ = maxHrButtonBlink_ = true;
    }
    if (displayFtp_->text() == u"0 Watts" && !ftpOptional()) {
        ftpTextBlink_ = ftpButtonBlink_ = true;
    }
    if (workoutNeedsChoosing()) {
        workoutSelectBlink_ = true;
    }
    checkEnableStartButton();
    setupBlink();
}

void SetupScreen::setupBlink() {
    struct Blinker {
        QWidget* widget;
        bool blink;
        bool& on;
        const QString& onStyle;
        const QString& offStyle;
    };
    const QList<Blinker> blinkers = {
        {powerButton_, powerButtonBlink_, powerButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {hrmButton_, hrButtonBlink_, hrButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {speedCadenceButton_, spdCadButtonBlink_, spdCadButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {connectButton_, connectButtonBlink_, connectButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {textWheelC_, wheelCTextBlink_, wheelCTextBlinkOn_, txtBlinkOn_, txtBlinkOff_},
        {setWheelC_, wheelCButtonBlink_, wheelCButtonBlinkOn_, tinyBtnBlinkOn_, tinyBtnBlinkOff_},
        {textMaxHr_, maxHrTextBlink_, maxHrTextBlinkOn_, txtBlinkOn_, txtBlinkOff_},
        {setMaxHr_, maxHrButtonBlink_, maxHrButtonBlinkOn_, tinyBtnBlinkOn_, tinyBtnBlinkOff_},
        {textFtp_, ftpTextBlink_, ftpTextBlinkOn_, txtBlinkOn_, txtBlinkOff_},
        {setFtp_, ftpButtonBlink_, ftpButtonBlinkOn_, tinyBtnBlinkOn_, tinyBtnBlinkOff_},
        {workoutFileButton_, workoutSelectBlink_, workoutSelectBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {startButton_, startButtonBlink_, startButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
        {restartScanButton_, disconnectButtonBlink_, disconnectButtonBlinkOn_, btnBlinkOn_, btnBlinkOff_},
    };
    for (const Blinker& blinker : blinkers) {
        blinker.on = !blinker.on;
        setSheetOnce(blinker.widget, blinker.blink && blinker.on ? blinker.onStyle : blinker.offStyle);
    }
}

void SetupScreen::getFoundDevices() {
    detectedRaw_ = bridge_ ? bridge_->foundDevices() : QStringList();
    detectedText_ = makeTextDevices(detectedRaw_, false);
}

void SetupScreen::getSelectedDevices() {
    selectedRaw_ = selectedDevices::read();
    selectedText_ = makeTextDevices(selectedRaw_, true);
}

// each device's text, and what's picked and found shown and noted
QStringList SetupScreen::makeTextDevices(const QStringList& devices, bool selected) {
    QStringList texts;
    for (const QString& line : devices) {
        const QStringList parts = line.split(u' ', Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            continue;
        }
        const QString& type = parts[0];
        const QString& number = parts[1];
        QString text;
        if (type == u"120") {
            text = QStringLiteral("Heart Rate Monitor: ") + number;
            if (selected) {
                hrmSelected_ = true;
                layout::setText(selectedHrmLabel_, text);
                selectedHrmLabel_->show();
            } else {
                hrmFound_ = true;
            }
            if (scanning_) {
                hrmButton_->setEnabled(true);
            }
        } else if (type == u"11" || type == u"17") {
            text = (type == u"11" ? QStringLiteral("Power Meter: ") : QStringLiteral("Stationary Trainer: ")) + number;
            if (selected) {
                pwrSelected_ = true;
                layout::setText(selectedPowerLabel_, text);
                selectedPowerLabel_->show();
                showMaxWatts(type == u"17");
            } else {
                pwrFound_ = true;
            }
            if (scanning_) {
                powerButton_->setEnabled(true);
            }
        } else if (type == u"121" || type == u"122" || type == u"123") {
            QLabel* label = type == u"121" ? selectedSpeedCadenceLabel_
                            : type == u"122" ? selectedCadenceLabel_
                                             : selectedSpeedLabel_;
            text = (type == u"121"   ? QStringLiteral("Speed/Cadence Meter: ")
                    : type == u"122" ? QStringLiteral("Cadence Meter: ")
                                     : QStringLiteral("Speed Meter: ")) +
                   number;
            if (selected) {
                layout::setText(label, text);
                label->show();
                (type == u"121" ? spdCadSelected_ : type == u"122" ? cadSelected_ : spdSelected_) = true;
            } else {
                (type == u"121" ? spdCadFound_ : type == u"122" ? cadFound_ : spdFound_) = true;
            }
            if (scanning_) {
                speedCadenceButton_->setEnabled(true);
            }
        } else {
            continue;
        }
        texts.append(text);
    }
    return texts;
}

// A workout needs one heart rate monitor, one power source (a power meter and an FE-C trainer share the slot) and
// either a combined speed/cadence sensor or a cadence and speed pair. A slot the scan found exactly one candidate for
// has nothing to decide, so it's picked (more than one is left for the rider, and blinks). Once every slot has its
// device, found, it connects. Disconnect and Rescan clears the picks, so the rider can pick by hand instead.
void SetupScreen::autoSelectAndConnect() {
    if (!scanning_) {
        return;
    }
    // the scan given time to hear every device in range first (a slot with two isn't given whichever was heard
    // first): checked again once it has
    const double settleRemaining = bridge_->scanSettleRemaining();
    if (settleRemaining > 0) {
        if (!settleCheckPending_) {
            settleCheckPending_ = true;
            QTimer::singleShot(static_cast<int>(settleRemaining * 1000) + 50, this, &SetupScreen::settleCheck);
        }
        return;
    }
    auto candidates = [this](auto matches) {
        QStringList found;
        for (const QString& line : detectedRaw_) {
            if (matches(typeOf(line))) {
                found.append(line);
            }
        }
        return found;
    };
    const QStringList power = candidates([](const QString& t) { return isPowerType(t); });
    const QStringList hrm = candidates([](const QString& t) { return t == u"120"; });
    const QStringList combined = candidates([](const QString& t) { return t == u"121"; });
    const QStringList cadence = candidates([](const QString& t) { return t == u"122"; });
    const QStringList speed = candidates([](const QString& t) { return t == u"123"; });
    QStringList toSelect;
    if (!pwrSelected_ && power.size() == 1) {
        toSelect.append(power.front());
    }
    if (!hrmSelected_ && hrm.size() == 1) {
        toSelect.append(hrm.front());
    }
    if (!(spdCadSelected_ || cadSelected_ || spdSelected_)) {
        if (combined.size() == 1 && cadence.isEmpty() && speed.isEmpty()) {
            toSelect.append(combined.front());
        } else if (cadence.size() == 1 && speed.size() == 1 && combined.isEmpty()) {
            toSelect.append(cadence.front());
            toSelect.append(speed.front());
        }
    }
    if (!toSelect.isEmpty()) {
        selectedDevices::write(selectedDevices::read() + toSelect);
        for (const QString& line : toSelect) {
            log::event(QStringLiteral("SETUP"), QStringLiteral("Auto-selected device: %1").arg(line));
        }
        // (the rest of this check sees the new picks)
        getSelectedDevices();
    }
    if (bridge_->isConnectRequested()) {
        return;
    }
    const bool speedCadenceResolved = spdCadSelected_ || (cadSelected_ && spdSelected_);
    if (!(pwrSelected_ && hrmSelected_ && speedCadenceResolved)) {
        return;
    }
    for (const QString& line : selectedRaw_) {
        if (!detectedRaw_.contains(line)) {
            // (a picked device hasn't been heard yet)
            return;
        }
    }
    log::event(QStringLiteral("SETUP"),
               QStringLiteral("Auto-connecting - all required slots resolved: ") + selectedRaw_.join(QStringLiteral(", ")));
    bridge_->requestConnect(ant::readSelectedDevices());
}

void SetupScreen::settleCheck() {
    settleCheckPending_ = false;
    initAntScanConnect();
}

// the connect button: enabled once a picked power device has been found; blinks if nothing else does
void SetupScreen::checkForSelectedScanned() {
    getFoundDevices();
    getSelectedDevices();
    bool foundPower = false;
    for (const QString& line : selectedRaw_) {
        if (isPowerType(typeOf(line)) && detectedRaw_.contains(line)) {
            foundPower = true;
        }
    }
    connectButton_->setEnabled(foundPower);
    if (foundPower && !powerButtonBlink_ && !hrButtonBlink_ && !spdCadButtonBlink_) {
        connectButtonBlink_ = true;
    }
}

void SetupScreen::stop() {
    stopped_ = true;
    timer_->stop();
    blinkTimer_->stop();
}

void SetupScreen::resume() {
    stopped_ = false;
    timer_->start();
    blinkTimer_->start();
}

void SetupScreen::backFromRide() {
    scanning_ = true;
    checkEnableStartButton();
    updateClearSessionButton();
    restartScanButton_->setEnabled(false);
    connectButton_->setEnabled(false);
    resume();
}

// "ANT Initialized" over whatever the connection is doing, once the stick is up
void SetupScreen::setInitStatus(const QString& text) {
    QStringList lines;
    if (bridge_ && bridge_->isInitialized()) {
        lines.append(QStringLiteral("ANT Initialized"));
    }
    if (!text.isEmpty()) {
        lines.append(text.split(u'\n'));
    }
    initStatusLines_ = lines;
    renderInitStatus();
}

void SetupScreen::updateBatteryWarnings() {
    batteryWarnings_ = bridge_ ? bridge_->lowBatteryWarnings() : QStringList();
    renderInitStatus();
}

// unset settings, a rejected workout file, then low batteries, under the ANT status, in red
void SetupScreen::renderInitStatus() {
    QStringList lines;
    for (const QString& line : initStatusLines_) {
        lines.append(line.toHtmlEscaped());
    }
    QStringList warnings;
    if (!wheelCSet_) {
        warnings.append(QStringLiteral("Wheel Circ. must be set!"));
    }
    if (!ftpSet_ && !ftpOptional()) {
        warnings.append(QStringLiteral("FTP must be set!"));
    }
    if (!maxHrSet_) {
        warnings.append(QStringLiteral("Max HR must be set!"));
    }
    warnings += workoutFileError_ + batteryWarnings_;
    for (const QString& warning : warnings) {
        lines.append(QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(kGaugeRed, warning.toHtmlEscaped()));
    }
    layout::setText(initStatusDisplay_, lines.join(QStringLiteral("<br>")));
}

// the details (where, and the line) in the workout info box, and two lines under the ANT status: the file, then
// Error, line <#>: "<reason>"
void SetupScreen::rejectWorkout(const QString& filename, const QString& reason, int line, const QString& text) {
    const QString location = line ? QStringLiteral("file;line\n%1;%2").arg(filename).arg(line)
                                  : QStringLiteral("file\n%1").arg(filename);
    QString details = QStringLiteral("Invalid workout: %1\n%2").arg(location, reason);
    if (!text.isNull()) {
        details += u'\n' + text;
    }
    layout::setText(workoutInfo_, details);
    const QString where = line ? QStringLiteral("Error, line %1").arg(line) : QStringLiteral("Error");
    workoutFileError_ = {filename, QStringLiteral("%1: \"%2\"").arg(where, reason.simplified())};
    renderInitStatus();
}

// ---- The buttons ----

void SetupScreen::openPopup(QWidget* popup) {
    delete popup_;
    popup_ = popup;
    popup->show();
}

void SetupScreen::calibratePower() {
    openPopup(new CalibratePowerWindow(bridge_));
}

void SetupScreen::updateClearSessionButton() {
    clearSessionButton_->setEnabled(!session::rides().isEmpty());
}

void SetupScreen::confirmClearSession() {
    openPopup(new ConfirmClearSessionWindow([this] { clearSession(); }));
}

void SetupScreen::clearSession() {
    log::event(QStringLiteral("SETUP"), QStringLiteral("User cleared the session (%1 rides)").arg(session::rides().size()));
    session::clear();
    updateClearSessionButton();
}

// kind 0: power, 1: heart rate, 2: speed/cadence
void SetupScreen::selectDevices(int kind) {
    auto ofKind = [kind](const QString& line) {
        const QString type = typeOf(line);
        return kind == 0 ? isPowerType(type) : kind == 1 ? type == u"120" : (type == u"121" || type == u"122" || type == u"123");
    };
    QStringList devices;
    for (const QStringList* list : {&detectedRaw_, &selectedRaw_}) {
        for (const QString& line : *list) {
            if (ofKind(line)) {
                devices.append(line);
            }
        }
    }
    const auto windowKind = kind == 0   ? SelectDevicesWindow::Kind::Power
                            : kind == 1 ? SelectDevicesWindow::Kind::HeartRate
                                        : SelectDevicesWindow::Kind::SpeedCadence;
    openPopup(new SelectDevicesWindow(devices, windowKind, bridge_ ? bridge_->foundDevices() : QStringList()));
}

void SetupScreen::connectSelectedDevices() {
    auto* window = new ConnectDevicesWindow(bridge_);
    window->move(20, 20);
    openPopup(window);
}

void SetupScreen::disconnectAndRescan() {
    log::event(QStringLiteral("SETUP"), QStringLiteral("User requested Disconnect and Rescan"));
    AppState& state = appState();
    state.antHRM = state.antSpeedCadence = state.antCadence = state.antSpeed = false;
    connectButton_->setEnabled(false);
    restartScanButton_->setEnabled(false);
    calibratePowerButton_->setEnabled(false);
    bridge_->requestDisconnect();
    // the picks cleared too, so the next scan starts afresh (picked by hand, or automatically) rather than
    // connecting straight back to what was just disconnected
    selectedDevices::clear();
    scanning_ = true;
}

// ---- The settings ----

// the power zones (with no FTP set, worked out from the FTP the zones use then)
void SetupScreen::getFtpRanges() {
    appState().ftpRanges = zones::powerRanges();
}

void SetupScreen::textWheelValidator() {
    const auto value = py::toFloat(textWheelC_->text());
    if (value && 0.5 <= *value && *value < 3) {
        return;
    }
    textWheelC_->setText(wheelText_);
}

void SetupScreen::textFtpValidator() {
    if (py::isDigits(textFtp_->text())) {
        const int ftp = textFtp_->text().toInt();
        if (20 <= ftp && ftp < 450) {
            return;
        }
    }
    textFtp_->setText(QString::number(appState().ftp));
}

void SetupScreen::textMaxHrValidator() {
    if (py::isDigits(textMaxHr_->text())) {
        const int maxHr = textMaxHr_->text().toInt();
        if (80 <= maxHr && maxHr < 250) {
            return;
        }
    }
    textMaxHr_->setText(QString::number(appState().maxHr));
}

// 0 (not set) or a reasonable weight in the weight unit
void SetupScreen::textWeightValidator() {
    if (const auto weight = py::toFloat(textWeight_->text())) {
        const auto [low, high] = Units::current().weightRange();
        if (*weight == 0 || (low <= *weight && *weight <= high)) {
            return;
        }
    }
    textWeight_->setText(Units::current().formatWeight(Units::current().riderWeightKg));
}

// FE-C Max Watts (free mode) or Power Drive Cutoff (workout mode), taken as soon as it's entered; anything invalid
// goes back to the last value
void SetupScreen::textMaxWattsValidator() {
    const QString text = textMaxWatts_->text();
    AppState& state = appState();
    const bool digits = py::isDigits(text);
    const int watts = digits ? text.toInt() : 0;
    if (state.workoutMode) {
        if (digits && kMinPowerDriveCutoff <= watts && watts <= ant::kTargetPowerMaxWatts) {
            state.powerDriveCutoff = watts;
            settings::update({{QStringLiteral("powerDriveCutoff"), state.powerDriveCutoff}});
        }
    } else if (digits && kMinMaxWatts <= watts && watts <= ant::kTargetPowerMaxWatts) {
        state.maxWatts = watts;
        settings::update({{QStringLiteral("maxWatts"), state.maxWatts}});
    }
    updateMaxWattsMode();
}

void SetupScreen::updateMaxWattsMode() {
    const AppState& state = appState();
    if (state.workoutMode) {
        layout::setText(maxWattsLabel_, QStringLiteral("Power Drive Cutoff:"));
        textMaxWatts_->setText(QString::number(state.powerDriveCutoff));
    } else {
        layout::setText(maxWattsLabel_, QStringLiteral("Max Watts:"));
        textMaxWatts_->setText(QString::number(state.maxWatts));
    }
}

// Power Drive Cutoff back to 120% of the FTP (save: a new FTP, so the saved one follows)
void SetupScreen::defaultPowerDriveCutoff(bool save) {
    AppState& state = appState();
    if (state.ftp != 0) {
        state.powerDriveCutoff =
            std::min(static_cast<int>(py::round(state.ftp * kPowerDriveCutoffFtp)), ant::kTargetPowerMaxWatts);
        if (save) {
            settings::update({{QStringLiteral("powerDriveCutoff"), state.powerDriveCutoff}});
        }
    }
}

// the Max Watts box only while the power device picked is an FE-C trainer
void SetupScreen::showMaxWatts(bool show) {
    maxWattsLabel_->setVisible(show);
    textMaxWatts_->setVisible(show);
}

// (each checks its box first: pressing Set, by click or key, leaves the box focused so it hasn't been checked yet)
void SetupScreen::setWheel() {
    textWheelValidator();
    if (textWheelC_->text() != u"0") {
        wheelText_ = textWheelC_->text();
        layout::setText(displayWheelC_, wheelText_ + u'm');
        appState().wheelCircumference = py::toFloat(wheelText_).value_or(0);
        settings::update({{QStringLiteral("wheelCircumference"), appState().wheelCircumference}});
        reloadWorkout();
    }
}

void SetupScreen::setFtpValue() {
    textFtpValidator();
    if (textFtp_->text() != u"0") {
        layout::setText(displayFtp_, textFtp_->text() + QStringLiteral(" Watts"));
        appState().ftp = textFtp_->text().toInt();
        getFtpRanges();
        settings::update({{QStringLiteral("ftp"), appState().ftp}});
        defaultPowerDriveCutoff(true);
        updateMaxWattsMode();
        reloadWorkout();
    }
}

void SetupScreen::setMaxHrValue() {
    textMaxHrValidator();
    if (textMaxHr_->text() != u"0") {
        layout::setText(displayMaxHr_, textMaxHr_->text() + QStringLiteral(" BPM"));
        appState().maxHr = textMaxHr_->text().toInt();
        settings::update({{QStringLiteral("maxHr"), appState().maxHr}});
        reloadWorkout();
    }
}

// (0 clears it), entered in the weight unit and kept in kg
void SetupScreen::setWeightValue() {
    textWeightValidator();
    Units& units = Units::current();
    units.setRiderWeightKg(units.weightToKg(py::toFloat(textWeight_->text()).value_or(0)));
}

// the weight in the unit it's shown in (converted when that changes)
void SetupScreen::showWeight() {
    const Units& units = Units::current();
    layout::setText(displayWeight_, units.weightText());
    textWeight_->setText(units.formatWeight(units.riderWeightKg));
}

void SetupScreen::applyFtp(int ftp) {
    textFtp_->setText(QString::number(ftp));
    setFtpValue();
}

void SetupScreen::applyMaxHr(int maxHr) {
    textMaxHr_->setText(QString::number(maxHr));
    setMaxHrValue();
}

// the workout again after a setting it was built from changed: the FTP test, else the workout file if there is one
void SetupScreen::reloadWorkout() {
    const AppState& state = appState();
    if (state.cadenceMode) {
        // the cadence zones again (a new FTP just put the power zones back)
        applyCadenceRanges();
    } else if (!state.ftpTestMode.isEmpty()) {
        buildFtpTest();
    } else if (!state.workoutFilename.isEmpty()) {
        loadWorkoutFile(state.workoutFilename);
    }
}

// ---- The mode ----

void SetupScreen::modeClicked() {
    chooseMode(sender()->property("awoMode").toString());
}

void SetupScreen::chooseMode(const QString& mode) {
    for (const auto& [button, buttonMode] : modeButtons_) {
        button->setEnabled(buttonMode != mode);
    }
    AppState& state = appState();
    // the FTP tests are workouts too, built here rather than read from a file
    state.workoutMode = mode != u"free";
    state.ftpTestMode = ftptest::find(mode) ? mode : QString();
    const bool wasCadenceMode = state.cadenceMode;
    state.cadenceMode = mode == u"cadence";
    // with no FTP set, the zones' colors depend on the mode; leaving Musical Cadence Mode, its cadence zones go back
    // to power zones
    if (state.ftp == 0 || wasCadenceMode) {
        getFtpRanges();
    }
    updateMaxWattsMode();
    layout::setText(workoutFileButton_,
                    state.cadenceMode ? QStringLiteral("Select Music") : QStringLiteral("Select a workout File"));
    if (state.cadenceMode) {
        workoutFileButton_->show();
        workoutFileSelected_->show();
        workoutInfo_->show();
        buildMusicWorkout();
    } else if (!state.ftpTestMode.isEmpty()) {
        workoutFileButton_->hide();
        workoutFileSelected_->show();
        workoutInfo_->show();
        buildFtpTest();
    } else if (state.workoutMode) {
        workoutFileButton_->show();
        workoutFileSelected_->show();
        workoutInfo_->show();
        // (back from an FTP test, the workout file's workout replaces the test's)
        if (!state.workoutFilename.isEmpty() && state.ftp != 0) {
            loadWorkoutFile(state.workoutFilename);
            workoutDisplayLabel_->show();
        } else {
            clearWorkout();
            layout::setText(workoutInfo_, QString());
            workoutDisplayLabel_->setFullText(QString());
            workoutDisplayLabel_->hide();
        }
        layout::setText(workoutFileSelected_, state.workoutFilename.isEmpty()
                                                  ? QStringLiteral("Workout not set")
                                                  : u' ' + state.workoutDisplayName() + u' ');
    } else {
        workoutFileButton_->hide();
        workoutFileSelected_->hide();
        workoutInfo_->hide();
        workoutDisplayLabel_->hide();
    }
    // what depends on the mode looked at again now, not at the next check
    workoutSelectBlink_ = workoutNeedsChoosing();
    checkEnableStartButton();
    setupBlink();
}

// a ramp FTP test can be ridden with no FTP set (its warmup is a free ride then), and so can Musical Cadence Mode
// (its targets are cadences)
bool SetupScreen::ftpOptional() const {
    const AppState& state = appState();
    return (!state.ftpTestMode.isEmpty() && ftptest::isRamp(state.ftpTestMode)) ||
           (state.workoutMode && state.cadenceMode);
}

// a workout mode that still needs its workout file (or music) picked
bool SetupScreen::workoutNeedsChoosing() const {
    const AppState& state = appState();
    if (!state.workoutMode || !state.ftpTestMode.isEmpty()) {
        return false;
    }
    if (state.cadenceMode) {
        return state.workoutList.isEmpty();
    }
    return state.workoutFilenameShort.isEmpty();
}

// the workout to ride is ready: a workout file's, an FTP test's or Musical Cadence Mode's
bool SetupScreen::workoutReady() const {
    const AppState& state = appState();
    if (!state.ftpTestMode.isEmpty() || state.cadenceMode) {
        return !state.workoutList.isEmpty();
    }
    return !state.workoutFilename.isEmpty();
}

// ---- The workout ----

// the FTP test picked as the workout: its blocks read like a workout file's, plus its messages
void SetupScreen::buildFtpTest() {
    AppState& state = appState();
    const ftptest::Test* test = ftptest::find(state.ftpTestMode);
    if (state.ftp == 0 && !ftpOptional()) {
        // (built once the FTP is set)
        clearWorkout();
        layout::setText(workoutInfo_, QStringLiteral("Set your FTP first"));
        workoutDisplayLabel_->setFullText(QString());
        workoutDisplayLabel_->hide();
        layout::setText(workoutFileSelected_, u' ' + test->name + u' ');
        return;
    }
    AwoOptions options;
    options.ftp = state.ftp;
    // an effort test's effort is one free ride block, which can be longer than a workout file's blocks
    options.maxBlockSeconds = std::max(kMaxBlockSeconds, test->effortSeconds);
    const AwoResult result = parseAwoLines(ftptest::workoutLines(state.ftpTestMode, state.ftp != 0), options);
    const bool ramp = ftptest::isRamp(state.ftpTestMode);
    if (!useWorkout(test->name, result, ramp)) {
        return;
    }
    int effortStart = 0;
    if (ramp) {
        // the ramp's first block; the workout screen adds the rest as they're ridden
        effortStart = state.workoutDurationSeconds;
        const WorkoutEntry firstBlock = ftptest::rampBlock(state.ftpTestMode, 0);
        state.workoutList.append(firstBlock);
        state.workoutDurationSeconds += firstBlock.block.seconds;
        layout::setText(workoutInfo_, QStringLiteral("Warmup + ramp until you stop pedaling"));
        // (a "more..." block under the first step: the ramp keeps going until the rider stops)
        showWorkoutText(true);
    } else {
        effortStart = state.workoutDurationSeconds - test->effortSeconds;
    }
    state.workoutMessages += ftptest::messages(state.ftpTestMode, effortStart, state.ftp);
    layout::setText(workoutFileSelected_, u' ' + test->name + u' ');
    workoutDisplayLabel_->show();
}

void SetupScreen::openWorkoutDialog() {
    if (appState().cadenceMode) {
        openMusicDialog();
        return;
    }
    WorkoutSelectDialog dialog(this);
    const QString path = dialog.exec() ? dialog.selectedPath() : QString();
    if (!path.isEmpty()) {
        pickWorkoutFile(path);
    }
}

void SetupScreen::pickWorkoutFile(const QString& path) {
    AppState& state = appState();
    if (loadWorkoutFile(path)) {
        workoutDisplayLabel_->show();
        state.workoutFilename = path;
        state.workoutFilenameShort = path.split(u'/').last();
        layout::setText(workoutFileSelected_, u' ' + state.workoutDisplayName() + u' ');
    } else {
        state.workoutFilename.clear();
        state.workoutFilenameShort.clear();
        state.workoutTitle.clear();
        state.workoutMessages.clear();
        layout::setText(workoutFileSelected_, QString());
        workoutDisplayLabel_->setFullText(QString());
    }
    // (on failure or success)
    workoutSelectBlink_ = state.workoutFilenameShort.isEmpty();
    checkEnableStartButton();
    setupBlink();
}

// ---- Musical Cadence Mode (see music/) ----

void SetupScreen::setMusicSelection(const std::optional<MusicSelection>& selection) {
    musicSelection_ = selection;
    buildMusicWorkout();
    workoutSelectBlink_ = workoutNeedsChoosing();
    checkEnableStartButton();
    setupBlink();
}

void SetupScreen::openMusicDialog() {
    MusicSelectDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        musicSelection_ = dialog.selection();
        buildMusicWorkout();
        workoutSelectBlink_ = workoutNeedsChoosing();
        checkEnableStartButton();
        setupBlink();
    }
}

// last time's checked songs, if every one of them is still analyzed in the cache (the ride can be built right away,
// without the song picker); none otherwise
std::optional<MusicSelection> SetupScreen::savedMusicSelection() {
    const music::Cache cache = music::loadCache();
    MusicSelection selection;
    bool complete = true;
    for (const music::SelectedSong& song : music::loadSelection()) {
        if (!song.checked) {
            continue;
        }
        const std::optional<music::Analysis> analysis =
            QFileInfo(song.path).isFile() ? music::cached(cache, song.path) : std::nullopt;
        if (!analysis) {
            complete = false;
            break;
        }
        selection.songs.append({song.path, song.title});
        selection.analyses.insert(song.path, *analysis);
    }
    // (a song looked up for the first time has its file read for its CRC: kept, so that's only ever once)
    music::saveNewCrcs(cache);
    if (!complete || selection.songs.isEmpty()) {
        return std::nullopt;
    }
    const QJsonObject saved = settings::load();
    selection.preferred = saved.value(QStringLiteral("musicPreferredRpm")).toInt(static_cast<int>(music::kDefaultPreferredRpm));
    selection.minSegment = saved.value(QStringLiteral("musicMinSegment")).toInt(static_cast<int>(music::kDefaultMinSegmentSeconds));
    return selection;
}

// The music's cadence profile as the workout: a steady block a cadence (the RPM where a workout file's blocks have
// watts, the same at every difficulty), free ride where there's no clear beat, and each song's title as a timed
// message as it starts
void SetupScreen::buildMusicWorkout() {
    clearWorkout();
    AppState& state = appState();
    state.musicTracks.clear();
    state.musicSongStarts.clear();
    state.musicPulses.clear();
    if (!musicSelection_ && !musicBuilder_) {
        musicSelection_ = savedMusicSelection();
    }
    if (!musicSelection_ && !musicBuilder_) {
        layout::setText(workoutFileSelected_, QStringLiteral("Music not picked"));
        layout::setText(workoutInfo_, QString());
        workoutDisplayLabel_->setFullText(QString());
        workoutDisplayLabel_->hide();
        return;
    }
    const MusicSelection selection = musicSelection_.value_or(MusicSelection{});
    const music::CadenceRide ride =
        musicBuilder_ ? musicBuilder_(selection)
                      : music::buildWorkout(selection.songs, selection.analyses, selection.preferred, selection.minSegment);
    music::applyRide(ride);
    const auto songCount = ride.tracks.size();
    applyCadenceRanges();
    layout::setText(workoutFileSelected_, QStringLiteral(" Musical Cadence: %1 song%2 ")
                                              .arg(songCount)
                                              .arg(songCount != 1 ? QStringLiteral("s") : QString()));
    layout::setText(workoutInfo_, QStringLiteral("Ride Duration: ") + minimizedDuration(state.workoutDurationSeconds));
    showWorkoutText();
    workoutDisplayLabel_->show();
}

// the zone limits (which color the blocks, graph, gauge arc and band): zone "0" up to the cadence scale's bottom, red
// from its top, the rest evenly between
void SetupScreen::applyCadenceRanges() {
    appState().ftpRanges = ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm);
}

void SetupScreen::clearWorkout() {
    AppState& state = appState();
    state.workoutDurationSeconds = 0;
    state.workoutList.clear();
    state.workoutTitle.clear();
    state.workoutMessages.clear();
}

bool SetupScreen::loadWorkoutFile(const QString& path) {
    clearWorkout();
    AwoOptions options;
    options.ftp = appState().ftp;
    const AwoResult result = readWorkoutFile(path, options);
    const QString filename = path.split(u'/').last();
    // (a Zwift workout's conversion notes)
    for (const QString& warning : result.warnings) {
        log::event(QStringLiteral("SETUP"), QStringLiteral("%1: %2").arg(filename, warning));
    }
    return useWorkout(filename, result);
}

// a workout read (from a file, or an FTP test's lines) as the app's workout; filename is what an error is reported
// against
bool SetupScreen::useWorkout(const QString& filename, const AwoResult& result, bool more) {
    clearWorkout();
    workoutDisplayLabel_->ftpRanges = appState().ftpRanges;
    if (!result.ok()) {
        const AwoError& error = result.errors.front();
        rejectWorkout(filename, error.reason, error.line, error.line ? error.text : QString());
        return false;
    }
    AppState& state = appState();
    state.workoutList = result.workout.entries;
    state.workoutMessages = result.workout.messages;
    state.workoutTitle = result.workout.title;
    state.workoutDurationSeconds = result.workout.seconds;
    layout::setText(workoutInfo_, QStringLiteral("Workout Duration: ") + minimizedDuration(state.workoutDurationSeconds));
    if (!workoutFileError_.isEmpty()) {
        workoutFileError_.clear();
        renderInitStatus();
    }
    if (!more) {
        showWorkoutText();
    }
    return true;
}

// the workout's blocks, listed beside the settings column
void SetupScreen::showWorkoutText(bool more) {
    const AppState& state = appState();
    QString text = workoutText(state.workoutList, state.targetUnit(),
                               state.cadenceMode ? state.musicSongStarts : QMap<int, QString>());
    if (more) {
        text += QStringLiteral("more...\n");
    }
    introductoryLayout_->addWidget(startButton_, 15, 0, 1, 3);
    workoutDisplayLabel_->setFullText(text);
    introductoryLayout_->setAlignment(centerInitWidget_, Qt::AlignTop | Qt::AlignHCenter);
}

// keeps the devices column exactly as tall as the settings column, so the bottom buttons line up
bool SetupScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == centerInitWidget_ && event->type() == QEvent::Resize) {
        initializationWidget_->setFixedHeight(centerInitWidget_->height());
    }
    return QWidget::eventFilter(watched, event);
}

void SetupScreen::useSettings(double wheelCircumference, int ftp, int maxHr) {
    AppState& state = appState();
    if (state.wheelCircumference == 0) {
        state.wheelCircumference = wheelCircumference;
        wheelText_ = py::str(wheelCircumference);
    }
    if (state.ftp == 0) {
        state.ftp = ftp;
    }
    if (state.maxHr == 0) {
        state.maxHr = maxHr;
    }
    getFtpRanges();
    // (none saved, and no saved FTP to default it from)
    if (state.powerDriveCutoff == 0) {
        defaultPowerDriveCutoff();
    }
    updateMaxWattsMode();
    layout::setText(displayWheelC_, wheelText_ + u'm');
    layout::setText(displayFtp_, QString::number(state.ftp) + QStringLiteral(" Watts"));
    layout::setText(displayMaxHr_, QString::number(state.maxHr) + QStringLiteral(" BPM"));
}

void SetupScreen::settleBlink() {
    for (bool* on : {&powerButtonBlinkOn_, &hrButtonBlinkOn_, &spdCadButtonBlinkOn_, &connectButtonBlinkOn_,
                     &wheelCTextBlinkOn_, &wheelCButtonBlinkOn_, &maxHrTextBlinkOn_, &maxHrButtonBlinkOn_,
                     &ftpTextBlinkOn_, &ftpButtonBlinkOn_, &workoutSelectBlinkOn_, &startButtonBlinkOn_,
                     &disconnectButtonBlinkOn_}) {
        *on = true;
    }
    // (setupBlink turns each over: off)
    setupBlink();
}

void SetupScreen::checkEnableStartButton() {
    const AppState& state = appState();
    if (state.wheelCircumference != 0) {
        wheelCSet_ = true;
    }
    if (state.ftp != 0) {
        ftpSet_ = true;
    }
    if (state.maxHr != 0) {
        maxHrSet_ = true;
    }
    renderInitStatus();
    const bool mainRequirements =
        wheelCSet_ && (ftpSet_ || ftpOptional()) && maxHrSet_ && bridge_ && bridge_->isReady();
    const bool ready = mainRequirements && (!state.workoutMode || workoutReady());
    startButton_->setEnabled(ready);
    if (ready) {
        if (!(hrButtonBlink_ || spdCadButtonBlink_)) {
            startButtonBlink_ = true;
        }
    } else {
        startButtonBlink_ = false;
    }
}

void SetupScreen::showHudWindow() {
    // (a stale enabled START, e.g. Workout Mode with no workout loaded)
    if (appState().workoutMode && !workoutReady()) {
        checkEnableStartButton();
        return;
    }
    hide();
    stop();
    appState().startTimestamp = QDateTime::currentDateTime();
    // a new ride: the FTP and zones it's drawn with in the session from here
    session::beginRide();
    emit startRequested();
}

}  // namespace awo
