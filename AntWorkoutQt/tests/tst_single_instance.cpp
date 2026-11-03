// Only one copy at a time (SingleInstance). The other copy is played by another thread: a mutex is owned per thread,
// so one in this process has to wait just as another process does.

#include <QTest>

#include <windows.h>

#include <atomic>
#include <future>
#include <thread>

#include "SingleInstance.hpp"

using namespace awo;

namespace {

// A launch elsewhere: everything on its own thread, as the mutex needs
std::future<SingleInstance::Start> launch(const QString& name, double waitSeconds = 10.0) {
    return std::async(std::launch::async, [name, waitSeconds] { return SingleInstance(name, waitSeconds).start(); });
}

// What's come of it, while this thread's event loop runs (the window's show arrives there)
SingleInstance::Start result(std::future<SingleInstance::Start>& launched) {
    QTest::qWaitFor([&launched] { return launched.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, 15000);
    return launched.get();
}

}  // namespace

class TestSingleInstance : public QObject {
    Q_OBJECT

private slots:
    void theFirstCopyStarts() {
        SingleInstance first(QStringLiteral("AwoTestFirst"));
        QCOMPARE(first.start(), SingleInstance::Start::First);
        QCOMPARE(SingleInstance::current(), &first);
    }

    void aSecondLaunchShowsTheRunningCopy() {
        SingleInstance first(QStringLiteral("AwoTestShow"));
        QCOMPARE(first.start(), SingleInstance::Start::First);
        int shown = 0;
        first.answer([&shown] { ++shown; });
        auto second = launch(QStringLiteral("AwoTestShow"));
        QCOMPARE(result(second), SingleInstance::Start::ShownOther);
        QTRY_COMPARE(shown, 1);
    }

    void aLaunchDuringStartupShowsTheCopyOnceItsWindowIsUp() {
        SingleInstance first(QStringLiteral("AwoTestStarting"));
        QCOMPARE(first.start(), SingleInstance::Start::First);
        auto second = launch(QStringLiteral("AwoTestStarting"));
        QCOMPARE(second.wait_for(std::chrono::milliseconds(150)), std::future_status::timeout);
        int shown = 0;
        first.answer([&shown] { ++shown; });
        QCOMPARE(result(second), SingleInstance::Start::ShownOther);
        QTRY_COMPARE(shown, 1);
    }

    void aLaunchWaitsForAClosingCopyAndThenStarts() {
        auto first = std::make_unique<SingleInstance>(QStringLiteral("AwoTestClosing"));
        QCOMPARE(first->start(), SingleInstance::Start::First);
        int shown = 0;
        first->answer([&shown] { ++shown; });
        first->stopAnswering();
        auto second = launch(QStringLiteral("AwoTestClosing"));
        QCOMPARE(second.wait_for(std::chrono::milliseconds(150)), std::future_status::timeout);
        first.reset();
        QCOMPARE(result(second), SingleInstance::Start::First);
        QCOMPARE(shown, 0);
    }

    void aCopyThatNeverFinishesClosingIsntWaitedForForever() {
        SingleInstance first(QStringLiteral("AwoTestStuck"));
        QCOMPARE(first.start(), SingleInstance::Start::First);
        auto second = launch(QStringLiteral("AwoTestStuck"), 0.1);
        QCOMPARE(result(second), SingleInstance::Start::TimedOut);
    }

    void aCrashedCopyDoesntGetInTheWay() {
        // (a thread ending without releasing the mutex, as a crash leaves it)
        std::thread([] { CreateMutexW(nullptr, TRUE, L"Local\\AwoTestCrashed.Running"); }).join();
        SingleInstance next(QStringLiteral("AwoTestCrashed"), 0.5);
        QCOMPARE(next.start(), SingleInstance::Start::First);
    }
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_single_instance.moc"
