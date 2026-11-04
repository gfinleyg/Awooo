#pragma once

// A window drawn in step with its screen's refresh, at most once a frame (Performance::frameMs: 60 times a second, or
// 120; a screen slower than that sets the pace). Each update() on a clean window has Qt paint and flush it straight
// away, so animations on timers of their own (the workout screen's bars, stars, needles, heartbeat, gear and scrolling
// labels) each got a paint and a flush of their own, about 230 a second. Held back here until the screen's next
// refresh, the changes in between all land in one. Nothing is lost: Qt asks once and keeps what's dirty until it's
// answered. An update after a quiet spell doesn't wait.
//
// The waiting's done in a thread of its own, on the monitor the window's on (DwmFlush won't do: with screens of different
// rates it keeps to the fastest). Where there's no refresh to wait for (a remote session, say) it ticks on a timer.
//
// While it lives, the app's timers run to the millisecond, even when Windows doesn't count the window as active (one
// behind a video, say): otherwise Windows slows them, and the animations with them.

#include <QElapsedTimer>
#include <QObject>

#include <chrono>
#include <memory>

class QTimer;
class QWidget;

namespace awo {

class FramePacer : public QObject {
public:
    // lives as long as the window
    explicit FramePacer(QWidget* window);
    ~FramePacer() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct RefreshClock;

    static void runClock(std::shared_ptr<RefreshClock> clock);
    void askForRefresh();
    void refreshed(std::chrono::steady_clock::time_point at);
    void flush();

    QWidget* window_;
    // in case the screen never gets there (turned off, say)
    QTimer* timer_;
    QElapsedTimer sinceFlush_;
    // the refresh the last frame was drawn for
    std::chrono::steady_clock::time_point lastRefresh_;
    bool flushing_ = false;
    bool held_ = false;
    std::shared_ptr<RefreshClock> clock_;
};

}  // namespace awo
