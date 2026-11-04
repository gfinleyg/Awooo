// FramePacer: however often a window's asked to update, it's painted about once a frame and no faster than its screen
// refreshes, what was asked for last always gets drawn, and the windows that should have one do.

#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QtTest>

#include <algorithm>
#include <memory>

#include "FramePacer.hpp"
#include "MainWindow.hpp"
#include "Performance.hpp"
#include "PreviewRide.hpp"
#include "ViewerDisplay.hpp"

using namespace awo;

// offscreen (as ctest runs these) there's no refresh to follow, and frames come on FramePacer's own tick
bool hasRealScreens() {
    return QGuiApplication::platformName() != u"offscreen";
}

// (QTest::qWait sleeps between its passes over the events, which would hold up each refresh's frame)
void waitInEventLoop(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
}

class Counted : public QWidget {
public:
    int paints = 0;
    int value = 0;
    int drawnValue = -1;

protected:
    void paintEvent(QPaintEvent*) override {
        ++paints;
        drawnValue = value;
        QPainter(this).fillRect(rect(), Qt::red);
    }
};

class TstFramePacer : public QObject {
    Q_OBJECT

private slots:
    void manyUpdatesLandInAboutOnePaintAFrame_data() {
        QTest::addColumn<int>("fps");
        QTest::newRow("60") << 60;
        QTest::newRow("120") << 120;
    }

    void manyUpdatesLandInAboutOnePaintAFrame() {
        QFETCH(int, fps);
        Performance::current().setGaugeFps(fps);
        Counted window;
        new FramePacer(&window);
        window.resize(100, 100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.paints = 0;
        // several timers out of step with each other, as the workout screen's are
        QList<QTimer*> timers;
        for (const int ms : {1, 2, 3, 5, 7}) {
            auto* timer = new QTimer(&window);
            timer->setTimerType(Qt::PreciseTimer);
            connect(timer, &QTimer::timeout, &window, qOverload<>(&QWidget::update));
            timer->start(ms);
            timers.append(timer);
        }
        QElapsedTimer clock;
        clock.start();
        waitInEventLoop(500);
        const int paints = window.paints;
        const double seconds = clock.elapsed() / 1000.0;
        // (a refresh a little early for the frame still counts: FramePacer::refreshed)
        const double frames = seconds * 1000 / (Performance::current().frameMs() * 0.6);
        const double refreshes = seconds * window.screen()->refreshRate();
        qDeleteAll(timers);
        const QString counts = QStringLiteral("%1 paints, %2 frames, %3 refreshes").arg(paints).arg(frames).arg(refreshes);
        QVERIFY2(paints <= frames + 3, qPrintable(counts));
        QVERIFY2(!hasRealScreens() || paints <= refreshes + 3, qPrintable(counts));
        QVERIFY2(paints >= 5, qPrintable(counts));
        Performance::current().setGaugeFps(60);
    }

    // on whichever screen the window's on, as fast as that one refreshes (with 120 fps asked for, more than a 60 Hz one).
    // Run tst_frame_pacer.exe itself for this, not through ctest
    void framesFollowTheScreenTheWindowIsOn() {
        if (!hasRealScreens()) {
            QSKIP("no real screens");
        }
        Performance::current().setGaugeFps(120);
        for (QScreen* screen : QGuiApplication::screens()) {
            Counted window;
            new FramePacer(&window);
            window.setGeometry(QRect(screen->availableGeometry().topLeft() + QPoint(50, 50), QSize(100, 100)));
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QCOMPARE(window.screen(), screen);
            auto* timer = new QTimer(&window);
            timer->setTimerType(Qt::PreciseTimer);
            connect(timer, &QTimer::timeout, &window, qOverload<>(&QWidget::update));
            timer->start(1);
            waitInEventLoop(100);
            window.paints = 0;
            QElapsedTimer clock;
            clock.start();
            waitInEventLoop(1000);
            const int paints = window.paints;
            const double refreshes = clock.elapsed() / 1000.0 * screen->refreshRate();
            const QString counts = QStringLiteral("%1: %2 paints, %3 refreshes").arg(screen->name()).arg(paints).arg(refreshes);
            qInfo().noquote() << counts;
            QVERIFY2(paints <= refreshes + 3, qPrintable(counts));
            QVERIFY2(paints >= refreshes * 0.7, qPrintable(counts));
        }
        Performance::current().setGaugeFps(60);
    }

    void theLastUpdateAlwaysGetsDrawn() {
        Counted window;
        new FramePacer(&window);
        window.resize(100, 100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // the first goes straight through, the rest wait for the frame
        for (int i = 1; i <= 5; ++i) {
            window.value = i;
            window.update();
            QCoreApplication::processEvents();
        }
        QTRY_COMPARE_WITH_TIMEOUT(window.drawnValue, 5, 200);
        // and with nothing asked for since, nothing's drawn
        QTest::qWait(50);
        const int paints = window.paints;
        QTest::qWait(50);
        QCOMPARE(window.paints, paints);
    }

    void anUpdateAfterAQuietSpellIsDrawnStraightAway() {
        Counted window;
        new FramePacer(&window);
        window.resize(100, 100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);
        window.value = 1;
        window.update();
        QCoreApplication::sendPostedEvents(&window, QEvent::UpdateRequest);
        QCOMPARE(window.drawnValue, 1);
    }

    void aGrabIsNotHeldBack() {
        Counted window;
        new FramePacer(&window);
        window.resize(100, 100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.update();
        QCoreApplication::processEvents();
        window.value = 7;
        window.update();
        window.grab();
        QCOMPARE(window.drawnValue, 7);
    }

    void theAppsWindowAndLayoutViewersDisplayArePaced() {
        const auto paced = [](const QWidget& window) {
            const QObjectList children = window.children();
            return std::any_of(children.begin(), children.end(),
                               [](QObject* child) { return dynamic_cast<FramePacer*>(child) != nullptr; });
        };
        viewer::DisplayWindow display;
        QVERIFY(paced(display));
        MainWindow window(std::make_unique<PreviewBridge>());
        QVERIFY(paced(window));
    }
};

QTEST_MAIN(TstFramePacer)
#include "tst_frame_pacer.moc"
