#pragma once

// A QApplication that can time every event the UI thread handles, for finding what's slow: each widget's paints, each
// timer's work, the flush to the screen (the window's UpdateRequest). Off, and nothing but a check per event, unless
// AWO_EVENT_TIMING names a file for the report (written as the app quits) or something turns it on (tests\perf\
// perf_ride.cpp rides a workout with it on). See README.md.

#include <QApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>

namespace awo {

class TimedApplication : public QApplication {
public:
    // the frame time an event is counted as too long against (60 frames a second)
    static constexpr qint64 kFrameNs = 16'666'667;
    static constexpr int kReportRows = 40;

    // one kind of event on one kind of object: "AnalogGauge Paint", "QTimer 16 ms in HudScreen Timer"
    struct Row {
        QString what;
        qint64 count = 0;
        // its own time, without the events handled inside it (the window's paint events inside its UpdateRequest)
        qint64 selfNs = 0;
        qint64 worstNs = 0;
        qint64 overFrame = 0;
        // paint events: the pixels asked for
        qint64 area = 0;
    };

    TimedApplication(int& argc, char** argv);

    bool notify(QObject* receiver, QEvent* event) override;

    void setTiming(bool on);
    // from now on: what's been timed so far forgotten (a ride's start-up, say)
    void resetTiming();
    // the most time first
    QList<Row> rows() const;
    double wallSeconds() const;
    // how much of the wall time the UI thread wasn't waiting for something to do
    double busyShare() const;
    QString report(int rows = kReportRows) const;

private:
    bool timing_ = false;
    QHash<QString, Row> rows_;
    QElapsedTimer clock_;
    qint64 startNs_ = 0;
    // time spent waiting since startNs_, and when the wait going on now began (-1: none)
    qint64 idleNs_ = 0;
    qint64 blockedAt_ = -1;
    // the events handled inside the one being handled, and the waits of a nested event loop (a modal dialog's)
    qint64 innerNs_ = 0;
};

}  // namespace awo
