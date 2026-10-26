#include "Dialogs.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppStyle.hpp"
#include "CustomTitleBar.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "Performance.hpp"
#include "Theme.hpp"
#include "Units.hpp"

namespace awo {

namespace {

// a popup window's frame: frameless, its own look, and its title bar
CustomTitleBar* framed(QWidget* window, const QString& name, const QString& title, const QString& extraStyle = {}) {
    window->setObjectName(name);
    window->setAttribute(Qt::WA_StyledBackground);
    window->setWindowTitle(title);
    window->setWindowFlags(qobject_cast<QDialog*>(window) ? (Qt::FramelessWindowHint | Qt::Dialog) : Qt::FramelessWindowHint);
    theme::setSheet(window, QStringLiteral("#%1 {background-color:#9fc5d1;}").arg(name) + extraStyle);
    auto* titleBar = new CustomTitleBar(window, false);
    theme::setSheet(titleBar, QStringLiteral("background-color: #9fc5d1"));
    titleBar->setFixedHeight(36);
    return titleBar;
}

void key(QWidget* widget, int code, const QString& label) {
    keys::add(widget, code, label);
}

}  // namespace

// ---- Confirm exit ----

ConfirmExitWindow::ConfirmExitWindow(ant::AntBridge* bridge) : bridge_(bridge) {
    auto* titleBar = framed(this, QStringLiteral("confirmExitWindow"), QStringLiteral("   Exit Awooo Workout?   "));
    yes_ = new QPushButton(QStringLiteral("Yes, Exit"));
    no_ = new QPushButton(QStringLiteral("No"));
    connect(yes_, &QPushButton::clicked, this, &ConfirmExitWindow::exitProgram);
    connect(no_, &QPushButton::clicked, this, &QWidget::hide);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    layout->addWidget(yes_);
    layout->addWidget(no_);
    key(yes_, Qt::Key_Y, QStringLiteral("Y"));
    key(no_, Qt::Key_N, QStringLiteral("N"));
    layout::fitDialog(this);
}

void ConfirmExitWindow::exitProgram() {
    // every window hidden first, so the app disappears the moment Yes is clicked: the ANT shutdown can take a second
    // or more (the SDK's USB receive thread only notices the stop between 1 s reads)
    for (QWidget* window : QApplication::topLevelWidgets()) {
        window->hide();
    }
    // the stick closed cleanly before leaving, or it's left mid-session for the next launch to recover from
    if (bridge_) {
        bridge_->stop();
        if (!bridge_->wait(3000)) {
            log::event(QStringLiteral("ANT"), QStringLiteral("ANT bridge did not shut down within 3s, exiting anyway"));
        }
    }
    QApplication::quit();
}

// ---- Confirm clear session ----

ConfirmClearSessionWindow::ConfirmClearSessionWindow(std::function<void()> clearSession) {
    auto* titleBar =
        framed(this, QStringLiteral("confirmClearSessionWindow"), QStringLiteral("   Clear the session's rides?   "));
    yes_ = new QPushButton(QStringLiteral("Yes, Clear Session"));
    auto* no = new QPushButton(QStringLiteral("No"));
    connect(yes_, &QPushButton::clicked, this, [this, clearSession] {
        hide();
        clearSession();
    });
    connect(no, &QPushButton::clicked, this, &QWidget::hide);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    layout->addWidget(yes_);
    layout->addWidget(no);
    key(yes_, Qt::Key_Y, QStringLiteral("Y"));
    key(no, Qt::Key_N, QStringLiteral("N"));
    layout::fitDialog(this);
}

// ---- The log ----

ShowLogsWindow::ShowLogsWindow() {
    auto* titleBar = framed(this, QStringLiteral("showLogsWindow"), QStringLiteral("   %1   ").arg(log::kLogFile));
    setGeometry(500, 80, 1100, 900);
    // a plain text view rather than a label: selectable and copyable (drag, Ctrl+A, Ctrl+C), and it scrolls itself
    text_ = new QPlainTextEdit;
    text_->setReadOnly(true);
    theme::setSheet(text_, QStringLiteral("QPlainTextEdit { font: 10pt 'Consolas'; background-color: white; "
                                          "border: 2px solid #bdd6de; border-radius: 8px; }"));
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    readLog();

    auto* buttons = new QHBoxLayout;
    auto* copy = new QPushButton(QStringLiteral("Copy All"));
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_->toPlainText()); });
    auto* clear = new QPushButton(QStringLiteral("Clear Logs"));
    connect(clear, &QPushButton::clicked, this, [this] {
        log::clear();
        readLog();
    });
    auto* exit = new QPushButton(QStringLiteral("Exit"));
    connect(exit, &QPushButton::clicked, this, &QWidget::hide);
    buttons->addWidget(copy);
    buttons->addWidget(clear);
    buttons->addWidget(exit);
    // (Ctrl+C still copies a selection; Esc is the X, and Exit)
    key(copy, Qt::Key_C, QStringLiteral("C"));
    key(clear, Qt::Key_L, QStringLiteral("L"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    layout->addWidget(text_);
    layout->addLayout(buttons);
    layout::fitDialog(this);

    // refreshed while showing, so a log viewed during a workout stays current
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &ShowLogsWindow::readLog);
}

void ShowLogsWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    readLog();
    timer_->start();
}

void ShowLogsWindow::hideEvent(QHideEvent* event) {
    // (Exit, the X, Esc and minimizing all end up here: nothing to refresh while it isn't showing)
    timer_->stop();
    QWidget::hideEvent(event);
}

void ShowLogsWindow::readLog() {
    // setting the text clears any selection and resets the cursor: not while text is selected (a copy in progress),
    // nor when nothing changed (most ticks)
    if (text_->textCursor().hasSelection()) {
        return;
    }
    const QString newText = log::read();
    if (newText == text_->toPlainText()) {
        return;
    }
    // stuck to the bottom across refreshes only when the latest line was showing: reading history isn't disturbed
    QScrollBar* scrollbar = text_->verticalScrollBar();
    const bool atBottom = scrollbar->value() >= scrollbar->maximum() - 4;
    text_->setPlainText(newText);
    if (atBottom) {
        scrollbar->setValue(scrollbar->maximum());
    }
}

// ---- Measurements ----

MeasurementsWindow::MeasurementsWindow(QWidget* parent) : QDialog(parent) {
    auto* titleBar = framed(this, QStringLiteral("measurementsWindow"), QStringLiteral("   Measurements   "),
                            QStringLiteral("\nQLabel {font: 900 14pt 'Orbitron';}"
                                           "\nQRadioButton {font: 900 13pt 'Orbitron'; color: black; padding: 4px 10px;}") +
                                style::kDialogButtons);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);

    struct Row {
        QString label;
        QStringList units;
        QStringList names;
        int key;
        std::function<QString()> current;
        std::function<void(const QString&)> set;
    };
    Units& units = Units::current();
    const QList<Row> rows = {
        {QStringLiteral("Weight"), {QStringLiteral("lbs"), QStringLiteral("kg")}, {QStringLiteral("lbs"), QStringLiteral("kg")},
         Qt::Key_W, [&units] { return units.weight; }, [&units](const QString& unit) { units.setUnits(unit); }},
        {QStringLiteral("Power to weight"), {QStringLiteral("W/kg"), QStringLiteral("W/lb")},
         {QStringLiteral("W/kg"), QStringLiteral("W/lb")}, Qt::Key_P, [&units] { return units.powerWeight; },
         [&units](const QString& unit) { units.setUnits({}, unit); }},
        {QStringLiteral("Distance"), {QStringLiteral("mi"), QStringLiteral("km")},
         {QStringLiteral("Miles (MPH)"), QStringLiteral("Km (KM/H)")}, Qt::Key_D, [&units] { return units.distance; },
         [&units](const QString& unit) { units.setUnits({}, {}, unit); }},
    };
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    for (int row = 0; row < rows.size(); ++row) {
        const Row& spec = rows[row];
        auto* label = new QLabel(spec.label + u':');
        grid->addWidget(label, row, 0, Qt::AlignRight);
        auto* group = new QButtonGroup(this);
        for (int column = 0; column < spec.units.size(); ++column) {
            auto* radio = new QRadioButton(spec.names[column]);
            radio->setProperty("unit", spec.units[column]);
            radio->setChecked(spec.current() == spec.units[column]);
            group->addButton(radio);
            grid->addWidget(radio, row, column + 1);
        }
        const auto set = spec.set;
        connect(group, &QButtonGroup::buttonClicked, this,
                [set](QAbstractButton* radio) { set(radio->property("unit").toString()); });
        // a key steps through the row's choices
        keys::Options options;
        options.label = QString(QChar(spec.key));
        options.action = [group] { nextChoice(group); };
        keys::add(label, {spec.key}, options);
    }
    layout->addLayout(grid);

    auto* note = new QLabel(QStringLiteral("Your weight is set on the setup screen (0 leaves it out).\n"
                                           "With a weight, the synopsis shows power to weight too."));
    theme::setSheet(note, QStringLiteral("font: 900 10pt 'Orbitron';"));
    note->setAlignment(Qt::AlignCenter);
    layout->addWidget(note);
    auto* done = new QPushButton(QStringLiteral("Done"));
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(done, 0, Qt::AlignCenter);
    keys::Options enter;
    enter.label = QStringLiteral("Enter");
    keys::add(done, keys::kEnter, enter);
    layout::fitDialog(this);
}

void MeasurementsWindow::nextChoice(QButtonGroup* group) {
    const QList<QAbstractButton*> radios = group->buttons();
    const qsizetype checked = radios.indexOf(group->checkedButton());
    radios[checked >= 0 ? (checked + 1) % radios.size() : 0]->click();
}

// ---- Performance ----

PerformanceWindow::PerformanceWindow(QWidget* parent) : QDialog(parent) {
    auto* titleBar = framed(this, QStringLiteral("performanceWindow"), QStringLiteral("   Performance   "),
                            QStringLiteral("\nQLabel {font: 900 14pt 'Orbitron';}") + style::kDialogButtons);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->addWidget(new QLabel(QStringLiteral("Gauge needles:")), 0, 0, 1, 3, Qt::AlignCenter);
    // one stop per choice, lightest on the left
    const QList<int>& choices = Performance::kGaugeFpsChoices;
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(0, static_cast<int>(choices.size()) - 1);
    slider_->setPageStep(1);
    slider_->setTickPosition(QSlider::TicksBelow);
    slider_->setTickInterval(1);
    slider_->setMinimumWidth(320);
    slider_->setValue(static_cast<int>(choices.indexOf(Performance::current().gaugeFps())));
    QList<QLabel*> ends;
    for (const QString& text : {QStringLiteral("Lighter"), QStringLiteral("Smoother")}) {
        auto* end = new QLabel(text);
        theme::setSheet(end, QStringLiteral("font: 900 10pt 'Orbitron';"));
        ends << end;
    }
    grid->addWidget(ends[0], 1, 0);
    grid->addWidget(slider_, 1, 1);
    grid->addWidget(ends[1], 1, 2);
    value_ = new QLabel;
    theme::setSheet(value_, QStringLiteral("font: 900 12pt 'Orbitron';"));
    grid->addWidget(value_, 2, 0, 1, 3, Qt::AlignCenter);
    layout->addLayout(grid);
    connect(slider_, &QSlider::valueChanged, this, [this](int index) {
        Performance::current().setGaugeFps(Performance::kGaugeFpsChoices[index]);
        showValue();
    });
    showValue();

    auto* note = new QLabel(QStringLiteral("How smoothly the gauge needles glide between readings.\n"
                                           "Lower it if the workout screen is sluggish on this computer."));
    theme::setSheet(note, QStringLiteral("font: 900 10pt 'Orbitron';"));
    note->setAlignment(Qt::AlignCenter);
    layout->addWidget(note);
    auto* done = new QPushButton(QStringLiteral("Done"));
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(done, 0, Qt::AlignCenter);
    // Left/Right move the slider wherever the focus is
    keys::Options left;
    left.label = QStringLiteral("← →");
    left.repeat = true;
    left.action = [this] { slider_->setValue(slider_->value() - 1); };
    keys::add(slider_, {Qt::Key_Left}, left);
    keys::Options right;
    right.repeat = true;
    right.action = [this] { slider_->setValue(slider_->value() + 1); };
    keys::add(slider_, {Qt::Key_Right}, right);
    keys::Options enter;
    enter.label = QStringLiteral("Enter");
    keys::add(done, keys::kEnter, enter);
    layout::fitDialog(this);
}

void PerformanceWindow::showValue() {
    layout::setText(value_, Performance::gaugeFpsText(Performance::kGaugeFpsChoices[slider_->value()]));
}

}  // namespace awo
