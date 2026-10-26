#include "ViewerControls.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

#include "AnalogGauge.hpp"
#include "AppLayout.hpp"
#include "AppStyle.hpp"
#include "CustomTitleBar.hpp"
#include "FtpColorsEditor.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"
#include "ViewerDisplay.hpp"

namespace awo::viewer {

const QList<ScreenSize> kScreenSizes = {
    {std::nullopt, QStringLiteral("This screen (maximized)")},
    {QSize(1280, 720), QStringLiteral("13–14\" FHD laptop at 150%")},
    {QSize(1280, 800), QStringLiteral("13–14\" 1920×1200 laptop at 150%")},
    {QSize(1366, 768), QStringLiteral("Older budget laptop")},
    {QSize(1440, 900), QStringLiteral("2880×1800 laptop at 200%")},
    {QSize(1536, 864), QStringLiteral("15\" FHD laptop at 125% (most common laptop)")},
    {QSize(1600, 900), QStringLiteral("1600×900 laptop")},
    {QSize(1707, 1067), QStringLiteral("2560×1600 laptop at 150%")},
    {QSize(1920, 1080), QStringLiteral("FHD monitor / 4K TV at 200%")},
    {QSize(1920, 1200), QStringLiteral("The app's design size")},
    {QSize(2560, 1440), QStringLiteral("QHD monitor / 4K at 150%")},
    {QSize(3840, 2160), QStringLiteral("4K at 100%")},
};
const QString kScreenshotDir = QStringLiteral("layoutScreenshots");
const QString kExampleMessage = QStringLiteral("Stand up for the last 30 seconds and keep your cadence above 90!");

namespace {

const QString kDefaultWorkout = QStringLiteral("0_Little_bit_of_everything.awo");
const QString kWorkoutDir = QStringLiteral("awoWorkouts");
// the font rows, in the order shown (a function: the roles are another library's globals, which may not be made
// yet while this file's are)
QStringList fontRoles() {
    return {theme::kRoleMain, theme::kRoleButton, theme::kRoleGauge};
}

QString percent(double scale) {
    return QString::number(static_cast<long long>(py::round(scale * 100)));
}

// greys first (no hue), then around the color wheel, dark to light
std::pair<int, int> hue(const QString& color) {
    const QColor c(color);
    return {c.hslHue(), c.lightness()};
}

QList<std::pair<int, int>> hues(const Gradient& gradient) {
    QList<std::pair<int, int>> out;
    for (const QString& color : gradient) {
        out.append(hue(color));
    }
    return out;
}

template <typename T>
bool isA(const QWidget* widget) {
    return dynamic_cast<const T*>(widget) != nullptr;
}

QString typeName(const QWidget* widget) {
    QString name = QString::fromLatin1(widget->metaObject()->className());
    const qsizetype colons = name.lastIndexOf(QStringLiteral("::"));
    return colons >= 0 ? name.mid(colons + 2) : name;
}

}  // namespace

ControlWindow::ControlWindow(DisplayWindow* display, const QStringList& families)
    : display_(display),
      preview_(new PreviewWindow),
      homeScreen_(display->screen()),
      monitorTimer_(new QTimer(this)),
      restyleTimer_(new QTimer(this)) {
    // (see displayScreenChanged)
    monitorTimer_->setSingleShot(true);
    monitorTimer_->setInterval(500);
    connect(monitorTimer_, &QTimer::timeout, this, &ControlWindow::monitorChanged);
    // the layout scale never applies to the controls or the preview
    theme::setExempt(this);
    theme::setExempt(preview_);
    setWindowTitle(QStringLiteral("Layout Viewer Controls"));
    // the app wide font is the theme's; the controls keep the plain system font
    setFont(theme::defaultFont());
    // Qt 6.7's windows11 style draws disabled text the same black as enabled, so grey it here
    QPalette palette = this->palette();
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, QColor(QStringLiteral("#a0a0a0")));
    }
    setPalette(palette);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    restyleTimer_->setSingleShot(true);
    restyleTimer_->setInterval(60);
    connect(restyleTimer_, &QTimer::timeout, this, &ControlWindow::applyTheme);

    auto* outer = new QVBoxLayout(this);

    // ---- View ----
    auto* viewBox = new QGroupBox(QStringLiteral("View  (keys 1 – 5)"));
    auto* viewLayout = new QHBoxLayout(viewBox);
    viewButtons_ = new QButtonGroup(this);
    for (int i = 0; i < kViews.size(); ++i) {
        const QString name = kViews[i];
        auto* radio = new QRadioButton(name);
        radio->setChecked(name == display->current());
        viewButtons_->addButton(radio, i);
        viewLayout->addWidget(radio);
        for (QWidget* target : {static_cast<QWidget*>(this), static_cast<QWidget*>(display), static_cast<QWidget*>(preview_)}) {
            auto* shortcut = new QShortcut(QKeySequence(QString::number(i + 1)), target);
            connect(shortcut, &QShortcut::activated, this, [this, name] { display_->showView(name); });
        }
    }
    connect(viewButtons_, &QButtonGroup::idClicked, this, [this](int id) { display_->showView(kViews[id]); });
    playButton_ = new QPushButton(QStringLiteral("▶ Play"));
    playButton_->setCheckable(true);
    playButton_->setToolTip(QStringLiteral("Runs the Workout view live from where it is (pauses on another view or a rebuild)"));
    connect(playButton_, &QPushButton::toggled, this, [this](bool on) {
        if (on) {
            display_->play();
        } else {
            display_->pause();
        }
    });
    connect(display, &DisplayWindow::playingChanged, this, &ControlWindow::playingChanged);
    viewLayout->addWidget(playButton_);
    connect(display, &DisplayWindow::viewChanged, this, &ControlWindow::viewChanged);
    connect(display, &DisplayWindow::resized, this,
            [this] { QTimer::singleShot(100, this, &ControlWindow::updateFitReport); });
    outer->addWidget(viewBox);

    // ---- The fit report, always in view ----
    fitLabel_ = new QLabel;
    fitLabel_->setTextFormat(Qt::RichText);
    fitLabel_->setWordWrap(true);
    outer->addWidget(fitLabel_);

    tabs_ = new QTabWidget;
    outer->addWidget(tabs_, 1);
    tabs_->addTab(buildScreenTab(), QStringLiteral("Screen"));
    fontsTab_ = buildFontsTab(families);
    tabs_->addTab(fontsTab_, QStringLiteral("Fonts"));
    colorsTab_ = buildColorsTab();
    tabs_->addTab(colorsTab_, QStringLiteral("Colors"));
    tabs_->addTab(buildScenarioTab(), QStringLiteral("Scenario"));
    // the power zone colors: edited here, shown on their own mockup window while the tab is open
    ftpTab_ = new FtpColorsTab;
    ftpPage_ = new QWidget;
    auto* ftpLayout = new QVBoxLayout(ftpPage_);
    ftpLayout->setContentsMargins(0, 0, 0, 0);
    ftpLayout->addLayout(partButtons(QStringLiteral("ftpColors"), QStringLiteral("gradients")));
    ftpLayout->addWidget(ftpTab_, 1);
    tabs_->addTab(ftpPage_, QStringLiteral("FTP Colors"));
    connect(tabs_, &QTabWidget::currentChanged, this, &ControlWindow::tabChanged);

    // ---- The theme file: fonts and colors, everything the app loads from its ⚙ menu ----
    auto* buttons = new QHBoxLayout;
    const struct {
        QString text;
        std::function<void()> slot;
        QString tip;
    } fileButtons[] = {
        {QStringLiteral("Save theme…"), [this] { saveTheme(); },
         QStringLiteral("Fonts, colors and power/cadence gradients, for the app's ⚙ menu")},
        {QStringLiteral("Save as default"), [this] { saveDefaultTheme(); },
         QStringLiteral("Save as %1, which the app uses whenever no theme is picked").arg(theme::kDefaultThemeFile)},
        {QStringLiteral("Load theme…"), [this] { loadTheme(); }, QString()},
        {QStringLiteral("Reset all"), [this] { resetTheme(); }, QStringLiteral("Back to the app's own fonts and colors")},
    };
    for (const auto& [text, slot, tip] : fileButtons) {
        auto* button = new QPushButton(text);
        connect(button, &QPushButton::clicked, this, slot);
        button->setToolTip(tip);
        buttons->addWidget(button);
    }
    outer->addLayout(buttons);

    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(true);
    outer->addWidget(statusLabel_);

    syncFontControls();
    // on the Fonts and Colors tabs, clicking the display identifies what's clicked (see eventFilter)
    qApp->installEventFilter(this);
}

ControlWindow::~ControlWindow() {
    qApp->removeEventFilter(this);
    delete ftpMockup_;
    delete preview_;
}

// ---- Tabs ----

void ControlWindow::tabChanged(int index) {
    if (tabs_->widget(index) == ftpPage_) {
        if (!ftpMockup_) {
            ftpMockup_ = new FtpMockupWindow;
            ftpMockup_->resize(1000, 1000);
            // beside the controls, on their screen
            const QRect screen = this->screen()->availableGeometry();
            int x = frameGeometry().right() + 10;
            if (x + 1000 > screen.right()) {
                x = std::max(screen.left(), frameGeometry().left() - 1010);
            }
            ftpMockup_->move(x, screen.top() + 30);
        }
        ftpMockup_->show();
        ftpMockup_->raise();
    } else if (ftpMockup_) {
        ftpMockup_->hide();
    }
}

QWidget* ControlWindow::buildScreenTab() {
    auto* tab = new QWidget;
    auto* form = new QFormLayout(tab);
    screenCombo_ = new QComboBox;
    for (const ScreenSize& screen : kScreenSizes) {
        if (screen.size) {
            screenCombo_->addItem(QStringLiteral("%1×%2  –  %3").arg(screen.size->width()).arg(screen.size->height()).arg(screen.note),
                                  QStringLiteral("%1x%2").arg(screen.size->width()).arg(screen.size->height()));
        } else {
            screenCombo_->addItem(screen.note, QStringLiteral("screen"));
        }
    }
    screenCombo_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    form->addRow(QStringLiteral("Screen"), screenCombo_);
    // in its own widget so the whole row (label included) can be greyed out when Custom isn't picked
    auto* customWidget = new QWidget;
    auto* customRow = new QHBoxLayout(customWidget);
    customRow->setContentsMargins(0, 0, 0, 0);
    customW_ = new QSpinBox;
    customH_ = new QSpinBox;
    for (const auto& [spin, value] : {std::pair{customW_, 1280}, std::pair{customH_, 720}}) {
        spin->setRange(640, 7680);
        spin->setValue(value);
        spin->setMinimumWidth(95);
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            if (screenCombo_->currentData().toString() == u"custom") {
                applyLayout();
            }
        });
        customRow->addWidget(spin);
    }
    customRow->addWidget(new QLabel(QStringLiteral("(effective size)")));
    customRow->addStretch();
    form->addRow(QStringLiteral("Custom"), customWidget);
    QWidget* customLabel = form->labelForField(customWidget);
    const auto updateCustomRow = [this, customWidget, customLabel] {
        const bool custom = screenCombo_->currentData().toString() == u"custom";
        customWidget->setEnabled(custom);
        customLabel->setEnabled(custom);
    };
    connect(screenCombo_, &QComboBox::currentIndexChanged, this, updateCustomRow);
    updateCustomRow();
    taskbarCheck_ = new QCheckBox(QStringLiteral("Leave room for the taskbar (%1px)").arg(layout::kTaskbarHeight));
    taskbarCheck_->setChecked(true);
    form->addRow(taskbarCheck_);
    scaleNote_ = new QLabel;
    scaleNote_->setWordWrap(true);
    form->addRow(scaleNote_);
    connect(screenCombo_, &QComboBox::currentIndexChanged, this, [this] { applyLayout(); });
    connect(taskbarCheck_, &QCheckBox::toggled, this, [this] { applyLayout(); });

    auto* shots = new QHBoxLayout;
    auto* shot = new QPushButton(QStringLiteral("Screenshot"));
    shot->setToolTip(QStringLiteral("This view at this size, into layoutScreenshots/"));
    connect(shot, &QPushButton::clicked, this, &ControlWindow::screenshot);
    shots->addWidget(shot);
    auto* all = new QPushButton(QStringLiteral("Capture all sizes"));
    all->setToolTip(QStringLiteral("Every view at every listed size, plus fit_report.txt, into layoutScreenshots/"));
    connect(all, &QPushButton::clicked, this, &ControlWindow::captureAll);
    shots->addWidget(all);
    form->addRow(shots);
    auto* note = new QLabel(QStringLiteral("The app scales itself to whatever screen it's on (every screen fits, all text "
                                           "fits); pick a size to see how the fonts and colors look there."));
    note->setWordWrap(true);
    form->addRow(note);
    return tab;
}

QWidget* ControlWindow::buildFontsTab(const QStringList& families) {
    auto* tab = new QWidget;
    auto* outer = new QVBoxLayout(tab);
    auto* tip = new QLabel(QStringLiteral("While this tab is open, click anything in the display: it's outlined, and the "
                                          "font row that styles it blinks below."));
    tip->setWordWrap(true);
    outer->addWidget(tip);
    auto* fontBox = new QGroupBox(QStringLiteral("Fonts"));
    auto* fontGrid = new QGridLayout(fontBox);
    fontGrid->addWidget(new QLabel(QStringLiteral("Family")), 0, 1);
    fontGrid->addWidget(new QLabel(QStringLiteral("Size")), 0, 2);
    fontGrid->addWidget(new QLabel(QStringLiteral("Weight")), 0, 3);
    QStringList systemFamilies;
    for (const QString& family : QFontDatabase::families()) {
        if (!families.contains(family)) {
            systemFamilies.append(family);
        }
    }
    int row = 1;
    for (const QString& role : fontRoles()) {
        auto* roleLabel = new QLabel(theme::kRoleTitles.value(role));
        fontRoleLabels_.insert(role, roleLabel);
        fontGrid->addWidget(roleLabel, row * 2 - 1, 0, 1, 4);
        auto* combo = new QComboBox;
        combo->setMaxVisibleItems(25);
        for (const QString& family : families) {
            combo->addItem(family);
            combo->setItemData(combo->count() - 1, QFont(family, 11), Qt::FontRole);
        }
        combo->insertSeparator(combo->count());
        combo->addItems(systemFamilies);
        auto* size = new QSpinBox;
        size->setRange(30, 300);
        size->setSingleStep(5);
        size->setSuffix(QStringLiteral("%"));
        size->setMinimumWidth(95);
        auto* weight = new QComboBox;
        weight->setMinimumWidth(80);
        weight->addItems(theme::kWeights);
        // (the gauges always draw bold)
        weight->setEnabled(role != theme::kRoleGauge);
        fontGrid->addWidget(combo, row * 2, 1);
        fontGrid->addWidget(size, row * 2, 2);
        fontGrid->addWidget(weight, row * 2, 3);
        connect(combo, &QComboBox::currentTextChanged, this,
                [this, role](const QString& value) { fontChanged(role, QStringLiteral("family"), value); });
        connect(size, &QSpinBox::valueChanged, this,
                [this, role](int value) { fontChanged(role, QStringLiteral("scale"), value); });
        connect(weight, &QComboBox::currentTextChanged, this,
                [this, role](const QString& value) { fontChanged(role, QStringLiteral("weight"), value); });
        fontControls_.insert(role, {combo, size, weight});
        ++row;
    }
    fontGrid->setColumnStretch(1, 1);
    outer->addWidget(fontBox);
    outer->addStretch(1);
    return tab;
}

QWidget* ControlWindow::buildColorsTab() {
    auto* tab = new QWidget;
    auto* outer = new QVBoxLayout(tab);
    auto* tip = new QLabel(QStringLiteral(
        "Every color the app uses, once each: changing one changes it everywhere, gradients included (the power zone "
        "colors are on the FTP Colors tab). Click a color's name or a gradient to see where it's used in this view; "
        "click anything in the display to blink its colors here."));
    tip->setWordWrap(true);
    outer->addWidget(tip);
    colorListWidget_ = new QWidget;
    colorList_ = new QGridLayout(colorListWidget_);
    colorList_->setContentsMargins(2, 2, 2, 2);
    colorScroll_ = new QScrollArea;
    colorScroll_->setWidgetResizable(true);
    colorScroll_->setWidget(colorListWidget_);
    outer->addWidget(colorScroll_, 1);
    outer->addLayout(partButtons(QStringLiteral("colors"), QStringLiteral("colors")));
    return tab;
}

QHBoxLayout* ControlWindow::partButtons(const QString& part, const QString& name) {
    auto* row = new QHBoxLayout;
    const QString what = theme::kParts.value(part);
    auto* save = new QPushButton(QStringLiteral("Save %1…").arg(name));
    save->setToolTip(QStringLiteral("Just the %1, as a file of their own (the app loads it from its ⚙ menu, on top of "
                                    "whichever theme it uses)")
                         .arg(what));
    connect(save, &QPushButton::clicked, this, [this, part] { savePart(part); });
    row->addWidget(save);
    auto* load = new QPushButton(QStringLiteral("Load %1…").arg(name));
    load->setToolTip(QStringLiteral("Replaces just the %1").arg(what));
    connect(load, &QPushButton::clicked, this, [this, part] { loadPart(part); });
    row->addWidget(load);
    row->addStretch();
    return row;
}

QWidget* ControlWindow::buildScenarioTab() {
    auto* tab = new QWidget;
    auto* form = new QFormLayout(tab);
    modeCombo_ = new QComboBox;
    for (const Mode& mode : kModes) {
        modeCombo_->addItem(mode.name, mode.mode);
    }
    modeCombo_->setToolTip(QStringLiteral("The setup screen's mode every view is built in"));
    form->addRow(QStringLiteral("Mode"), modeCombo_);
    workoutCombo_ = new QComboBox;
    QStringList workouts = QDir(kWorkoutDir).entryList({QStringLiteral("*.awo")}, QDir::Files);
    std::sort(workouts.begin(), workouts.end());
    workoutCombo_->addItems(workouts);
    if (workouts.contains(kDefaultWorkout)) {
        workoutCombo_->setCurrentText(kDefaultWorkout);
    }
    form->addRow(QStringLiteral("Workout file"), workoutCombo_);
    auto* progressRow = new QHBoxLayout;
    progressSlider_ = new QSlider(Qt::Horizontal);
    progressSlider_->setRange(1, 99);
    progressSlider_->setValue(40);
    progressLabel_ = new QLabel(QStringLiteral("40%"));
    progressLabel_->setMinimumWidth(36);
    connect(progressSlider_, &QSlider::valueChanged, this,
            [this](int value) { progressLabel_->setText(QStringLiteral("%1%").arg(value)); });
    progressRow->addWidget(progressSlider_);
    progressRow->addWidget(progressLabel_);
    form->addRow(QStringLiteral("Workout progress"), progressRow);
    batteryCheck_ = new QCheckBox(QStringLiteral("Show low battery warnings"));
    form->addRow(batteryCheck_);
    trainerCheck_ = new QCheckBox(
        QStringLiteral("Power device is an FE-C trainer (Max Watts/Power Drive Cutoff box, Free Ride Watts slider)"));
    form->addRow(trainerCheck_);
    messageCheck_ = new QCheckBox(QStringLiteral("Show an example workout message (under Ride At)"));
    messageCheck_->setToolTip(QStringLiteral("What an .awo file's timed |message| lines look like during a workout"));
    connect(messageCheck_, &QCheckBox::toggled, this, &ControlWindow::applyExampleMessage);
    form->addRow(messageCheck_);
    keysCheck_ = new QCheckBox(QStringLiteral("Show key bindings (the labels holding Ctrl shows in the app)"));
    keysCheck_->setToolTip(QStringLiteral("The keyboard keys for the buttons on the view showing"));
    connect(keysCheck_, &QCheckBox::toggled, this, &ControlWindow::keysToggled);
    form->addRow(keysCheck_);
    // (kept up to date while shown: buttons clicked here change which keys apply)
    keyLabelTimer_ = new QTimer(this);
    keyLabelTimer_->setInterval(500);
    connect(keyLabelTimer_, &QTimer::timeout, this, &ControlWindow::updateKeyLabels);
    auto* rebuildButton = new QPushButton(QStringLiteral("Rebuild views"));
    rebuildButton->setToolTip(QStringLiteral("Puts the screens back as they were built, e.g. after clicking their buttons"));
    connect(rebuildButton, &QPushButton::clicked, this, &ControlWindow::rebuild);
    form->addRow(rebuildButton);
    connect(workoutCombo_, &QComboBox::currentTextChanged, this, &ControlWindow::rebuild);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &ControlWindow::modeChanged);
    connect(progressSlider_, &QSlider::sliderReleased, this, &ControlWindow::rebuildWorkout);
    connect(progressSlider_, &QSlider::valueChanged, this, [this] {
        if (!progressSlider_->isSliderDown()) {
            rebuildWorkout();
        }
    });
    connect(batteryCheck_, &QCheckBox::toggled, display_, &DisplayWindow::setBatteryWarnings);
    connect(trainerCheck_, &QCheckBox::toggled, this, &ControlWindow::trainerChanged);
    return tab;
}

// ---- View ----

void ControlWindow::playingChanged(bool playing) {
    const QSignalBlocker blocker(playButton_);
    playButton_->setChecked(playing);
    playButton_->setText(playing ? QStringLiteral("❚❚ Pause") : QStringLiteral("▶ Play"));
}

void ControlWindow::viewChanged(const QString& name) {
    const qsizetype index = kViews.indexOf(name == u"Free Ride" ? QStringLiteral("Workout") : name);
    if (index >= 0) {
        viewButtons_->button(static_cast<int>(index))->setChecked(true);
    }
    refreshColorList();
    updateFitReport();
    // once the newly shown view has laid itself out: its key labels, then the preview
    QTimer::singleShot(250, this, &ControlWindow::updateKeyLabels);
    if (isOversize()) {
        QTimer::singleShot(300, this, [this] { preview_->refresh(display_); });
    }
}

void ControlWindow::rebuild() {
    const QString path = kWorkoutDir + u'/' + workoutCombo_->currentText();
    busy([this, path] { display_->build(modeCombo_->currentData().toString(), path, progressSlider_->value()); }, true);
}

void ControlWindow::modeChanged() {
    // (the workout file is only Workout Mode's)
    workoutCombo_->setEnabled(modeCombo_->currentData().toString() == u"workout");
    rebuild();
}

void ControlWindow::rebuildWorkout() {
    busy([this] { display_->buildWorkout(progressSlider_->value()); });
}

void ControlWindow::applyExampleMessage() {
    // the Workout view's message line, as a timed .awo message would show it (or empty)
    if (auto* hud = qobject_cast<HudScreen*>(display_->view(QStringLiteral("Workout")))) {
        hud->setWorkoutMessage(messageCheck_->isChecked() ? kExampleMessage : QString());
        if (isOversize()) {
            QTimer::singleShot(300, this, [this] { preview_->refresh(display_); });
        }
    }
}

void ControlWindow::keysToggled(bool show) {
    if (show) {
        keyLabelTimer_->start();
        updateKeyLabels();
    } else {
        keyLabelTimer_->stop();
        keys::unpinLabels();
    }
    if (isOversize()) {
        QTimer::singleShot(100, this, [this] { preview_->refresh(display_); });
    }
}

void ControlWindow::updateKeyLabels() {
    if (keysCheck_->isChecked() && !display_->views().isEmpty()) {
        keys::pinLabels(display_);
    }
}

void ControlWindow::trainerChanged(bool trainer) {
    display_->setTrainer(trainer);
    // (the Init view's power device changes too)
    rebuild();
}

void ControlWindow::busy(std::function<void()> work, bool full) {
    // (run inside this one, a second build would discard views this one is still building on)
    if (busyRunning_) {
        if (full || !busyNextFull_ || !busyNext_) {
            busyNext_ = std::move(work);
            busyNextFull_ = full;
        }
        return;
    }
    busyRunning_ = true;
    busyNext_ = std::move(work);
    busyNextFull_ = full;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusLabel_->setText(QStringLiteral("Building…"));
    while (busyNext_) {
        // (shows the status; may ask for another build, see above)
        QApplication::processEvents();
        const std::function<void()> next = std::exchange(busyNext_, {});
        busyNextFull_ = false;
        next();
        // (the rebuilt Workout view starts without it)
        applyExampleMessage();
        applyLayout();
        QApplication::processEvents();
    }
    busyRunning_ = false;
    QApplication::restoreOverrideCursor();
    statusLabel_->setText(QString());
}

// ---- Screen size ----

std::optional<QSize> ControlWindow::screenSize() const {
    const QString picked = screenCombo_->currentData().toString();
    if (picked == u"custom") {
        return QSize(customW_->value(), customH_->value());
    }
    if (picked.isEmpty() || picked == u"screen") {
        return std::nullopt;
    }
    const QStringList parts = picked.split(u'x');
    return QSize(parts[0].toInt(), parts[1].toInt());
}

QSize ControlWindow::availableSize() const {
    const std::optional<QSize> size = screenSize();
    if (!size) {
        QScreen* screen = homeScreen_ ? homeScreen_.data() : QGuiApplication::primaryScreen();
        return screen->availableGeometry().size();
    }
    return {size->width(), size->height() - (taskbarCheck_->isChecked() ? layout::kTaskbarHeight : 0)};
}

void ControlWindow::displayScreenChanged() {
    // moved to another monitor: when showing this screen's own size, that monitor's layout and scale (else they'd
    // only catch up at the next edit)
    if (!screenSize()) {
        monitorTimer_->start();
    }
}

void ControlWindow::monitorChanged() {
    // the scale only: the window is where the user (or Windows) put it, and changing its state in the middle of a
    // move to another monitor crashes Qt
    applyLayout(false);
}

QScreen* ControlWindow::homeScreen() {
    // the monitor the display window is on (wherever it's been moved to); while it's parked off the edge of the
    // desktop for an oversized screen, the one it was on before
    const QPoint center = display_->geometry().center();
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->geometry().contains(center)) {
            homeScreen_ = screen;
            break;
        }
    }
    return homeScreen_ ? homeScreen_.data() : QGuiApplication::primaryScreen();
}

bool ControlWindow::isOversize() const {
    if (!screenSize()) {
        return false;
    }
    const QSize available = availableSize();
    QScreen* screen = homeScreen_ ? homeScreen_.data() : QGuiApplication::primaryScreen();
    const QRect geometry = screen->availableGeometry();
    return available.width() > geometry.width() || available.height() > geometry.height();
}

void ControlWindow::placeDisplay() {
    // maximized for this screen, else exactly the simulated size; only touched when something has to change
    // (re-showing or re-maximizing it on every apply upsets Qt now and then)
    DisplayWindow* display = display_;
    if (!display->isVisible()) {
        // (the first build at startup: main() shows the window once it's done)
        return;
    }
    const QRect screen = homeScreen()->availableGeometry();
    if (!screenSize()) {
        display->maximizedMode = true;
        preview_->hide();
        if (display->isMaximized()) {
            return;
        }
        if (!screen.contains(display->geometry().center())) {
            // back from off the edge of the desktop onto its monitor, to maximize there
            display->setGeometry(screen.x(), screen.y(), 800, 600);
        }
        display->setWindowState(Qt::WindowMaximized);
        return;
    }
    display->maximizedMode = false;
    const QSize available = availableSize();
    QRect target;
    if (isOversize()) {
        // bigger than this screen: laid out at full size just past the right edge of the desktop (out of sight), and
        // shown scaled down in the preview window instead
        target = QRect(homeScreen()->virtualGeometry().right() + 100, screen.y(), available.width(), available.height());
    } else {
        target = QRect(screen.x(), screen.y(), available.width(), available.height());
        preview_->hide();
    }
    const bool wasMaximized = display->isMaximized();
    if (wasMaximized) {
        display->showNormal();
    }
    if (wasMaximized || display->geometry() != target) {
        display->setGeometry(target);
        // again once the window system has settled: leaving maximized, Windows restores the window's old normal size
        // afterwards (even when the maximized size already matched)
        for (const int delay : {50, 250}) {
            QTimer::singleShot(delay, display, [display, target] {
                if (display->geometry() != target && !display->maximizedMode) {
                    display->setGeometry(target);
                }
            });
        }
    }
}

// ---- Scaling to the screen (as the app does) ----

void ControlWindow::applyLayout(bool place) {
    const QSize available = availableSize();
    QMap<QString, QWidget*> screens;
    for (const auto& [name, view] : display_->views()) {
        screens.insert(name, view);
    }
    layout::applyLayout(display_, screens, layout::scaleFor(available), available);
    scaleNote_->setText(QStringLiteral("Available area %1×%2; scaled to %3%.")
                            .arg(available.width())
                            .arg(available.height())
                            .arg(percent(theme::uiScale())));
    refreshColorList();
    if (place) {
        placeDisplay();
    }
    QTimer::singleShot(250, this, &ControlWindow::afterLayout);
}

void ControlWindow::afterLayout() {
    updateFitReport();
    updateKeyLabels();
    if (isOversize()) {
        preview_->refresh(display_);
    }
}

QList<ControlWindow::Fit> ControlWindow::fitResults() const {
    const QSize available = availableSize();
    QList<Fit> results;
    for (const auto& [name, view] : display_->views()) {
        const QSize need = display_->requiredSize(name);
        results.append({name, need, need.width() <= available.width() && need.height() <= available.height()});
    }
    return results;
}

void ControlWindow::updateFitReport() {
    if (display_->views().isEmpty()) {
        return;
    }
    const QSize available = availableSize();
    QStringList lines{QStringLiteral("<b>%1×%2</b> available, layout scale %3%")
                          .arg(available.width())
                          .arg(available.height())
                          .arg(percent(theme::uiScale()))};
    for (const Fit& fit : fitResults()) {
        const QString current = fit.name == display_->current() ? QStringLiteral("▶ ") : QString();
        const QString needs = QStringLiteral("(needs %1×%2)").arg(fit.need.width()).arg(fit.need.height());
        if (fit.fits) {
            lines.append(QStringLiteral("%1<span style='color:#1b7f1b'>%2: fits</span> %3").arg(current, fit.name, needs));
        } else {
            QStringList over;
            if (fit.need.width() > available.width()) {
                over.append(QStringLiteral("%1px too wide").arg(fit.need.width() - available.width()));
            }
            if (fit.need.height() > available.height()) {
                over.append(QStringLiteral("%1px too tall").arg(fit.need.height() - available.height()));
            }
            lines.append(QStringLiteral("%1<span style='color:#b00020'>%2: %3</span> %4")
                             .arg(current, fit.name, over.join(QStringLiteral(", ")), needs));
        }
    }
    fitLabel_->setText(lines.join(QStringLiteral("<br>")));
}

// ---- Screenshots ----

QString ControlWindow::screenTag() const {
    if (const std::optional<QSize> size = screenSize()) {
        return QStringLiteral("%1x%2").arg(size->width()).arg(size->height());
    }
    const QSize available = availableSize();
    return QStringLiteral("screen_%1x%2").arg(available.width()).arg(available.height());
}

void ControlWindow::screenshot() {
    QDir().mkpath(kScreenshotDir);
    QString view = display_->current();
    view.remove(u' ');
    const QString path = QStringLiteral("%1/%2_%3.png").arg(kScreenshotDir, screenTag(), view);
    updateKeyLabels();
    display_->grab().save(path);
    statusLabel_->setText(QStringLiteral("Saved ") + QDir::toNativeSeparators(path));
}

void ControlWindow::captureAll() {
    // every listed screen size, every view: screenshots plus a fit report
    QDir().mkpath(kScreenshotDir);
    const int startIndex = screenCombo_->currentIndex();
    const QString startView = display_->current();
    QList<int> indexes;
    for (int i = 0; i < screenCombo_->count(); ++i) {
        const QString kind = screenCombo_->itemData(i).toString();
        if (kind != u"screen" && kind != u"custom") {
            indexes.append(i);
        }
    }
    // the steps: a size, then each view at it
    struct Step {
        int size = -1;
        QString view;
    };
    auto steps = std::make_shared<QList<Step>>();
    for (const int index : indexes) {
        steps->append(Step{index, {}});
        for (const QString& view : kViews) {
            steps->append(Step{-1, view});
        }
    }
    auto report = std::make_shared<QStringList>();
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [this, steps, report, run, startIndex, startView, sizes = indexes.size()](int position) {
        if (position == steps->size()) {
            QFile file(kScreenshotDir + QStringLiteral("/fit_report.txt"));
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&file);
                out.setEncoding(QStringConverter::Utf8);
                out << report->join(u'\n') << u'\n';
            }
            screenCombo_->setCurrentIndex(startIndex);
            display_->showView(startView);
            statusLabel_->setText(QStringLiteral("Captured %1 sizes × %2 views into layoutScreenshots/").arg(sizes).arg(kViews.size()));
            // (done: the chain lets itself go)
            QTimer::singleShot(0, this, [run] { *run = nullptr; });
            return;
        }
        const Step step = steps->at(position);
        if (step.size >= 0) {
            statusLabel_->setText(QStringLiteral("Capturing %1…").arg(screenCombo_->itemText(step.size)));
            screenCombo_->setCurrentIndex(step.size);
            // once the new size has settled
            QTimer::singleShot(700, this, [this, report, run, position, index = step.size] {
                const QSize available = availableSize();
                report->append(QStringLiteral("%1  (available %2×%3, scale %4%)")
                                   .arg(screenCombo_->itemText(index))
                                   .arg(available.width())
                                   .arg(available.height())
                                   .arg(percent(theme::uiScale())));
                for (const Fit& fit : fitResults()) {
                    report->append(QStringLiteral("    %1 %2  (needs %3×%4)")
                                       .arg(fit.name, -10)
                                       .arg(fit.fits ? QStringLiteral("fits") : QStringLiteral("DOES NOT FIT"))
                                       .arg(fit.need.width())
                                       .arg(fit.need.height()));
                }
                (*run)(position + 1);
            });
        } else {
            display_->showView(step.view);
            QTimer::singleShot(500, this, [this, run, position] {
                screenshot();
                (*run)(position + 1);
            });
        }
    };
    (*run)(0);
}

// ---- Fonts ----

void ControlWindow::syncFontControls() {
    for (auto it = fontControls_.cbegin(); it != fontControls_.cend(); ++it) {
        const theme::FontSettings settings = theme::current().fonts.value(it.key());
        const FontControls& controls = it.value();
        const QSignalBlocker family(controls.family);
        const QSignalBlocker size(controls.size);
        const QSignalBlocker weight(controls.weight);
        if (controls.family->findText(settings.family) < 0) {
            controls.family->addItem(settings.family);
        }
        controls.family->setCurrentText(settings.family);
        controls.size->setValue(static_cast<int>(settings.scale));
        controls.weight->setCurrentText(settings.weight);
    }
}

void ControlWindow::fontChanged(const QString& role, const QString& key, const QVariant& value) {
    theme::FontSettings& settings = theme::current().fonts[role];
    if (key == u"family") {
        settings.family = value.toString();
    } else if (key == u"scale") {
        settings.scale = value.toDouble();
    } else {
        settings.weight = value.toString();
    }
    restyleTimer_->start();
}

QComboBox* ControlWindow::fontFamily(const QString& role) const {
    return fontControls_.value(role).family;
}

QSpinBox* ControlWindow::fontSize(const QString& role) const {
    return fontControls_.value(role).size;
}

QComboBox* ControlWindow::fontWeight(const QString& role) const {
    return fontControls_.value(role).weight;
}

// ---- Colors ----

std::pair<QMap<QString, QSet<QString>>, QMap<Gradient, QSet<QString>>> ControlWindow::usedColors() const {
    QMap<QString, QSet<QString>> singles;
    QMap<Gradient, QSet<QString>> gradients;
    const auto add = [&](const WidgetColors& colors, const QString& where) {
        for (const QString& color : colors.singles) {
            singles[color].insert(where);
        }
        for (const Gradient& gradient : colors.gradients) {
            gradients[gradient].insert(where);
        }
    };
    const auto [rest, appGradients] = splitGradients(style::kAppStyleSheet);
    add({theme::colorsInCss(rest), appGradients}, QStringLiteral("All"));
    const auto inView = [this](const QWidget* widget) {
        for (const auto& [name, view] : display_->views()) {
            if (view == widget || view->isAncestorOf(widget)) {
                return true;
            }
        }
        return false;
    };
    add(widgetColors(display_), QStringLiteral("All"));
    for (const QWidget* widget : display_->findChildren<QWidget*>()) {
        if (!inView(widget)) {
            add(widgetColors(widget), QStringLiteral("All"));
        }
    }
    for (const auto& [name, view] : display_->views()) {
        add(widgetColors(view), name);
        for (const QWidget* widget : view->findChildren<QWidget*>()) {
            add(widgetColors(widget), name);
        }
    }
    return {singles, gradients};
}

void ControlWindow::refreshColorList() {
    // one row per color the app uses (in any view, in any mode seen this session), then the gradients made of them
    const auto [singles, gradients] = usedColors();
    for (auto it = singles.cbegin(); it != singles.cend(); ++it) {
        knownColors_[it.key()] += it.value();
    }
    for (auto it = gradients.cbegin(); it != gradients.cend(); ++it) {
        knownGradients_[it.key()] += it.value();
    }
    // a gradient's colors are rows like any other: what they're used for, gradients included
    QMap<QString, QSet<QString>> uses = knownColors_;
    for (auto it = knownGradients_.cbegin(); it != knownGradients_.cend(); ++it) {
        for (const QString& color : it.key()) {
            for (const QString& where : it.value()) {
                uses[color].insert(where + QStringLiteral(" gradient"));
            }
        }
    }
    if (uses.keys() == colorRows_.keys() && knownGradients_.keys() == gradientRows_.keys()) {
        for (auto it = uses.cbegin(); it != uses.cend(); ++it) {
            colorRows_[it.key()].uses->setText(usesText(it.value()));
            updateColorRow(it.key());
        }
        return;
    }
    while (QLayoutItem* item = colorList_->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    colorRows_.clear();
    gradientRows_.clear();
    QStringList colors = uses.keys();
    std::stable_sort(colors.begin(), colors.end(), [](const QString& a, const QString& b) { return hue(a) < hue(b); });
    int row = 0;
    for (const QString& color : colors) {
        auto* swatch = new QPushButton;
        swatch->setFixedSize(64, 24);
        swatch->setToolTip(QStringLiteral("%1: click to change it (everywhere it's used)").arg(color));
        connect(swatch, &QPushButton::clicked, this, [this, color] { pickColor(color); });
        auto* name = new ClickableLabel(color, [this, color] { showUses(color); });
        name->setToolTip(QStringLiteral("Click to see where it's used in this view"));
        auto* usesLabel = new QLabel(usesText(uses.value(color)));
        usesLabel->setWordWrap(true);
        usesLabel->setStyleSheet(QStringLiteral("color: #606060;"));
        auto* reset = new QPushButton(QStringLiteral("↺"));
        reset->setFixedWidth(28);
        reset->setToolTip(QStringLiteral("Back to the original color"));
        connect(reset, &QPushButton::clicked, this, [this, color] { setColor(color, std::nullopt); });
        colorList_->addWidget(swatch, row, 0);
        colorList_->addWidget(name, row, 1);
        colorList_->addWidget(usesLabel, row, 2);
        colorList_->addWidget(reset, row, 3);
        colorRows_.insert(color, {swatch, name, usesLabel, reset});
        ++row;
    }
    if (!knownGradients_.isEmpty()) {
        colorList_->addWidget(new QLabel(QStringLiteral("<b>Gradients</b> (made of the colors above)")), row, 0, 1, 4);
        ++row;
    }
    QList<Gradient> sortedGradients = knownGradients_.keys();
    std::stable_sort(sortedGradients.begin(), sortedGradients.end(),
                     [](const Gradient& a, const Gradient& b) { return hues(a) < hues(b); });
    for (const Gradient& gradient : sortedGradients) {
        auto* preview = new GradientPreview(gradient, [this, gradient] { showUses(QString(), gradient); });
        preview->setToolTip(gradient.join(QStringLiteral(" → ")) + QStringLiteral("\nUsed in: ") +
                            usesText(knownGradients_.value(gradient)) + QStringLiteral("\nClick to see where"));
        colorList_->addWidget(preview, row, 0, 1, 2);
        colorList_->addWidget(new QLabel(gradient.join(QStringLiteral(" → "))), row, 2, 1, 2);
        gradientRows_.insert(gradient, preview);
        ++row;
    }
    for (const QString& color : colorRows_.keys()) {
        updateColorRow(color);
    }
    colorList_->setRowStretch(row, 1);
    colorList_->setColumnStretch(2, 1);
}

QString ControlWindow::usesText(const QSet<QString>& where) {
    // "every view" or the views a color is in ("All": what every view shares), and whether gradients use it
    QSet<QString> views;
    bool inGradients = false;
    for (QString place : where) {
        if (place.endsWith(QStringLiteral(" gradient"))) {
            inGradients = true;
            place.chop(9);
        }
        views.insert(place);
    }
    QString text;
    if (views.contains(QStringLiteral("All"))) {
        text = QStringLiteral("every view");
    } else {
        QStringList names(views.begin(), views.end());
        names.sort();
        text = names.join(QStringLiteral(", "));
    }
    return text + (inGradients ? QStringLiteral(" · in gradients") : QString());
}

QString ControlWindow::usesText(const QString& color) const {
    const auto row = colorRows_.constFind(color);
    return row != colorRows_.cend() ? row->uses->text() : QString();
}

void ControlWindow::updateColorRow(const QString& color) {
    // the color's row redrawn, and the gradients it's in
    const auto found = theme::current().colors.constFind(color);
    const std::optional<QString> replacement =
        found != theme::current().colors.cend() ? std::optional<QString>(*found) : std::nullopt;
    if (const auto row = colorRows_.constFind(color); row != colorRows_.cend()) {
        // (as it is: not through the theme)
        row->swatch->setStyleSheet(QStringLiteral("background-color:%1; border:1px solid #444; border-radius:4px;")
                                       .arg(replacement.value_or(color)));
        row->name->setText(replacement ? QStringLiteral("%1  →  %2").arg(color, *replacement) : color);
        row->reset->setEnabled(replacement.has_value());
    }
    for (auto it = gradientRows_.cbegin(); it != gradientRows_.cend(); ++it) {
        if (it.key().contains(color)) {
            it.value()->update();
        }
    }
}

void ControlWindow::pickColor(const QString& color) {
    const auto found = theme::current().colors.constFind(color);
    const std::optional<QString> before =
        found != theme::current().colors.cend() ? std::optional<QString>(*found) : std::nullopt;
    QColorDialog dialog(QColor(before.value_or(color)), this);
    dialog.setWindowTitle(QStringLiteral("Replace ") + color);
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    connect(&dialog, &QColorDialog::currentColorChanged, this, [this, color](const QColor& picked) { setColor(color, picked.name()); });
    if (!dialog.exec()) {
        setColor(color, before);
    }
}

void ControlWindow::setColor(const QString& color, const std::optional<QString>& replacement) {
    if (!replacement || replacement->toLower() == color) {
        theme::current().colors.remove(color);
    } else {
        theme::current().colors.insert(color, replacement->toLower());
    }
    updateColorRow(color);
    restyleTimer_->start();
}

// ---- Identifying what's what (the Fonts and Colors tabs) ----

bool ControlWindow::eventFilter(QObject* watched, QEvent* event) {
    // while the Fonts or Colors tab is open, a click on the display identifies what's under it instead of clicking it
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
        auto* widget = qobject_cast<QWidget*>(watched);
        const QWidget* tab = tabs_->currentWidget();
        if (widget && (tab == fontsTab_ || tab == colorsTab_) && widget->window() == display_ &&
            !widget->property("awoExempt").toBool()) {
            if (type == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                // the widget the click went to (widgetAt() would find whatever is on top there, e.g. the controls
                // window overlapping the display)
                identify(widget);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString ControlWindow::fontRoleOf(QWidget* widget) const {
    // which font row styles the widget's text
    QList<QWidget*> chain{widget};
    while (chain.last()->parentWidget()) {
        chain.append(chain.last()->parentWidget());
    }
    for (int i = 0; i < std::min<qsizetype>(3, chain.size()); ++i) {
        if (isA<AnalogGauge>(chain[i]) || isA<Gauge>(chain[i])) {
            return theme::kRoleGauge;
        }
    }
    const QString buttonFamily = theme::current().fonts.value(theme::kRoleButton).family;
    if (qobject_cast<QPushButton*>(widget) ||
        (widget->font().family() == buttonFamily && buttonFamily != theme::current().fonts.value(theme::kRoleMain).family)) {
        return theme::kRoleButton;
    }
    return theme::kRoleMain;
}

void ControlWindow::identify(QWidget* widget) {
    // the clicked part of the display outlined, and the font and color rows for it blinked
    OutlineFlash::flash(display_, widget);
    const QString role = fontRoleOf(widget);
    QSet<const QWidget*> stops{display_, display_->centralWidget()};
    for (const auto& [name, view] : display_->views()) {
        stops.insert(view);
    }
    QList<QWidget*> chain{widget};
    while (chain.last()->parentWidget() && !stops.contains(chain.last()->parentWidget())) {
        chain.append(chain.last()->parentWidget());
    }
    bool inGauge = false;
    for (int i = 0; i < std::min<qsizetype>(3, chain.size()); ++i) {
        const QWidget* part = chain[i];
        inGauge = inGauge || isA<AnalogGauge>(part) || isA<Gauge>(part) || isA<RotatingSvgWidget>(part) ||
                  isA<BeatingSvgWidget>(part);
    }
    // its colors: the widget's own, else the closest part around it (never the whole screen's)
    WidgetColors colors;
    if (!inGauge) {
        for (int i = 0; i < std::min<qsizetype>(3, chain.size()); ++i) {
            colors = widgetColors(chain[i]);
            if (!colors.singles.isEmpty() || !colors.gradients.isEmpty()) {
                break;
            }
        }
    }
    QList<QWidget*> rows{fontRoleLabels_.value(role)};
    QStringList found;
    QStringList singles(colors.singles.begin(), colors.singles.end());
    singles.sort();
    for (const QString& color : singles) {
        if (colorRows_.contains(color)) {
            rows.append(colorRows_.value(color).name);
            found.append(color);
        }
    }
    for (const Gradient& gradient : colors.gradients) {
        if (gradientRows_.contains(gradient)) {
            rows.append(gradientRows_.value(gradient));
            for (const QString& color : gradient) {
                if (colorRows_.contains(color)) {
                    rows.append(colorRows_.value(color).name);
                }
            }
            found.append(gradient.join(QStringLiteral(" → ")));
        }
    }
    blinkWidgets(rows);
    if (rows.size() > 1 && tabs_->currentWidget() == colorsTab_) {
        colorScroll_->ensureWidgetVisible(rows[1]);
    }
    const QString what = !widget->objectName().isEmpty() && !widget->objectName().startsWith(QStringLiteral("themed"))
                             ? widget->objectName()
                             : typeName(widget);
    QString colorText;
    if (inGauge) {
        colorText = QStringLiteral("; its colors are drawn by the gauge itself (not themeable)");
    } else if (!found.isEmpty()) {
        colorText = QStringLiteral("; colors ") + found.join(QStringLiteral(", "));
    } else {
        colorText = QStringLiteral("; no colors of its own");
    }
    statusLabel_->setText(
        QStringLiteral("Clicked %1: font row \"%2\"%3").arg(what, theme::kRoleTitles.value(role), colorText));
}

void ControlWindow::showUses(const QString& color, const Gradient& gradient) {
    // everything in the current view (and the title bar) that uses the color or gradient, outlined
    QList<QWidget*> widgets{display_->titleBar()};
    widgets += display_->titleBar()->findChildren<QWidget*>();
    if (QWidget* view = display_->view(display_->current())) {
        widgets.append(view);
        widgets += view->findChildren<QWidget*>();
    }
    QList<QWidget*> hits;
    for (QWidget* widget : widgets) {
        if (!widget->isVisible()) {
            continue;
        }
        const WidgetColors colors = widgetColors(widget);
        bool uses = false;
        if (!color.isEmpty()) {
            uses = colors.singles.contains(color);
            for (const Gradient& g : colors.gradients) {
                uses = uses || g.contains(color);
            }
        } else if (!gradient.isEmpty()) {
            uses = colors.gradients.contains(gradient);
        }
        if (uses) {
            hits.append(widget);
        }
    }
    // the innermost ones are enough when a whole screen uses it (e.g. a background)
    QList<QWidget*> innermost;
    for (QWidget* widget : hits) {
        bool outer = false;
        for (QWidget* other : hits) {
            outer = outer || (other != widget && widget->isAncestorOf(other));
        }
        if (!outer) {
            innermost.append(widget);
        }
    }
    if (!innermost.isEmpty()) {
        hits = innermost;
    }
    for (QWidget* widget : hits.mid(0, 60)) {
        OutlineFlash::flash(display_, widget, QStringLiteral("#00e5ff"));
    }
    const QString label = !color.isEmpty() ? color : gradient.join(QStringLiteral(" → "));
    statusLabel_->setText(QStringLiteral("%1: %2 part(s) of this view").arg(label).arg(hits.size()) +
                          (hits.isEmpty() ? QStringLiteral(" (it may be in the app wide stylesheet, used by parts not on "
                                                           "this view)")
                                          : QString()));
}

// ---- Applying the theme ----

void ControlWindow::applyTheme() {
    theme::apply();
    refreshColorList();
    QTimer::singleShot(250, this, &ControlWindow::afterLayout);
}

// ---- The theme file ----

void ControlWindow::saveTheme() {
    const QString suggested = QDir(theme::themesDir()).filePath(QStringLiteral("theme.json"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save theme"), suggested, QStringLiteral("Theme (*.json)"));
    if (!path.isEmpty()) {
        saveThemeTo(path);
    }
}

void ControlWindow::saveThemeTo(const QString& path) {
    theme::saveFile(path);
    statusLabel_->setText(QStringLiteral("Saved to %1  (load it in the app from the ⚙ menu)").arg(QFileInfo(path).fileName()));
}

void ControlWindow::savePart(const QString& part) {
    const QString folder = theme::partDir(part);
    QDir().mkpath(folder);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save ") + theme::kParts.value(part), folder,
                                                      QStringLiteral("Theme (*.json)"));
    if (!path.isEmpty()) {
        savePartTo(part, path);
    }
}

void ControlWindow::savePartTo(const QString& part, const QString& path) {
    theme::saveFile(path, part);
    statusLabel_->setText(QStringLiteral("Saved the %1 to %2  (load it in the app from the ⚙ menu: it goes on top of "
                                         "whichever theme the app uses)")
                              .arg(theme::kParts.value(part), QFileInfo(path).fileName()));
}

void ControlWindow::loadPart(const QString& part) {
    const QString folder = QFileInfo(theme::partDir(part)).isDir() ? theme::partDir(part) : QDir::currentPath();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load ") + theme::kParts.value(part), folder,
                                                      QStringLiteral("Theme (*.json)"));
    if (!path.isEmpty()) {
        loadPartFrom(part, path);
    }
}

void ControlWindow::loadPartFrom(const QString& part, const QString& path) {
    try {
        const QJsonObject file = theme::readFile(path);
        if (!file.contains(part)) {
            throw theme::ThemeError(QStringLiteral("it has no %1").arg(theme::kParts.value(part)).toStdString());
        }
        if (theme::fileKind(file) == part) {
            theme::loadFile(path);
        } else {
            // a whole theme: just this part of it
            theme::current().setPart(part, file.value(part).toObject());
            theme::apply();
        }
    } catch (const theme::ThemeError& e) {
        statusLabel_->setText(QStringLiteral("Couldn't load %1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what())));
        return;
    }
    applyLayout();
    statusLabel_->setText(QStringLiteral("Loaded the %1 from %2").arg(theme::kParts.value(part), QFileInfo(path).fileName()));
}

void ControlWindow::saveDefaultTheme(bool ask) {
    if (ask && QFile::exists(theme::kDefaultThemeFile) &&
        QMessageBox::question(this, QStringLiteral("Save as default"),
                              QStringLiteral("Replace the current %1?").arg(theme::kDefaultThemeFile)) != QMessageBox::Yes) {
        return;
    }
    theme::saveFile(theme::kDefaultThemeFile);
    statusLabel_->setText(QStringLiteral("Saved as %1: the app uses it whenever no theme is picked (and after Clear Theme)")
                              .arg(theme::kDefaultThemeFile));
}

void ControlWindow::loadTheme() {
    const QString folder = QFileInfo(theme::themesDir()).isDir() ? theme::themesDir() : QDir::currentPath();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load theme"), folder, QStringLiteral("Theme (*.json)"));
    if (!path.isEmpty()) {
        loadThemeFrom(path);
    }
}

void ControlWindow::loadThemeFrom(const QString& path) {
    QString kind;
    try {
        kind = theme::loadFile(path);
    } catch (const theme::ThemeError& e) {
        statusLabel_->setText(QStringLiteral("Couldn't load %1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what())));
        return;
    }
    syncFontControls();
    applyLayout();
    statusLabel_->setText(QStringLiteral("Loaded ") + QFileInfo(path).fileName() +
                          (kind == u"theme" ? QString() : QStringLiteral(" (just the %1)").arg(theme::kParts.value(kind))));
}

void ControlWindow::resetTheme() {
    theme::current().reset();
    syncFontControls();
    applyLayout();
    statusLabel_->setText(QStringLiteral("Back to the app's own fonts and colors"));
}

void ControlWindow::closeEvent(QCloseEvent* event) {
    if (ftpMockup_) {
        ftpMockup_->close();
    }
    event->accept();
    QApplication::quit();
}

}  // namespace awo::viewer
