#include "ViewerDisplay.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AppStyle.hpp"
#include "CustomTitleBar.hpp"
#include "HudScreen.hpp"
#include "MusicSelectDialog.hpp"
#include "PreviewRide.hpp"
#include "PyCompat.hpp"
#include "Session.hpp"
#include "SetupScreen.hpp"
#include "SimulatedRider.hpp"
#include "SynopsisScreen.hpp"
#include "Theme.hpp"
#include "WorkoutData.hpp"
#include "WorkoutHelpers.hpp"
#include "WorkoutSelectDialog.hpp"

namespace awo::viewer {

const QStringList kViews = {QStringLiteral("Init"), QStringLiteral("Workout"), QStringLiteral("Synopsis"),
                            QStringLiteral("Music Picker"), QStringLiteral("Workout Picker")};
const QList<Mode> kModes = {{QStringLiteral("workout"), QStringLiteral("Workout Mode (workout file)")},
                            {QStringLiteral("free"), QStringLiteral("Free Mode")},
                            {QStringLiteral("20m"), QStringLiteral("20m FTP Test")},
                            {QStringLiteral("1h"), QStringLiteral("1h FTP Test")},
                            {QStringLiteral("ramp10"), QStringLiteral("10W delta FTP Ramp")},
                            {QStringLiteral("ramp20"), QStringLiteral("20W delta FTP Ramp")},
                            {QStringLiteral("cadence"), QStringLiteral("Musical Cadence Mode")}};
const QString kAppTitle = QStringLiteral("Awooo Workout");

const QList<std::pair<QString, QList<std::pair<int, int>>>> kViewerSongs = {
    {QStringLiteral("Viewer Band - Warm Up Groove"), {{30, 0}, {90, 82}, {60, 88}}},
    {QStringLiteral("The Layouts - Tempo Change"), {{70, 96}, {50, 72}, {60, 104}}},
    {QStringLiteral("Sample Artist - Cool Down"), {{80, 90}, {20, 0}, {60, 78}}}};

music::CadenceRide viewerRide() {
    music::CadenceRide ride;
    int start = 0;
    for (int index = 0; index < kViewerSongs.size(); ++index) {
        const auto& [title, blocks] = kViewerSongs[index];
        int duration = 0;
        for (const auto& [seconds, rpm] : blocks) {
            duration += seconds;
        }
        ride.tracks.append({QStringLiteral("viewer-song-%1.mp3").arg(index + 1), title, double(start), double(duration)});
        ride.songStarts.insert(static_cast<int>(ride.blocks.size()), index);
        int blockStart = start;
        for (const auto& [seconds, rpm] : blocks) {
            ride.blocks.append({seconds, rpm});
            for (int k = 0; rpm && k < seconds * rpm / 60; ++k) {
                ride.pulses.append(blockStart + k * 60.0 / rpm);
            }
            blockStart += seconds;
        }
        start += duration;
    }
    return ride;
}

namespace {

const QString kWindowStyle = QStringLiteral(
    "#layoutDisplay{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}");
constexpr int kTitleHeight = 36;

// The song picker with made-up songs: nothing analyzed, nothing saved
class ViewerMusicPicker : public MusicSelectDialog {
public:
    ViewerMusicPicker() : MusicSelectDialog(nullptr, false) {
        QList<std::pair<QString, std::optional<QString>>> songs;
        QSet<QString> checked;
        for (const auto& [title, blocks] : kViewerSongs) {
            songs.append({QStringLiteral("C:/Music/%1.mp3").arg(title), title});
            checked.insert(QStringLiteral("C:/Music/%1.mp3").arg(title));
        }
        songs.append({QStringLiteral("C:/Music/Unchecked Song.mp3"), QStringLiteral("Some Artist - Unchecked Song")});
        songs.append({QStringLiteral("C:/Music/Broken File.mp3"), QStringLiteral("Broken File")});
        addToList(songs, checked);
        missing = {QStringLiteral("C:/Music/Not There.mp3")};
        updateSummary();
    }

protected:
    void analyze(const QStringList& paths) override {
        for (const QString& path : paths) {
            music::Analysis analysis;
            if (path.contains(QStringLiteral("Broken"))) {
                analysis.error = QStringLiteral("can't be read");
            } else {
                analysis.duration = 180.0 + 37 * (analyses.size() % 4);
            }
            analyses.insert(path, analysis);
        }
        for (QListWidgetItem* item : items()) {
            labelItem(item);
        }
    }
    void save() override {}
};

// The workout picker, listing what the app's would (nothing saved)
class ViewerWorkoutPicker : public WorkoutSelectDialog {
protected:
    void save() override {}
};

// A picker dialog as a screen of the display window (its own title shows in the display's title bar)
QWidget* embedded(QDialog* dialog) {
    dialog->setWindowFlags(Qt::Widget);
    if (auto* titleBar = dialog->findChild<CustomTitleBar*>()) {
        titleBar->hide();
    }
    return dialog;
}

}  // namespace

// ---- DisplayWindow ----

DisplayWindow::DisplayWindow() : bridge_(std::make_unique<PreviewBridge>()), readingTimer_(new QTimer(this)) {
    readingTimer_->setInterval(1000);
    connect(readingTimer_, &QTimer::timeout, this, &DisplayWindow::playReading);
    setObjectName(QStringLiteral("layoutDisplay"));
    resize(800, 600);
    setWindowFlags(Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("   ") + kAppTitle);
    // the app wide stylesheet here only: the controls keep the plain look
    theme::setPrefixSheet(this, style::kAppStyleSheet);
    theme::setSheet(this, kWindowStyle);
    mainLayout_ = new QVBoxLayout;
    // the window can be smaller than its contents need, so a too small screen shows what gets cut off
    mainLayout_->setSizeConstraint(QLayout::SetNoConstraint);
    titleBar_ = new CustomTitleBar(this);
    titleBar_->setFixedHeight(kTitleHeight);
    mainLayout_->addWidget(titleBar_);
    auto* central = new QWidget;
    central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    central->setLayout(mainLayout_);
    setCentralWidget(central);
}

DisplayWindow::~DisplayWindow() {
    // (the views go before the bridge they hold)
    for (const QString& name : QStringList{QStringLiteral("Workout"), QStringLiteral("Free Ride")}) {
        discard(name);
    }
    while (!views_.isEmpty()) {
        delete views_.takeLast().second;
    }
}

QWidget* DisplayWindow::view(const QString& name) const {
    for (const auto& [viewName, widget] : views_) {
        if (viewName == name) {
            return widget;
        }
    }
    return nullptr;
}

HudScreen* DisplayWindow::workoutHud() const {
    return qobject_cast<HudScreen*>(view(viewKey(QStringLiteral("Workout"))));
}

SetupScreen* DisplayWindow::setupScreen() const {
    return qobject_cast<SetupScreen*>(view(QStringLiteral("Init")));
}

void DisplayWindow::discard(const QString& name) {
    if (name == u"Workout" || name == u"Free Ride") {
        pause();
    }
    for (int i = 0; i < views_.size(); ++i) {
        if (views_[i].first != name) {
            continue;
        }
        QWidget* widget = views_.takeAt(i).second;
        if (auto* hud = qobject_cast<HudScreen*>(widget)) {
            hud->stopTimers();
        }
        for (QTimer* timer : widget->findChildren<QTimer*>()) {
            timer->stop();
        }
        mainLayout_->removeWidget(widget);
        widget->hide();
        // (out of the window straight away: what's in it is looked through for its colors)
        widget->setParent(nullptr);
        widget->deleteLater();
        return;
    }
}

void DisplayWindow::addView(const QString& name, QWidget* widget) {
    views_.append({name, widget});
    mainLayout_->addWidget(widget);
    widget->hide();
}

void DisplayWindow::setBatteryWarnings(bool show) {
    bridge_->batteryWarnings = show;
    emit bridge_->batteryChanged();
}

void DisplayWindow::setTrainer(bool trainer) {
    bridge_->trainer = trainer;
}

QString DisplayWindow::viewKey(const QString& name) const {
    if (name == u"Workout" || name == u"Free Ride") {
        // (either way: current stays "Free Ride" when the views get rebuilt in another mode)
        return mode_ == u"free" ? QStringLiteral("Free Ride") : QStringLiteral("Workout");
    }
    return name;
}

void DisplayWindow::build(const QString& mode, const QString& workoutPath, int progressPercent) {
    while (!views_.isEmpty()) {
        discard(views_.last().first);
    }
    resetRideState();
    mode_ = mode;
    AppState& state = appState();
    state.workoutMode = true;
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.workoutFilename.clear();
    state.workoutFilenameShort.clear();
    selectedDevices::clear();
    selectedDevices::write(bridge_->foundDevices());

    // Init: connected, settings entered, workout loaded, ready for START
    auto* setup = new SetupScreen(bridge_.get());
    setup->useSettings(kViewerWheel, kViewerFtp, kViewerMaxHr);
    setup->initAntScanConnect();
    if (QFileInfo(workoutPath).isFile()) {
        setup->pickWorkoutFile(workoutPath);
    }
    connect(setup, &SetupScreen::startRequested, this, [this] { showView(QStringLiteral("Workout")); });

    // the pickers: as Workout Mode opens them (the workout picker), and Musical Cadence Mode (the song picker)
    addView(QStringLiteral("Workout Picker"), embedded(new ViewerWorkoutPicker));
    addView(QStringLiteral("Music Picker"), embedded(new ViewerMusicPicker));

    // the mode, picked on the setup screen like the app's own mode buttons
    if (mode != u"workout") {
        if (mode == u"cadence") {
            setup->setMusicBuilder([](const MusicSelection&) { return viewerRide(); });
        }
        setup->modeButton(mode)->click();
    }
    setup->checkEnableStartButton();
    for (QTimer* timer : setup->findChildren<QTimer*>(Qt::FindDirectChildrenOnly)) {
        timer->stop();
    }
    setup->settleBlink();
    addView(QStringLiteral("Init"), setup);

    // Synopsis: the whole workout ridden start to finish (by a workout screen of its own, never shown)
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.antCadence = false;
    state.antSpeed = false;
    const QDateTime startTimestamp = QDateTime::currentDateTime().addSecs(-2 * 3600);
    const double startSeconds = startTimestamp.toMSecsSinceEpoch() / 1000.0;
    double revolutions = 0;
    const auto record = [&](double t, const RiderReading& reading) {
        const double ts = startSeconds + t;
        WorkoutData& data = workoutData();
        data.power.appendAt(ts, {static_cast<double>(reading.power)});
        data.heartRate.appendAt(ts, {static_cast<double>(reading.heartRate)});
        data.fallbackCadence.appendAt(ts, {py::round(reading.cadence)});
        // speed sensor counters: cumulative revolutions and the 1/1024 s time of the last one
        const double revsPerSecond = reading.speedMph / 2.23694 / state.wheelCircumference;
        revolutions += revsPerSecond;
        const double lastRevTime = t - (revolutions - std::trunc(revolutions)) / std::max(revsPerSecond, 0.01);
        data.speed.appendAt(ts, {static_cast<double>(static_cast<long long>(revolutions) & 0xFFFF),
                                 static_cast<double>(static_cast<long long>(lastRevTime * 1024) & 0xFFFF)});
    };
    state.workoutMode = mode != u"free";
    {
        HudScreen simHud(bridge_.get());
        SimulatedRider rider(11);
        simHud.runSimulatedRide(state.workoutMode ? state.workoutDurationSeconds + 1 : kFreeRideSeconds, rider, record);
        simHud.stopTimers();
    }
    state.startTimestamp = startTimestamp;
    state.workoutPowerRequestsSysTime.clear();
    for (const double seconds : state.workoutPowerRequestsSeconds) {
        state.workoutPowerRequestsSysTime.append(startTimestamp.addMSecs(static_cast<qint64>(py::round(seconds * 1000))));
    }
    session::addCurrentRide();
    auto* synopsis = new SynopsisScreen(bridge_.get(), setup);
    // (the real one maximizes the window a second after it's built)
    synopsis->keepWindowSize();
    connect(synopsis, &SynopsisScreen::returnToSetupRequested, this, [this] { showView(QStringLiteral("Init")); });
    connect(synopsis, &SynopsisScreen::exitRequested, this, &QWidget::close);
    addView(QStringLiteral("Synopsis"), synopsis);
    buildWorkout(progressPercent);
}

void DisplayWindow::buildWorkout(int progressPercent) {
    const bool workoutMode = mode_ != u"free";
    AppState& state = appState();
    const double seconds = (workoutMode ? state.workoutDurationSeconds : kFreeRideSeconds) * progressPercent / 100.0;
    for (const QString& name : QStringList{QStringLiteral("Workout"), QStringLiteral("Free Ride")}) {
        discard(name);
    }
    resetRideState();
    state.startTimestamp = QDateTime::currentDateTime();
    state.workoutMode = workoutMode;
    songTitle_.clear();
    auto* hud = new HudScreen(bridge_.get());
    connect(hud, &HudScreen::setupRequested, this, [this] { showView(QStringLiteral("Init")); });
    connect(hud, &HudScreen::synopsisRequested, this, [this] { showView(QStringLiteral("Synopsis")); });
    // Musical Cadence Mode: the song playing, in the title bar
    connect(hud, &HudScreen::titleChanged, this, [this](const QString& title) {
        songTitle_ = title;
        if (current_ == u"Workout") {
            setWindowTitle(title);
            titleBar_->setTitle(title);
        }
    });
    rider_ = std::make_unique<SimulatedRider>(workoutMode ? 3 : 5);
    hud->runSimulatedRide(seconds, *rider_);
    addView(viewKey(QStringLiteral("Workout")), hud);
    showView(current_);
}

void DisplayWindow::showView(const QString& requested) {
    const QString name = viewKey(requested);
    QWidget* shown = view(name);
    if (!shown) {
        return;
    }
    if (name != current_) {
        pause();
    }
    current_ = name;
    // the others hidden before this one shows: for a moment both in the layout would add their minimum heights
    // together, pushing the window taller than the screen and out of maximized
    for (const auto& [viewName, widget] : views_) {
        if (widget != shown) {
            widget->hide();
        }
    }
    shown->show();
    // the same deferred re-maximize the main window uses after swapping screens (a frameless window quirk)
    if (maximizedMode && isVisible() && !isMaximized()) {
        QTimer::singleShot(0, this, &QWidget::showMaximized);
    }
    const AppState& state = appState();
    QString title;
    if (name == u"Music Picker" || name == u"Workout Picker") {
        title = QStringLiteral("   ") + shown->windowTitle().trimmed();
    } else if (name == u"Workout" && !songTitle_.isEmpty()) {
        title = songTitle_;
    } else if ((name == u"Workout" || name == u"Synopsis") && mode_ != u"free" && !state.workoutDisplayName().isEmpty()) {
        title = QStringLiteral("   %1: %2").arg(kAppTitle, state.workoutDisplayName());
    } else {
        title = QStringLiteral("   ") + kAppTitle;
    }
    setWindowTitle(title);
    titleBar_->setTitle(title);
    if (auto* synopsis = qobject_cast<SynopsisScreen*>(shown)) {
        QTimer::singleShot(0, synopsis, &SynopsisScreen::layoutMaxLabels);
    }
    emit viewChanged(name);
}

QSize DisplayWindow::requiredSize(const QString& name) const {
    QWidget* widget = view(name);
    return widget ? layout::requiredSize(widget) : QSize();
}

void DisplayWindow::play() {
    HudScreen* hud = workoutHud();
    if (!hud || playing_) {
        return;
    }
    showView(QStringLiteral("Workout"));
    hud->playOn(true);
    readingTimer_->start();
    playing_ = true;
    emit playingChanged(true);
}

void DisplayWindow::pause() {
    if (!playing_) {
        return;
    }
    playing_ = false;
    readingTimer_->stop();
    if (HudScreen* hud = workoutHud()) {
        hud->playOn(false);
    }
    emit playingChanged(false);
}

void DisplayWindow::playReading() {
    HudScreen* hud = workoutHud();
    if (!hud || !rider_) {
        pause();
        return;
    }
    hud->playReading(*rider_);
}

void DisplayWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    emit resized();
}

void DisplayWindow::closeEvent(QCloseEvent* event) {
    event->accept();
    QApplication::quit();
}

// ---- PreviewWindow ----

PreviewWindow::PreviewWindow() {
    setWindowTitle(QStringLiteral("Layout Viewer Preview (scaled)"));
    setAlignment(Qt::AlignCenter);
    // (as it is: not through the theme)
    setStyleSheet(QStringLiteral("background:#202020;"));
}

void PreviewWindow::refresh(QWidget* display) {
    const QPixmap picture = display->grab();
    const QRect available = screen()->availableGeometry();
    const QSize target = picture.size().scaled(static_cast<int>(available.width() * 0.95),
                                               static_cast<int>(available.height() * 0.9), Qt::KeepAspectRatio);
    setStyleSheet(QStringLiteral("background:#202020;"));
    setPixmap(picture.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    setWindowTitle(QStringLiteral("Layout Viewer Preview: %1×%2 shown at %3%")
                       .arg(picture.width())
                       .arg(picture.height())
                       .arg(py::round(100.0 * target.width() / std::max(picture.width(), 1))));
    if (!isVisible()) {
        resize(target);
        move(available.topLeft());
        show();
    }
}

}  // namespace awo::viewer
