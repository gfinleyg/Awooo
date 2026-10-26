#pragma once

// LayoutViewer's display window (LayoutViewer.py's DisplayWindow): it stands in for the app's main window, showing the
// real setup (Init), workout (frozen part way through a simulated ride), synopsis, song picker and workout picker
// screens, all built in the mode picked on the Scenario tab (Workout Mode, Free Mode, an FTP test or Musical Cadence
// Mode). No ANT stick: a PreviewBridge stands in for it, and Musical Cadence Mode rides made-up songs (nothing is
// analyzed or played). Play runs the workout screen live from where it's frozen.
//
// PreviewWindow shows it scaled down, for a picked screen size bigger than this one.

#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <memory>
#include <utility>

#include "CadenceWorkout.hpp"

class QTimer;
class QVBoxLayout;

namespace awo {

class CustomTitleBar;
class HudScreen;
class PreviewBridge;
class SetupScreen;
class SimulatedRider;

namespace viewer {

// the view buttons; Workout is the Free Ride screen in Free Mode
extern const QStringList kViews;
// the Scenario tab's modes: the setup screen's mode, and its name
struct Mode {
    QString mode;
    QString name;
};
extern const QList<Mode> kModes;
// how long a Free Mode ride is (the Workout progress is a part of it)
constexpr int kFreeRideSeconds = 45 * 60;
extern const QString kAppTitle;
// the rider's settings when none are saved (and the setup screen's FTP and max heart rate)
constexpr double kViewerWheel = 2.105;
constexpr int kViewerFtp = 250;
constexpr int kViewerMaxHr = 185;

// Musical Cadence Mode without music: made-up songs, each (title, its blocks as (seconds, rpm - 0 is free ride))
extern const QList<std::pair<QString, QList<std::pair<int, int>>>> kViewerSongs;
// the made-up songs as a ride (stands in for music::buildWorkout)
music::CadenceRide viewerRide();

class DisplayWindow : public QMainWindow {
    Q_OBJECT

public:
    DisplayWindow();
    ~DisplayWindow() override;

    // Every view built in this mode (Workout Mode: with this workout file); the synopsis rides the whole workout
    // (Free Mode: kFreeRideSeconds), the workout view progressPercent of the way through
    void build(const QString& mode, const QString& workoutPath, int progressPercent);
    // (Re)builds the Workout view (Free Ride in Free Mode), frozen progressPercent of the way through the ride
    void buildWorkout(int progressPercent);
    void showView(const QString& name);
    // the views key for a view button: Free Mode's Workout is the Free Ride screen
    QString viewKey(const QString& name) const;
    // smallest window the view fits in without anything squeezed below its minimum
    QSize requiredSize(const QString& name) const;

    // the views in the order built: (name, screen)
    const QList<std::pair<QString, QWidget*>>& views() const { return views_; }
    QWidget* view(const QString& name) const;
    QString current() const { return current_; }
    QString mode() const { return mode_; }
    HudScreen* workoutHud() const;
    SetupScreen* setupScreen() const;
    PreviewBridge* bridge() const { return bridge_.get(); }
    CustomTitleBar* titleBar() const { return titleBar_; }

    // Play: the Workout view runs live from where it's frozen, the rider's readings coming once a second
    void play();
    // freezes the Workout view where it's got to
    void pause();
    bool playing() const { return playing_; }
    void setBatteryWarnings(bool show);
    // (takes effect at the next build)
    void setTrainer(bool trainer);

    // false while showing a simulated screen size
    bool maximizedMode = true;

signals:
    void viewChanged(const QString& name);
    void resized();
    void playingChanged(bool playing);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void discard(const QString& name);
    void addView(const QString& name, QWidget* view);
    void playReading();

    std::unique_ptr<PreviewBridge> bridge_;
    CustomTitleBar* titleBar_;
    QVBoxLayout* mainLayout_;
    QList<std::pair<QString, QWidget*>> views_;
    // a key of views_: Free Mode's Workout view is "Free Ride"
    QString current_ = QStringLiteral("Init");
    // the setup screen mode the views are built in (see kModes)
    QString mode_ = QStringLiteral("workout");
    bool playing_ = false;
    QTimer* readingTimer_;
    // the Workout view's rider (played on from where it left off), and Musical Cadence Mode's song showing
    std::unique_ptr<SimulatedRider> rider_;
    QString songTitle_;
};

// Scaled down picture of the display window, for simulated screens bigger than this one
class PreviewWindow : public QLabel {
    Q_OBJECT

public:
    PreviewWindow();
    void refresh(QWidget* display);
};

}  // namespace viewer
}  // namespace awo
