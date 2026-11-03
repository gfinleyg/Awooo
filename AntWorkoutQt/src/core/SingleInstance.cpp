#include "SingleInstance.hpp"

#include <QDeadlineTimer>
#include <QWinEventNotifier>

#include <windows.h>

namespace awo {

namespace {

SingleInstance* currentInstance = nullptr;
constexpr DWORD kPollMilliseconds = 50;

std::wstring mutexName(const QString& name) {
    return (QStringLiteral("Local\\") + name + QStringLiteral(".Running")).toStdWString();
}

std::wstring showEventName(const QString& name) {
    return (QStringLiteral("Local\\") + name + QStringLiteral(".Show")).toStdWString();
}

}  // namespace

SingleInstance::SingleInstance(const QString& name, double waitSeconds) : name_(name), waitSeconds_(waitSeconds) {
    currentInstance = this;
}

SingleInstance::~SingleInstance() {
    stopAnswering();
    if (mutex_) {
        if (owned_) {
            ReleaseMutex(mutex_);
        }
        CloseHandle(mutex_);
    }
    if (currentInstance == this) {
        currentInstance = nullptr;
    }
}

SingleInstance::Start SingleInstance::start() {
    mutex_ = CreateMutexW(nullptr, FALSE, mutexName(name_).c_str());
    if (!mutex_) {
        // no way to tell: not worth keeping the app from starting
        return Start::First;
    }
    QDeadlineTimer deadline(static_cast<qint64>(waitSeconds_ * 1000));
    while (true) {
        // (abandoned: the other copy crashed, and is gone just the same)
        const DWORD waited = WaitForSingleObject(mutex_, kPollMilliseconds);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
            owned_ = true;
            return Start::First;
        }
        // Held, and the other copy has its window up
        if (HANDLE other = OpenEventW(EVENT_MODIFY_STATE, FALSE, showEventName(name_).c_str())) {
            // (a launch may come to the front, and may let the copy it's asking do so)
            AllowSetForegroundWindow(ASFW_ANY);
            SetEvent(other);
            CloseHandle(other);
            return Start::ShownOther;
        }
        // Held, and no window: the other copy is still starting or already closing
        if (deadline.hasExpired()) {
            return Start::TimedOut;
        }
    }
}

void SingleInstance::answer(std::function<void()> show) {
    showEvent_ = CreateEventW(nullptr, FALSE, FALSE, showEventName(name_).c_str());
    if (!showEvent_) {
        return;
    }
    notifier_ = new QWinEventNotifier(showEvent_, this);
    connect(notifier_, &QWinEventNotifier::activated, this, [show = std::move(show)] { show(); });
}

void SingleInstance::stopAnswering() {
    delete notifier_;
    notifier_ = nullptr;
    if (showEvent_) {
        CloseHandle(showEvent_);
        showEvent_ = nullptr;
    }
}

SingleInstance* SingleInstance::current() {
    return currentInstance;
}

}  // namespace awo
