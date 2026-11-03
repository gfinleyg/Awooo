// TimedApplication: an event's own time without the events handled inside it, what its rows are called, nothing timed
// while it's off, the UI thread's waits not counted as busy, and the report AWO_EVENT_TIMING asks for.

#include <QEventLoop>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QtTest>

#include <cstdio>
#include <optional>

#include "TimedApplication.hpp"

using namespace awo;

// (no Q_OBJECT: still called by its own name)
class Sleeper : public QObject {
public:
    int sleepMs = 0;
    QObject* inside = nullptr;
    bool deleteSelf = false;

protected:
    bool event(QEvent* event) override {
        if (event->type() != QEvent::User) {
            return QObject::event(event);
        }
        QThread::msleep(sleepMs);
        if (inside) {
            QEvent nested(QEvent::User);
            QCoreApplication::sendEvent(inside, &nested);
        }
        if (deleteSelf) {
            delete this;
        }
        return true;
    }
};

// (an event loop that really waits: QTest::qWait keeps waking up to sleep)
static void waitInEventLoop(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// as a modal dialog's event loop inside a click
class Waiter : public QObject {
protected:
    bool event(QEvent* event) override {
        if (event->type() != QEvent::User) {
            return QObject::event(event);
        }
        waitInEventLoop(200);
        return true;
    }
};

class Painted : public QWidget {
protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), Qt::red); }
};

static TimedApplication* timedApp() {
    return static_cast<TimedApplication*>(QCoreApplication::instance());
}

static std::optional<TimedApplication::Row> rowFor(const QString& what) {
    for (const TimedApplication::Row& row : timedApp()->rows()) {
        if (row.what == what) {
            return row;
        }
    }
    return std::nullopt;
}

class TstEventTiming : public QObject {
    Q_OBJECT

private slots:
    void init() {
        timedApp()->setTiming(true);
        timedApp()->resetTiming();
    }

    void anEventsOwnTimeLeavesOutTheEventsInsideIt() {
        Sleeper inner;
        inner.sleepMs = 200;
        Sleeper outer;
        outer.sleepMs = 20;
        outer.inside = &inner;
        outer.setObjectName(QStringLiteral("outer"));
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(&outer, &event);
        const auto outerRow = rowFor(QStringLiteral("Sleeper (outer) User"));
        const auto innerRow = rowFor(QStringLiteral("Sleeper User"));
        QVERIFY(outerRow);
        QVERIFY(innerRow);
        QCOMPARE(innerRow->count, 1);
        QVERIFY(innerRow->selfNs >= 190'000'000);
        QCOMPARE(innerRow->overFrame, 1);
        QVERIFY(outerRow->selfNs >= 15'000'000);
        QVERIFY2(outerRow->selfNs < 150'000'000, qPrintable(QString::number(outerRow->selfNs)));
    }

    void aReceiverDeletedByItsEventIsStillCounted() {
        auto* sleeper = new Sleeper;
        sleeper->deleteSelf = true;
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(sleeper, &event);
        QVERIFY(rowFor(QStringLiteral("Sleeper User")));
    }

    void timersAreToldApartByIntervalAndOwner() {
        Sleeper owner;
        QTimer timer(&owner);
        timer.setInterval(5);
        timer.start();
        QTRY_VERIFY(rowFor(QStringLiteral("QTimer 5 ms in Sleeper Timer")));
    }

    void paintsCountTheirPixels() {
        Painted widget;
        widget.resize(50, 40);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        const QString what = QStringLiteral("Painted Paint");
        QTRY_VERIFY(rowFor(what));
        QVERIFY(rowFor(what)->area >= 50 * 40);
    }

    void aPlainWidgetIsCalledByItsParent() {
        Painted parent;
        auto* plain = new QWidget(&parent);
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(plain, &event);
        QVERIFY(rowFor(QStringLiteral("QWidget in Painted User")));
    }

    void nothingIsTimedWhileOff() {
        timedApp()->setTiming(false);
        Sleeper sleeper;
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(&sleeper, &event);
        QVERIFY(timedApp()->rows().isEmpty());
    }

    void waitingIsNotBusy() {
        waitInEventLoop(300);
        const double waiting = timedApp()->busyShare();
        QVERIFY2(waiting < 0.5, qPrintable(QString::number(waiting)));
        timedApp()->resetTiming();
        QThread::msleep(100);
        const double working = timedApp()->busyShare();
        QVERIFY2(working > 0.9, qPrintable(QString::number(working)));
    }

    void aWaitInsideAnEventIsNotItsOwnTime() {
        Waiter waiter;
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(&waiter, &event);
        const auto row = rowFor(QStringLiteral("Waiter User"));
        QVERIFY(row);
        QVERIFY2(row->selfNs < 100'000'000, qPrintable(QString::number(row->selfNs)));
    }

    void theReportListsTheMostTimeFirst() {
        Sleeper slow;
        slow.sleepMs = 30;
        slow.setObjectName(QStringLiteral("slow"));
        Sleeper quick;
        quick.setObjectName(QStringLiteral("quick"));
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(&quick, &event);
        QCoreApplication::sendEvent(&slow, &event);
        const QString report = timedApp()->report();
        QVERIFY(report.contains(QStringLiteral("UI thread busy")));
        QVERIFY(report.indexOf(QStringLiteral("(slow)")) < report.indexOf(QStringLiteral("(quick)")));
        QVERIFY(timedApp()->report(1).contains(QStringLiteral("more)")));
    }
};

int main(int argc, char* argv[]) {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("timing.txt"));
    qputenv("AWO_EVENT_TIMING", file.toUtf8());
    TimedApplication app(argc, argv);
    TstEventTiming test;
    int code = QTest::qExec(&test, argc, argv);
    // the report written as the app quits
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    app.exec();
    QFile report(file);
    if (!report.open(QIODevice::ReadOnly) || !report.readAll().contains("UI thread busy")) {
        std::fprintf(stderr, "AWO_EVENT_TIMING's report wasn't written\n");
        code = 1;
    }
    return code;
}

#include "tst_event_timing.moc"
