// Fitting the screens to the screen (src/ui/AppLayout), the main window and the small popup windows: scales, gaps,
// sizes, text fitted to its widget, button symbols, and each window built and working offscreen

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QVBoxLayout>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "FakeAnt.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "MainWindow.hpp"
#include "Performance.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"

using namespace awo;

namespace {

// a key press as the app gets it: at the focus widget, else the window
void press(QWidget* window, int key) {
    QWidget* target = QApplication::focusWidget() ? QApplication::focusWidget() : window;
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

}  // namespace

class TestLayout : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { keys::install(qApp); }

    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::setUiScale(1.0);
        Units::current().reset();
        Performance::current().reset();
    }

    void cleanup() {
        theme::setUiScale(1.0);
        dir_.reset();
    }

    void scales() {
        QCOMPARE(layout::scaleFor({1920, 1152}), 1.0);
        QCOMPARE(layout::scaleFor({1920, 1032}), 0.896);
        QCOMPARE(layout::scaleFor({3840, 2112}), 1.833);
        QCOMPARE(layout::scaleFor({200, 100}), layout::kMinScale);
        QCOMPARE(layout::scaleFor({20000, 20000}), layout::kMaxScale);
    }

    void gapsSitOnTheGrid() {
        QCOMPARE(layout::gap(0, 0.5), 0);
        QCOMPARE(layout::gap(-1, 1.0), layout::kDefaultGap);
        QCOMPARE(layout::gap(1, 0.3), 1);  // (a hairline just scales, never to nothing)
        // equal design gaps stay equal; 5 and 6 design px land on the same grid step
        QCOMPARE(layout::gap(5, 1.0), 6);
        QCOMPARE(layout::gap(6, 0.5), 3);
        QCOMPARE(layout::gap(7, 0.5), 4);
    }

    void sizesAndGapsScale() {
        QWidget root;
        auto* box = new QVBoxLayout(&root);
        box->setContentsMargins(10, 10, 10, 10);
        box->setSpacing(8);
        auto* fixed = new QLabel(QStringLiteral("x"));
        fixed->setFixedSize(200, 100);
        box->addWidget(fixed);
        layout::applyLayout(&root, {}, 0.5);
        QCOMPARE(fixed->size(), QSize(100, 50));
        QCOMPARE(box->contentsMargins(), QMargins(5, 5, 5, 5));
        QCOMPARE(box->spacing(), 4);
        // from the design sizes each time, not from the last scale
        layout::applyLayout(&root, {}, 2.0);
        QCOMPARE(fixed->size(), QSize(400, 200));
    }

    void textShrinksToFitItsWidget() {
        QWidget root;
        auto* box = new QVBoxLayout(&root);
        auto* label = new QLabel(QStringLiteral("A rather long piece of text for a small label"));
        theme::setSheet(label, QStringLiteral("font: 20pt 'Arial';"));
        label->setFixedSize(150, 80);
        box->addWidget(label);
        theme::registerFonts();
        root.resize(300, 200);
        root.show();
        QVERIFY(QTest::qWaitForWindowExposed(&root));
        const int before = QFontInfo(label->font()).pixelSize();
        QVERIFY(before > 0);
        layout::fitText(&root);
        QVERIFY(label->property("awoFitPx").toInt() > 0);
        QVERIFY(QFontInfo(label->font()).pixelSize() < before);
        // never under kTextFitMin of its own size
        QVERIFY(QFontInfo(label->font()).pixelSize() >= static_cast<int>(before * layout::kTextFitMin));
        // short text: back to its own size, once the layout has room for it
        layout::setText(label, QStringLiteral("Short"));
        QTRY_COMPARE(QFontInfo(label->font()).pixelSize(), before);
    }

    void symbolButtons() {
        QPushButton button(QStringLiteral("START"));
        layout::setSymbol(&button);
        QVERIFY(button.text().isEmpty() && !button.icon().isNull());
        QCOMPARE(button.toolTip(), QStringLiteral("Start"));
        QCOMPARE(layout::text(&button), QStringLiteral("START"));
        layout::setText(&button, QStringLiteral("PAUSE"));
        QCOMPARE(layout::text(&button), QStringLiteral("PAUSE"));
        QVERIFY(button.text().isEmpty());
        // text without a symbol shows as text
        layout::setText(&button, QStringLiteral("Resume?"));
        QCOMPARE(button.text(), QStringLiteral("Resume?"));
        QVERIFY(button.icon().isNull());
    }

    void dialogsScaleToTheLayout() {
        theme::setUiScale(0.5);
        PerformanceWindow window;
        // its fixed sizes at the layout scale (the slider's minimum width, 320 design px)
        QCOMPARE(window.slider()->minimumWidth(), 160);
    }

    // ---- The windows ----

    void mainWindowTitlesAndExitPrompt() {
        awotest::FakeAnt ant;
        MainWindow window(ant.makeBridge());
        QCOMPARE(window.windowTitle(), QStringLiteral("   Awooo Workout"));
        window.setAppTitle(true);
        // (no workout loaded: just the app's name)
        QCOMPARE(window.windowTitle(), QStringLiteral("   Awooo Workout"));
        QVERIFY(window.titleBar()->settingsButton() != nullptr);
        // the X asks first: the window stays, the prompt shows
        window.close();
        ConfirmExitWindow* prompt = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* found = qobject_cast<ConfirmExitWindow*>(top)) {
                prompt = found;
            }
        }
        QVERIFY(prompt && prompt->isVisible());
        QVERIFY(window.isVisible());
        prompt->noButton()->click();
        QVERIFY(!prompt->isVisible());
    }

    void confirmClearSession() {
        bool cleared = false;
        ConfirmClearSessionWindow window([&cleared] { cleared = true; });
        window.show();
        window.yesButton()->click();
        QVERIFY(cleared && !window.isVisible());
    }

    void logWindowShowsTheLog() {
        log::event(QStringLiteral("TEST"), QStringLiteral("shown in the window"));
        ShowLogsWindow window;
        window.show();
        QVERIFY(window.text()->toPlainText().contains(QStringLiteral("[TEST] shown in the window")));
    }

    void measurementsApplyAsClicked() {
        MeasurementsWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QList<QRadioButton*> radios = window.findChildren<QRadioButton*>();
        QCOMPARE(radios.size(), 6);
        // the kg choice: saved as it's clicked
        radios[1]->click();
        QCOMPARE(Units::current().weight, QStringLiteral("kg"));
        // W steps through the weight row
        press(&window, Qt::Key_W);
        QCOMPARE(Units::current().weight, QStringLiteral("lbs"));
    }

    void performanceAppliesAsTheSliderMoves() {
        PerformanceWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.slider()->setValue(0);
        QCOMPARE(Performance::current().gaugeFps(), 0);
        press(&window, Qt::Key_Right);
        QCOMPARE(Performance::current().gaugeFps(), Performance::kGaugeFpsChoices[1]);
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_MAIN(TestLayout)
#include "tst_layout.moc"
