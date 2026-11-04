#include "FramePacer.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#include <windows.h>
#include <dxgi.h>
#include <processthreadsapi.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "Performance.hpp"

using Microsoft::WRL::ComPtr;

namespace awo {

namespace {

// how long a held frame waits for the screen before it's drawn anyway
constexpr int kLongestWaitMs = 100;
// the tick where there's no refresh to wait for
constexpr int kNoScreenTickMs = 8;
// how often to look for the screen again after that
constexpr int kLookAgainMs = 1000;

ComPtr<IDXGIOutput> outputOf(HMONITOR monitor) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                return output;
            }
        }
    }
    return nullptr;
}

}  // namespace

// (shared with the thread, which is left to finish on its own: a screen that's off could keep it waiting)
struct FramePacer::RefreshClock {
    std::mutex mutex;
    std::condition_variable asked;
    FramePacer* pacer = nullptr;
    bool wanted = false;
    HMONITOR monitor = nullptr;
};

FramePacer::FramePacer(QWidget* window)
    : QObject(window), window_(window), timer_(new QTimer(this)), clock_(std::make_shared<RefreshClock>()) {
    timer_->setSingleShot(true);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &FramePacer::flush);
    window->installEventFilter(this);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    timeBeginPeriod(1);
    clock_->pacer = this;
    std::thread(&FramePacer::runClock, clock_).detach();
}

FramePacer::~FramePacer() {
    {
        const std::lock_guard lock(clock_->mutex);
        clock_->pacer = nullptr;
    }
    clock_->asked.notify_one();
    timeEndPeriod(1);
}

void FramePacer::runClock(std::shared_ptr<RefreshClock> clock) {
    ComPtr<IDXGIOutput> output;
    HMONITOR outputMonitor = nullptr;
    ULONGLONG lookedAt = 0;
    while (true) {
        HMONITOR monitor = nullptr;
        {
            std::unique_lock lock(clock->mutex);
            clock->asked.wait(lock, [&] { return clock->wanted || !clock->pacer; });
            if (!clock->pacer) {
                return;
            }
            clock->wanted = false;
            monitor = clock->monitor;
        }
        if (monitor != outputMonitor || (!output && GetTickCount64() - lookedAt >= kLookAgainMs)) {
            output = outputOf(monitor);
            outputMonitor = monitor;
            lookedAt = GetTickCount64();
        }
        const ULONGLONG started = GetTickCount64();
        if (!output || FAILED(output->WaitForVBlank())) {
            output.Reset();
            Sleep(kNoScreenTickMs);
        } else if (GetTickCount64() == started) {
            // (one that returns at once, not to spin on it)
            Sleep(1);
        }
        const auto at = std::chrono::steady_clock::now();
        const std::lock_guard lock(clock->mutex);
        if (FramePacer* const pacer = clock->pacer) {
            QMetaObject::invokeMethod(pacer, [pacer, at] { pacer->refreshed(at); }, Qt::QueuedConnection);
        }
    }
}

void FramePacer::askForRefresh() {
    {
        const std::lock_guard lock(clock_->mutex);
        clock_->wanted = true;
        clock_->monitor = MonitorFromWindow(reinterpret_cast<HWND>(window_->winId()), MONITOR_DEFAULTTONEAREST);
    }
    clock_->asked.notify_one();
}

bool FramePacer::eventFilter(QObject*, QEvent* event) {
    if (event->type() != QEvent::UpdateRequest || flushing_) {
        return false;
    }
    if (!sinceFlush_.isValid() || sinceFlush_.elapsed() >= 2 * Performance::current().frameMs()) {
        sinceFlush_.start();
        lastRefresh_ = std::chrono::steady_clock::now();
        return false;
    }
    // (Qt won't ask again until it's been answered, see flush)
    if (!held_) {
        held_ = true;
        timer_->start(kLongestWaitMs);
        askForRefresh();
    }
    return true;
}

void FramePacer::refreshed(std::chrono::steady_clock::time_point at) {
    if (!held_) {
        return;
    }
    // a refresh too soon for the frame rate (a screen faster than it) is skipped, but not one that's just a little early:
    // that would halve the rate on a screen that matches it. Counted between refreshes, not from when the last frame got
    // drawn, or one drawn late would cost the next
    if ((at - lastRefresh_) * 5 < std::chrono::milliseconds(Performance::current().frameMs()) * 3) {
        askForRefresh();
        return;
    }
    flush();
    lastRefresh_ = at;
}

void FramePacer::flush() {
    held_ = false;
    timer_->stop();
    sinceFlush_.start();
    lastRefresh_ = std::chrono::steady_clock::now();
    flushing_ = true;
    QEvent event(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(window_, &event);
    flushing_ = false;
}

}  // namespace awo
