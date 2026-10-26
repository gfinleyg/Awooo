#include "DeviceDialogs.hpp"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"

namespace awo {

namespace {

QString typeOf(const QString& line) {
    return line.split(u' ', Qt::SkipEmptyParts).value(0);
}

QString numberOf(const QString& line) {
    return line.split(u' ', Qt::SkipEmptyParts).value(1);
}

bool ofKind(const QString& line, SelectDevicesWindow::Kind kind) {
    const QString type = typeOf(line);
    switch (kind) {
        case SelectDevicesWindow::Kind::Power:
            return type == u"11" || type == u"17";
        case SelectDevicesWindow::Kind::HeartRate:
            return type == u"120";
        default:
            return type == u"121" || type == u"122" || type == u"123";
    }
}

}  // namespace

// ---- Select devices ----

SelectDevicesWindow::SelectDevicesWindow(const QStringList& devices, Kind kind, const QStringList& found)
    : group_(new QButtonGroup(this)),
      speedGroup_(new QButtonGroup(this)),
      cadenceGroup_(new QButtonGroup(this)),
      blinkTimer_(new QTimer(this)) {
    for (const QString& device : devices) {
        devices_.append(device.trimmed());
    }
    auto* titleBar = framed(this, QStringLiteral("selectDevicesWindow"), QStringLiteral("   Device Selection   "), {}, false);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    for (QButtonGroup* group : {group_, speedGroup_, cadenceGroup_}) {
        connect(group, &QButtonGroup::buttonClicked, this, &SelectDevicesWindow::buttonClicked);
    }
    // the devices picked before, listed even when the scan hasn't found them
    const QStringList selected = selectedDevices::read();
    for (const QString& line : selected) {
        if (ofKind(line, kind)) {
            devices_.append(line);
        }
    }
    QStringList lines = found;
    for (const QString& line : selected) {
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    QList<qsizetype> picked;
    for (const QString& line : lines) {
        if (!devices_.contains(line)) {
            continue;
        }
        const QString type = typeOf(line);
        const QString number = numberOf(line);
        static const QHash<QString, QString> kNames = {
            {QStringLiteral("120"), QStringLiteral("Heart Rate Monitor #: ")},
            {QStringLiteral("11"), QStringLiteral("Power Meter #: ")},
            {QStringLiteral("17"), QStringLiteral("Stationary Bike #: ")},
            {QStringLiteral("121"), QStringLiteral("Speed/Cadence Meter #: ")},
            {QStringLiteral("122"), QStringLiteral("Cadence Meter #: ")},
            {QStringLiteral("123"), QStringLiteral("Speed Meter #: ")},
        };
        if (!kNames.contains(type)) {
            continue;
        }
        auto* button = new QPushButton(kNames.value(type) + number);
        pressed_.insert(button, false);
        if (selected.contains(line)) {
            picked.append(buttons_.size());
        }
        buttons_.append(button);
        QButtonGroup* group = type == u"122" ? cadenceGroup_ : type == u"123" ? speedGroup_ : group_;
        group->addButton(button, type.toInt());
    }
    for (QPushButton* button : buttons_) {
        layout->addWidget(button);
    }
    keep_ = new QPushButton(QStringLiteral("Keep Current Selections"));
    connect(keep_, &QPushButton::clicked, this, [this] {
        stopBlinking();
        hide();
    });
    layout->addWidget(keep_);

    // keys (Ctrl shows them): 1-9 the devices in order
    for (qsizetype i = 0; i < std::min<qsizetype>(buttons_.size(), 9); ++i) {
        keys::add(buttons_[i], Qt::Key_1 + static_cast<int>(i), QString::number(i + 1));
    }
    keys::add(keep_, keys::kEnter, {QStringLiteral("Enter")});
    layout::fitDialog(this);

    // the devices picked before, pressed (not saved again)
    for (const qsizetype index : picked) {
        buttons_[index]->click();
    }
    initialized_ = true;

    // every candidate blinks while there's a choice to make: more than one found for the slot (a single candidate is
    // picked by the setup screen itself, see SetupScreen::autoSelectAndConnect)
    blinkTimer_->setInterval(500);
    connect(blinkTimer_, &QTimer::timeout, this, &SelectDevicesWindow::blinkCandidates);
    if (buttons_.size() > 1) {
        blinkTimer_->start();
    }
}

bool SelectDevicesWindow::blinking() const {
    return blinkTimer_->isActive();
}

void SelectDevicesWindow::blinkCandidates() {
    blinkOn_ = !blinkOn_;
    for (QPushButton* button : buttons_) {
        if (button->isEnabled() && !pressed_.value(button) && blinkOn_) {
            theme::setSheet(button, QStringLiteral("background-color: #FFEB3B;"));
        } else {
            theme::setSheet(button, QString());
        }
    }
}

// one of each kind picked: picking a power meter, heart rate monitor or combined speed/cadence meter greys out the
// others of its kind; a speed or cadence meter greys out the combined ones (and the others of its kind)
void SelectDevicesWindow::buttonClicked(QAbstractButton* clicked) {
    auto* toggled = qobject_cast<QPushButton*>(clicked);
    pressed_[toggled] = !pressed_.value(toggled);
    const bool on = pressed_.value(toggled);
    const bool speedButton = speedGroup_->buttons().contains(toggled);
    const bool cadenceButton = cadenceGroup_->buttons().contains(toggled);
    QButtonGroup* group = speedButton ? speedGroup_ : cadenceButton ? cadenceGroup_ : group_;
    const QString device = QString::number(group->id(toggled)) + u' ' + toggled->text().split(u' ').last();
    bool speedPressed = false;
    bool cadencePressed = false;
    for (QAbstractButton* button : speedGroup_->buttons()) {
        button->setDown(pressed_.value(qobject_cast<QPushButton*>(button)));
        speedPressed = speedPressed || pressed_.value(qobject_cast<QPushButton*>(button));
        if (speedButton) {
            if (on) {
                if (button != toggled) {
                    button->setEnabled(false);
                }
            } else {
                button->setEnabled(true);
            }
        } else if (!cadenceButton) {
            button->setEnabled(!on);
        }
    }
    for (QAbstractButton* button : cadenceGroup_->buttons()) {
        button->setDown(pressed_.value(qobject_cast<QPushButton*>(button)));
        cadencePressed = cadencePressed || pressed_.value(qobject_cast<QPushButton*>(button));
        if (cadenceButton) {
            if (on) {
                if (button != toggled) {
                    button->setEnabled(false);
                }
            } else {
                button->setEnabled(true);
            }
        } else if (!speedButton) {
            button->setEnabled(!on);
        }
    }
    for (QAbstractButton* button : group_->buttons()) {
        button->setDown(pressed_.value(qobject_cast<QPushButton*>(button)));
        if (speedButton || cadenceButton) {
            button->setEnabled(!(speedPressed || cadencePressed));
        } else if (on) {
            button->setEnabled(button == toggled);
        } else {
            button->setEnabled(true);
        }
    }
    if (!initialized_) {
        return;
    }
    if (on) {
        log::event(QStringLiteral("SETUP"), QStringLiteral("User manually selected device: %1").arg(device));
        selectedDevices::write(selectedDevices::read() + QStringList{device});
    } else {
        log::event(QStringLiteral("SETUP"), QStringLiteral("User manually deselected device: %1").arg(device));
        QStringList lines = selectedDevices::read();
        lines.removeAll(device.trimmed());
        selectedDevices::write(lines);
    }
}

void SelectDevicesWindow::closeEvent(QCloseEvent* event) {
    // (the title bar's X and Esc)
    stopBlinking();
    QWidget::closeEvent(event);
}

void SelectDevicesWindow::stopBlinking() {
    blinkTimer_->stop();
    for (QPushButton* button : buttons_) {
        theme::setSheet(button, QString());
    }
}

// ---- Connect devices ----

ConnectDevicesWindow::ConnectDevicesWindow(ant::AntBridge* bridge) : bridge_(bridge) {
    auto* titleBar = framed(this, QStringLiteral("connectDevicesWindow"), QStringLiteral("   Connect Devices   "),
                            QStringLiteral(" QLabel{font: 900 28pt 'Orbitron'; qproperty-indent:3; border-radius: 15px; "
                                           "background-color: transparent;}"),
                            false);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    label_ = new QLabel;
    label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(label_);
    layout::setText(label_, connectingMessage(selectedDevices::read(), bridge ? bridge->foundDevices() : QStringList()));
    connect_ = new QPushButton(QStringLiteral("Connect to Selected Devices"));
    connect(connect_, &QPushButton::clicked, this, &ConnectDevicesWindow::connectDevices);
    layout->addWidget(connect_);
    reselect_ = new QPushButton(QStringLiteral("Reselect Devices"));
    connect(reselect_, &QPushButton::clicked, this, &QWidget::hide);
    layout->addWidget(reselect_);
    keys::add(connect_, QList<int>{Qt::Key_C} + keys::kEnter, {QStringLiteral("Enter")});
    keys::add(reselect_, Qt::Key_R, QStringLiteral("R"));
    layout::fitDialog(this);
}

QString ConnectDevicesWindow::message() const {
    return label_->text();
}

QString ConnectDevicesWindow::connectingMessage(const QStringList& selected, const QStringList& found) {
    QStringList connecting;
    QStringList others;
    bool notFound = false;
    bool power = false, heartRate = false, speedCadence = false, speed = false, cadence = false;
    for (const QString& line : selected) {
        const QString type = typeOf(line);
        const QString number = numberOf(line);
        QString text;
        if (type == u"11") {
            text = QStringLiteral("Power Meter: ") + number;
            power = true;
        } else if (type == u"17") {
            text = QStringLiteral("Stationary Trainer: ") + number;
            power = true;
        } else if (type == u"120") {
            text = QStringLiteral("Heart Rate Monitor: ") + number;
            heartRate = true;
        } else if (type == u"121") {
            text = QStringLiteral("Speed/Cadence Meter: ") + number;
            speedCadence = true;
        } else if (type == u"122") {
            text = QStringLiteral("Cadence Meter: ") + number;
            cadence = true;
        } else if (type == u"123") {
            text = QStringLiteral("Speed Meter: ") + number;
            speed = true;
        }
        if (found.contains(line)) {
            connecting.append(text + QStringLiteral(" - Connecting"));
        } else {
            connecting.append(line + QStringLiteral(" - Not Found"));
            notFound = true;
        }
    }
    // anything else found that could be used but won't be
    for (const QString& line : found) {
        if (selected.contains(line)) {
            continue;
        }
        const QString type = typeOf(line);
        const QString number = numberOf(line);
        if (type == u"11" && !power) {
            others.append(QStringLiteral("Power Meter: ") + number);
        } else if (type == u"17" && !power) {
            others.append(QStringLiteral("Stationary Trainer: ") + number);
        } else if (type == u"120" && !heartRate) {
            others.append(QStringLiteral("Heart Rate Monitor: ") + number);
        } else if (type == u"121" && !(speedCadence || cadence || speed)) {
            others.append(QStringLiteral("Speed/Cadence Meter: ") + number);
        } else if (type == u"122" && !(speedCadence || cadence)) {
            others.append(QStringLiteral("Cadence Meter: ") + number);
        } else if (type == u"123" && !(speedCadence || speed)) {
            others.append(QStringLiteral("Speed Meter: ") + number);
        }
    }
    QString message = QStringLiteral("\n");
    if (notFound) {
        message += QStringLiteral("Not all selected devices have been detected yet:\n");
    }
    message += connecting.join(u'\n');
    if (others.size() > 1) {
        message += QStringLiteral("\n\nOther Unselected Usable Devices Scanned:\n") + others.join(u'\n');
    } else if (others.size() == 1) {
        message += QStringLiteral("\n\nOne Unselected Usable Device Scanned:\n") + others.front();
    }
    if (!others.isEmpty() || notFound) {
        message += QStringLiteral("\n\nAvailable devices unselected, are you sure you want to connect?");
    }
    return message + u'\n';
}

void ConnectDevicesWindow::connectDevices() {
    if (bridge_) {
        bridge_->requestConnect(ant::readSelectedDevices());
    }
    hide();
}

}  // namespace awo
