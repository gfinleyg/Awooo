#pragma once

// Only one AntWorkout at a time: there's one ANT stick, and only one program can have it open. A second launch brings
// up the window of the copy that's running and leaves. If that copy is on its way out (its window gone, still letting
// go of the stick), the launch waits for it to finish and then carries on as the only copy.
//
// Windows objects rather than a lock file: the system lets go of them when a process ends, however it ends, so a crash
// leaves nothing behind for the next launch to trip over.

#include <QObject>
#include <QString>

#include <functional>

class QWinEventNotifier;

namespace awo {

class SingleInstance : public QObject {
    Q_OBJECT

public:
    enum class Start {
        First,       // the only copy now
        ShownOther,  // another copy is running and was asked to show itself: leave
        TimedOut,    // another copy never finished starting or closing: carry on, the stick may stay out of reach
    };

    // well past how long closing takes (ConfirmExitWindow gives the stick 3s, then leaves regardless)
    static constexpr double kWaitSeconds = 15.0;

    explicit SingleInstance(const QString& name = QStringLiteral("AntWorkout"), double waitSeconds = kWaitSeconds);
    // (on the thread that called start(): that thread owns the mutex)
    ~SingleInstance() override;

    Start start();
    // The window is up: from here on, a later launch asking for it calls show
    void answer(std::function<void()> show);
    // Closing: later launches wait for this copy to end instead of asking a window that's gone to show itself
    void stopAnswering();

    // the one made last, if it's still around
    static SingleInstance* current();

private:
    QString name_;
    double waitSeconds_;
    void* mutex_ = nullptr;
    bool owned_ = false;
    void* showEvent_ = nullptr;
    QWinEventNotifier* notifier_ = nullptr;
};

}  // namespace awo
