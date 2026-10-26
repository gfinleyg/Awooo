#include "CalibratePowerWindow.hpp"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

constexpr int kDeviceTypePowerMeter = 11;

QString optionalText(const std::optional<int>& value) {
    return value ? QString::number(*value) : QStringLiteral("None");
}

}  // namespace

CalibratePowerWindow::CalibratePowerWindow(ant::AntBridge* bridge) : bridge_(bridge), timer_(new QTimer(this)) {
    auto* titleBar = framed(this, QStringLiteral("calibratePowerWindow"), QStringLiteral("   Calibrate Power   "),
                            QStringLiteral(" QLabel {font: 900 14pt 'Orbitron';}"), false);
    setGeometry(500, 120, 1000, 760);
    if (const auto device = bridge_->selectedDevice(ant::Role::Power)) {
        deviceType_ = device->deviceType;
        deviceNumber_ = device->deviceNumber;
        saved_ = calibration::get(device->deviceType, device->deviceNumber);
    }
    antZeroOffset_ = saved_ ? saved_->antZeroOffset : std::nullopt;

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleBar);
    live_ = new QLabel;
    live_->setAlignment(Qt::AlignCenter);
    theme::setSheet(live_, QStringLiteral("background-color:#bdd6de; border-radius: 10px; padding: 6px;"));
    layout->addWidget(live_);
    pages_ = new QStackedWidget;
    pages_->addWidget(buildIntroPage());
    pages_->addWidget(buildZeroOffsetPage());
    pages_->addWidget(buildRestPage());
    pages_->addWidget(buildRidePage());
    pages_->addWidget(buildReviewPage());
    layout->addWidget(pages_, 1);

    auto* nav = new QHBoxLayout;
    cancel_ = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel_, &QPushButton::clicked, this, &QWidget::close);
    back_ = new QPushButton(QStringLiteral("Back"));
    connect(back_, &QPushButton::clicked, this, [this] { goToPage(pages_->currentIndex() - 1); });
    next_ = new QPushButton(QStringLiteral("Next"));
    connect(next_, &QPushButton::clicked, this, &CalibratePowerWindow::nextPage);
    nav->addWidget(cancel_);
    nav->addStretch();
    nav->addWidget(back_);
    nav->addWidget(next_);
    layout->addLayout(nav);

    // keys (Ctrl shows them; Esc is the X and Cancel); each page's own buttons only work on that page (they're
    // hidden on the others). Enter in the reference watts box is Apply
    keys::add(next_, QList<int>{Qt::Key_N} + keys::kEnter, {QStringLiteral("N")});
    keys::add(back_, Qt::Key_B, QStringLiteral("B"));
    for (const auto& [button, key] : {std::pair{sendZero_, Qt::Key_Z}, std::pair{measureRest_, Qt::Key_M},
                                      std::pair{measureRide_, Qt::Key_S}, std::pair{applyScale_, Qt::Key_A},
                                      std::pair{skipRide_, Qt::Key_K}, std::pair{clear_, Qt::Key_R}}) {
        keys::add(button, key, QString(QChar(key)));
    }
    keys::Options inReference;
    inReference.field = reference_;
    keys::add(applyScale_, keys::kEnter, inReference);

    layout::fitDialog(this);
    // (each page's text fitted as it's shown too)
    for (int i = 0; i < pages_->count(); ++i) {
        layout::watchScreen(pages_->widget(i));
    }
    connect(bridge_, &ant::AntBridge::calibrationUpdate, this, &CalibratePowerWindow::onCalibrationUpdate);
    connect(timer_, &QTimer::timeout, this, &CalibratePowerWindow::pollPower);
    timer_->start(100);
    goToPage(0);
    pollPower();
}

// ---- The pages ----

QWidget* CalibratePowerWindow::pageWith(const QString& text, QLabel** label) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* textLabel = new QLabel(text);
    textLabel->setWordWrap(true);
    textLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(textLabel);
    if (label) {
        *label = textLabel;
    }
    return page;
}

QLabel* CalibratePowerWindow::statusLabel(QWidget* page) {
    auto* label = new QLabel;
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    page->layout()->addWidget(label);
    return label;
}

QWidget* CalibratePowerWindow::buildIntroPage() {
    QString text;
    if (!deviceNumber_) {
        text = QStringLiteral("No power meter is connected.\n\nConnect your power meter on the setup screen first, then "
                              "open Calibrate Power again.");
    } else {
        text = QStringLiteral("Calibrating power device %1.\nCurrent settings: %2\n\n"
                              "This takes about a minute:\n"
                              "   1. Zero offset - the power meter's own calibration\n"
                              "   2. At-rest check - measures what the meter reads with no load\n"
                              "   3. Accuracy check (optional) - compare a steady ride against a reference\n"
                              "   4. Review and save\n\n"
                              "Put the bike on the trainer and have the cranks free to turn.")
                   .arg(*deviceNumber_)
                   .arg(calibration::describe(saved_));
    }
    QWidget* page = pageWith(text);
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();
    return page;
}

QWidget* CalibratePowerWindow::buildZeroOffsetPage() {
    const bool meter = deviceType_ == kDeviceTypePowerMeter;
    const QString text =
        meter ? QStringLiteral("Step 1: Zero offset\n\n"
                               "   - Spin the cranks backwards a few times to wake the power meter\n"
                               "   - Unclip and stop the cranks with the drive side crank pointing straight down\n"
                               "   - Take all weight off the pedals and don't touch the cranks\n\n"
                               "Then press Send Zero Offset. The meter usually answers within a few seconds.")
              : QStringLiteral("Step 1: Zero offset\n\n"
                               "This power source is a smart trainer, which calibrates with its own spin down from the "
                               "trainer maker's app rather than a zero offset. Press Next to continue.");
    QWidget* page = pageWith(text);
    auto* layout = qobject_cast<QVBoxLayout*>(page->layout());
    sendZero_ = new QPushButton(QStringLiteral("Send Zero Offset"));
    connect(sendZero_, &QPushButton::clicked, this, &CalibratePowerWindow::sendZeroOffset);
    sendZero_->setVisible(meter);
    layout->addWidget(sendZero_, 0, Qt::AlignHCenter);
    zeroStatus_ = statusLabel(page);
    layout->addStretch();
    return page;
}

QWidget* CalibratePowerWindow::buildRestPage() {
    QWidget* page = pageWith(
        QStringLiteral("Step 2: At-rest check\n\n"
                       "Keep your feet off the pedals and the cranks still. Press Measure and the reading is averaged "
                       "for %1 seconds. Anything above 0 W becomes an offset subtracted from every reading. Most meters "
                       "read 0 W at rest, which means no offset is needed.")
            .arg(kRestMeasureSeconds));
    auto* layout = qobject_cast<QVBoxLayout*>(page->layout());
    measureRest_ = new QPushButton(QStringLiteral("Measure"));
    connect(measureRest_, &QPushButton::clicked, this, [this] { startMeasure(QStringLiteral("rest")); });
    layout->addWidget(measureRest_, 0, Qt::AlignHCenter);
    restStatus_ = statusLabel(page);
    layout->addStretch();
    return page;
}

QWidget* CalibratePowerWindow::buildRidePage() {
    QWidget* page = pageWith(
        QStringLiteral("Step 3: Accuracy check (optional)\n\n"
                       "Only needed if you have a reference to compare against, such as a trainer display or a second "
                       "power meter. Press Start, then ride at a steady effort for %1 seconds. Enter the average watts "
                       "your reference showed for that same period and press Apply. Press Skip to leave accuracy "
                       "unchanged.")
            .arg(kRideMeasureSeconds));
    auto* layout = qobject_cast<QVBoxLayout*>(page->layout());
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Reference average watts:")));
    reference_ = new QLineEdit;
    reference_->setMaximumWidth(160);
    row->addWidget(reference_);
    row->addStretch();
    layout->addLayout(row);
    auto* buttons = new QHBoxLayout;
    measureRide_ = new QPushButton(QStringLiteral("Start"));
    connect(measureRide_, &QPushButton::clicked, this, [this] { startMeasure(QStringLiteral("ride")); });
    applyScale_ = new QPushButton(QStringLiteral("Apply"));
    connect(applyScale_, &QPushButton::clicked, this, &CalibratePowerWindow::applyScale);
    applyScale_->setEnabled(false);
    skipRide_ = new QPushButton(QStringLiteral("Skip"));
    connect(skipRide_, &QPushButton::clicked, this, &CalibratePowerWindow::skipRide);
    for (QPushButton* button : {measureRide_, applyScale_, skipRide_}) {
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    rideStatus_ = statusLabel(page);
    layout->addStretch();
    return page;
}

QWidget* CalibratePowerWindow::buildReviewPage() {
    QWidget* page = pageWith(QString(), &review_);
    auto* layout = qobject_cast<QVBoxLayout*>(page->layout());
    auto* buttons = new QHBoxLayout;
    clear_ = new QPushButton(QStringLiteral("Remove Saved Calibration"));
    connect(clear_, &QPushButton::clicked, this, &CalibratePowerWindow::clearSaved);
    buttons->addWidget(clear_);
    layout->addLayout(buttons);
    reviewStatus_ = statusLabel(page);
    layout->addStretch();
    return page;
}

// ---- Moving between them ----

void CalibratePowerWindow::goToPage(int index) {
    index = std::clamp(index, 0, pages_->count() - 1);
    stopMeasure();
    pages_->setCurrentIndex(index);
    back_->setEnabled(index > 0);
    const bool last = index == pages_->count() - 1;
    layout::setText(next_, last ? QStringLiteral("Save") : index == 0 ? QStringLiteral("Start") : QStringLiteral("Next"));
    next_->setEnabled(deviceNumber_.has_value());
    if (last) {
        refreshReview();
    }
}

int CalibratePowerWindow::page() const {
    return pages_->currentIndex();
}

void CalibratePowerWindow::nextPage() {
    if (pages_->currentIndex() == pages_->count() - 1) {
        save();
    } else {
        goToPage(pages_->currentIndex() + 1);
    }
}

// ---- Step 1: ANT+ zero offset ----

void CalibratePowerWindow::sendZeroOffset() {
    sendZero_->setEnabled(false);
    layout::setText(zeroStatus_, QStringLiteral("Sending the zero offset request..."));
    bridge_->requestPowerCalibration(*deviceNumber_);
}

void CalibratePowerWindow::onCalibrationUpdate(int deviceNumber, const QString& result, const QVariant& offset,
                                               int attempts) {
    if (deviceNumber != deviceNumber_) {
        return;
    }
    if (result == u"sending") {
        layout::setText(zeroStatus_,
                        QStringLiteral("Waiting for the power meter to answer... (request %1)").arg(attempts));
        return;
    }
    sendZero_->setEnabled(true);
    const QString offsetText = offset.isValid() && !offset.isNull() ? QString::number(offset.toInt()) : QStringLiteral("None");
    if (result == u"success") {
        antZeroOffset_ = offset.isValid() && !offset.isNull() ? std::optional(offset.toInt()) : std::nullopt;
        layout::setText(zeroStatus_,
                        QStringLiteral("Zero offset succeeded. The meter reported offset %1.\nPress Next.").arg(offsetText));
    } else if (result == u"failed") {
        layout::setText(zeroStatus_, QStringLiteral("The power meter reported that calibration failed (offset %1).\n"
                                                    "Check the cranks are still and unweighted, then try again.")
                                         .arg(offsetText));
    } else {
        layout::setText(zeroStatus_,
                        QStringLiteral("No answer from the power meter.\nSpin the cranks backwards to wake it and try "
                                       "again, or press Next to skip this step."));
    }
}

// ---- Live readings and measurements ----

QString CalibratePowerWindow::liveText() const {
    return live_->text();
}

QString CalibratePowerWindow::zeroStatus() const {
    return zeroStatus_->text();
}

void CalibratePowerWindow::pollPower() {
    const auto device = bridge_->selectedDevice(ant::Role::Power);
    if (!device || !device->powerWatts) {
        layout::setText(live_, QStringLiteral("Power meter: waiting for readings..."));
    } else {
        const int raw = *device->powerWatts;
        calibration::Calibration trial;
        trial.offsetWatts = offsetWatts_;
        trial.scale = scale_;
        layout::setText(live_, QStringLiteral("Power meter now: %1 W    with new calibration: %2 W")
                                   .arg(raw)
                                   .arg(calibration::apply(raw, trial)));
        // (new power readings only, not every page)
        if (!measuring_.isEmpty() && device->version != lastVersion_) {
            samples_.append(raw);
        }
        lastVersion_ = device->version;
    }
    if (!measuring_.isEmpty()) {
        const double remaining = measureSeconds_ - measureClock_.nsecsElapsed() / 1e9;
        QLabel* status = measuring_ == u"rest" ? restStatus_ : rideStatus_;
        if (remaining > 0) {
            layout::setText(status, QStringLiteral("Measuring... %1 s left (%2 readings)")
                                        .arg(static_cast<int>(remaining) + 1)
                                        .arg(samples_.size()));
        } else {
            finishMeasure();
        }
    }
}

void CalibratePowerWindow::startMeasure(const QString& kind) {
    samples_.clear();
    measuring_ = kind;
    measureSeconds_ = kind == u"rest" ? kRestMeasureSeconds : kRideMeasureSeconds;
    measureClock_.start();
    measureRest_->setEnabled(false);
    measureRide_->setEnabled(false);
    next_->setEnabled(false);
}

void CalibratePowerWindow::stopMeasure() {
    measuring_.clear();
    measureRest_->setEnabled(true);
    measureRide_->setEnabled(true);
    next_->setEnabled(deviceNumber_.has_value());
}

void CalibratePowerWindow::finishMeasureWith(const QString& kind, const QList<int>& samples) {
    measuring_ = kind;
    samples_ = samples;
    finishMeasure();
}

void CalibratePowerWindow::finishMeasure() {
    const QString kind = measuring_;
    const QList<int> samples = samples_;
    stopMeasure();
    QLabel* status = kind == u"rest" ? restStatus_ : rideStatus_;
    if (samples.isEmpty()) {
        layout::setText(status, QStringLiteral("No readings arrived from the power meter. Wake it up and try again."));
        return;
    }
    double sum = 0;
    for (const int sample : samples) {
        sum += sample;
    }
    const double average = sum / samples.size();
    if (kind == u"rest") {
        offsetWatts_ = py::round(average, 1);
        if (offsetWatts_ == 0) {
            layout::setText(restStatus_, QStringLiteral("Reads 0 W at rest, so no offset is needed. Press Next."));
        } else {
            layout::setText(restStatus_, QStringLiteral("Reads %1 W at rest; this will be subtracted from every "
                                                        "reading. Press Next.")
                                             .arg(py::formatG(offsetWatts_)));
        }
    } else {
        double above = 0;
        for (const int sample : samples) {
            above += std::max(0.0, sample - offsetWatts_);
        }
        rideMeasuredWatts_ = above / samples.size();
        applyScale_->setEnabled(true);
        layout::setText(rideStatus_, QStringLiteral("Power meter averaged %1 W.\nEnter your reference's average for "
                                                    "the same period and press Apply.")
                                         .arg(py::fixed(*rideMeasuredWatts_, 0)));
    }
}

// ---- Step 3: accuracy ----

void CalibratePowerWindow::setReference(const QString& text) {
    reference_->setText(text);
}

void CalibratePowerWindow::applyScale() {
    const std::optional<double> reference = py::toFloat(reference_->text());
    if (!reference) {
        layout::setText(rideStatus_, QStringLiteral("Enter the reference average as a number of watts."));
        return;
    }
    if (!rideMeasuredWatts_ || *rideMeasuredWatts_ < 30) {
        layout::setText(rideStatus_, QStringLiteral("The ride average was too low to compare. Ride at a steady effort "
                                                    "of at least 50 W and try again."));
        return;
    }
    const double scale = *reference / *rideMeasuredWatts_;
    if (!(calibration::kMinScale <= scale && scale <= calibration::kMaxScale)) {
        layout::setText(rideStatus_, QStringLiteral("That would scale readings by x%1, which is too far off to be a "
                                                    "calibration error.\nCheck the reference number, or ride again at "
                                                    "a steadier effort.")
                                         .arg(py::fixed(scale, 2)));
        return;
    }
    scale_ = py::round(scale, 4);
    layout::setText(rideStatus_, QStringLiteral("Readings will be scaled by x%1. Press Next.").arg(py::fixed(scale_, 3)));
}

void CalibratePowerWindow::skipRide() {
    scale_ = 1.0;
    goToPage(pages_->currentIndex() + 1);
}

// ---- Step 4: review ----

QString CalibratePowerWindow::reviewText() const {
    return review_->text();
}

void CalibratePowerWindow::refreshReview() {
    const QString zero = antZeroOffset_ ? QStringLiteral("meter reported offset %1").arg(*antZeroOffset_)
                                        : QStringLiteral("not run");
    layout::setText(review_, QStringLiteral("Step 4: Review\n\n"
                                            "   Zero offset: %1\n"
                                            "   At-rest offset: %2 W subtracted from each reading\n"
                                            "   Accuracy scale: x%3\n\n"
                                            "Previously saved: %4\n\n"
                                            "Press Save to use these settings for this power meter in every workout.")
                                 .arg(zero, py::formatG(offsetWatts_), py::fixed(scale_, 3),
                                      calibration::describe(saved_)));
    clear_->setEnabled(saved_.has_value());
}

void CalibratePowerWindow::save() {
    if (!calibration::save(*deviceType_, *deviceNumber_, offsetWatts_, scale_, antZeroOffset_)) {
        layout::setText(reviewStatus_, QStringLiteral("Couldn't save the calibration: %1 couldn't be written")
                                           .arg(settings::kSettingsFile));
        return;
    }
    log::event(QStringLiteral("CALIBRATE"),
               QStringLiteral("Saved power calibration for %1 %2: offset=%3 scale=%4 ant_zero_offset=%5")
                   .arg(*deviceType_)
                   .arg(*deviceNumber_)
                   .arg(py::str(offsetWatts_), py::str(scale_), optionalText(antZeroOffset_)));
    close();
}

void CalibratePowerWindow::clearSaved() {
    if (!calibration::clear(*deviceType_, *deviceNumber_)) {
        layout::setText(reviewStatus_, QStringLiteral("Couldn't remove the calibration: %1 couldn't be written")
                                           .arg(settings::kSettingsFile));
        return;
    }
    saved_.reset();
    offsetWatts_ = 0.0;
    scale_ = 1.0;
    refreshReview();
    layout::setText(reviewStatus_,
                    QStringLiteral("Saved calibration removed; readings are used as the meter reports them."));
}

// Cancel, Save, the title bar's X and Esc all end here
void CalibratePowerWindow::closeEvent(QCloseEvent* event) {
    timer_->stop();
    bridge_->cancelPowerCalibration();
    disconnect(bridge_, &ant::AntBridge::calibrationUpdate, this, &CalibratePowerWindow::onCalibrationUpdate);
    QWidget::closeEvent(event);
}

}  // namespace awo
