// LayoutViewer (src/viewer) built offscreen: every view in every mode on the stand-in bridge, the views shown and
// their titles, Play, the screen sizes and the fit report, the font and color lists changing the theme live, what a
// click on the display identifies, the example message, a screenshot, and theme files saved and loaded.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QtTest>

#include "AnalogGauge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "CustomTitleBar.hpp"
#include "HudScreen.hpp"
#include "MaxStar.hpp"
#include "PreviewRide.hpp"
#include "SetupScreen.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ViewerControls.hpp"
#include "ViewerDisplay.hpp"
#include "ZoneColors.hpp"

using namespace awo;
using namespace awo::viewer;

namespace {

const QString kWorkoutFile = QStringLiteral("awoWorkouts/0_Little_bit_of_everything.awo");

QStringList viewNames(const DisplayWindow& display) {
    QStringList names;
    for (const auto& [name, view] : display.views()) {
        names.append(name);
    }
    return names;
}

QString titleText(const DisplayWindow& display) {
    return display.windowTitle();
}

}  // namespace

class TestLayoutViewer : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::forgetFiles();
        theme::setUiScale(1.0);
        appState() = AppState();
        QDir().mkpath(QStringLiteral("awoWorkouts"));
        QFile::copy(awotest::pythonAppDir() + QStringLiteral("/") + kWorkoutFile, kWorkoutFile);
        QFile::copy(awotest::pythonAppDir() + QStringLiteral("/awoWorkouts/04_Z2_3.awo"),
                    QStringLiteral("awoWorkouts/04_Z2_3.awo"));
    }

    void cleanup() {
        theme::current().reset();
        theme::forgetFiles();
        dir_.reset();
    }

    void buildsEveryViewInEveryMode() {
        DisplayWindow display;
        for (const Mode& mode : kModes) {
            display.build(mode.mode, kWorkoutFile, 40);
            const QString workout = mode.mode == u"free" ? QStringLiteral("Free Ride") : QStringLiteral("Workout");
            QCOMPARE(viewNames(display), (QStringList{"Workout Picker", "Music Picker", "Init", "Synopsis", workout}));
            QVERIFY(display.workoutHud());
            QCOMPARE(display.workoutHud()->cadenceMode(), mode.mode == u"cadence");
            QCOMPARE(appState().workoutMode, mode.mode != u"free");
            // the workout view part way through (a ride's own state: the setup screen's FTP and the rider's)
            if (mode.mode != u"free") {
                QVERIFY(display.workoutHud()->engine().workoutSecondsPassed() > 0);
            } else {
                QVERIFY(appState().secondsPassed >= kFreeRideSeconds * 0.4);
            }
            display.showView(QStringLiteral("Init"));
            QVERIFY(display.setupScreen()->isVisibleTo(&display));
            QCOMPARE(titleText(display), QStringLiteral("   Awooo Workout"));
            // Workout is the Free Ride screen in Free Mode
            display.showView(QStringLiteral("Workout"));
            QCOMPARE(display.current(), workout);
            QVERIFY(display.workoutHud()->isVisibleTo(&display));
            QVERIFY(!display.setupScreen()->isVisibleTo(&display));
        }
        // Musical Cadence Mode: the made up songs, the song playing in the title
        display.build(QStringLiteral("cadence"), kWorkoutFile, 40);
        display.showView(QStringLiteral("Workout"));
        QCOMPARE(appState().musicTracks.size(), kViewerSongs.size());
        QVERIFY2(titleText(display).contains(QStringLiteral(": The Layouts - Tempo Change : ")), qPrintable(titleText(display)));
        // Workout Mode: the workout's name in the title on the workout screen and the synopsis
        display.build(QStringLiteral("workout"), kWorkoutFile, 40);
        display.showView(QStringLiteral("Synopsis"));
        QCOMPARE(titleText(display), QStringLiteral("   Awooo Workout: ") + appState().workoutDisplayName());
        display.showView(QStringLiteral("Workout Picker"));
        QCOMPARE(titleText(display), QStringLiteral("   ") + display.view(QStringLiteral("Workout Picker"))->windowTitle().trimmed());
        // the pickers as screens of the window: no title bars of their own
        QVERIFY(!display.view(QStringLiteral("Workout Picker"))->findChild<CustomTitleBar*>()->isVisibleTo(&display));
        QVERIFY(!display.view(QStringLiteral("Workout Picker"))->isWindow());
        // the setup screen's START goes to the workout screen
        display.showView(QStringLiteral("Init"));
        emit display.setupScreen()->startRequested();
        QCOMPARE(display.current(), QStringLiteral("Workout"));
    }

    void playRunsTheWorkoutLive() {
        DisplayWindow display;
        display.build(QStringLiteral("workout"), kWorkoutFile, 40);
        HudScreen* hud = display.workoutHud();
        const double before = hud->engine().workoutSecondsPassed();
        QSignalSpy playing(&display, &DisplayWindow::playingChanged);
        display.play();
        QVERIFY(display.playing());
        QTRY_VERIFY_WITH_TIMEOUT(hud->engine().workoutSecondsPassed() > before + 1, 5000);
        // the rider's readings once a second
        QTRY_VERIFY_WITH_TIMEOUT(appState().savedPowerVals.times.last() > before + 1, 5000);
        // another view pauses it
        display.showView(QStringLiteral("Init"));
        QVERIFY(!display.playing());
        QCOMPARE(playing.count(), 2);
        const double frozen = hud->engine().workoutSecondsPassed();
        QTest::qWait(400);
        QCOMPARE(hud->engine().workoutSecondsPassed(), frozen);
    }

    void playsMusicalCadenceModesSilentMusic() {
        DisplayWindow display;
        display.build(QStringLiteral("cadence"), kWorkoutFile, 40);
        HudScreen* hud = display.workoutHud();
        const double before = hud->engine().workoutSecondsPassed();
        display.play();
        // (the workout follows the music's clock)
        QTRY_VERIFY_WITH_TIMEOUT(hud->engine().workoutSecondsPassed() > before + 1, 5000);
        display.pause();
        QVERIFY(*hud->simulatedMusic() > before + 1);
    }

    void screenSizesAndTheFitReport() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        QVERIFY(!controls.building());
        // the app's design size: scale 1, everything fits
        controls.screenCombo()->setCurrentIndex(controls.screenCombo()->findData(QStringLiteral("1920x1200")));
        QCOMPARE(controls.availableSize(), QSize(1920, 1200 - layout::kTaskbarHeight));
        QCOMPARE(theme::uiScale(), 1.0);
        const QList<ControlWindow::Fit> fits = controls.fitResults();
        QCOMPARE(fits.size(), 5);
        for (const ControlWindow::Fit& fit : fits) {
            QVERIFY2(fit.fits, qPrintable(fit.name));
        }
        QTRY_VERIFY(controls.fitLabel()->text().contains(QStringLiteral("<b>1920×1152</b> available, layout scale 100%")));
        QVERIFY(controls.fitLabel()->text().contains(QStringLiteral("▶ ")));
        // a small laptop: scaled down until every screen fits (rule 1)
        controls.screenCombo()->setCurrentIndex(controls.screenCombo()->findData(QStringLiteral("1280x720")));
        QVERIFY(theme::uiScale() <= layout::scaleFor(QSize(1280, 672)));
        for (const ControlWindow::Fit& fit : controls.fitResults()) {
            QVERIFY2(fit.fits, qPrintable(QStringLiteral("%1 needs %2x%3").arg(fit.name).arg(fit.need.width()).arg(fit.need.height())));
        }
        // a custom size, without the taskbar
        controls.screenCombo()->setCurrentIndex(controls.screenCombo()->findData(QStringLiteral("custom")));
        controls.taskbarCheck()->setChecked(false);
        QCOMPARE(controls.availableSize(), QSize(1280, 720));
    }

    void colorsListedAndChangedEverywhere() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        display.showView(QStringLiteral("Workout"));
        const QStringList colors = controls.listedColors();
        // the window's gradient, in every view
        QVERIFY(colors.contains(QStringLiteral("#9fc5d1")));
        QVERIFY(controls.usesText(QStringLiteral("#9fc5d1")).startsWith(QStringLiteral("every view")));
        QVERIFY(controls.listedGradients().contains(Gradient{"#28275e", "#9fc5d1"}));
        // the workout graph's stars, which it paints itself
        QVERIFY(colors.contains(MaxStar::kColor.toLower()));
        QVERIFY(controls.usesText(MaxStar::kColor.toLower()).contains(QStringLiteral("Workout")));
        // the zone colors are the FTP Colors tab's, not on this list unless something else uses them
        QVERIFY(!colors.contains(zones::zoneColors()[2].toLower()));
        // a color changed: everywhere it's used, once the restyle runs
        controls.setColor(QStringLiteral("#9fc5d1"), QStringLiteral("#123456"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#123456"));
        QTRY_VERIFY(display.styleSheet().contains(QStringLiteral("#123456")));
        controls.setColor(QStringLiteral("#9fc5d1"), std::nullopt);
        QVERIFY(!theme::current().colors.contains(QStringLiteral("#9fc5d1")));
        QTRY_VERIFY(!display.styleSheet().contains(QStringLiteral("#123456")));
    }

    void fontsChangeTheTheme() {
        DisplayWindow display;
        ControlWindow controls(&display, theme::registerFonts());
        QCOMPARE(controls.fontFamily(theme::kRoleMain)->currentText(), theme::current().fonts[theme::kRoleMain].family);
        controls.fontSize(theme::kRoleMain)->setValue(120);
        QCOMPARE(theme::current().fonts[theme::kRoleMain].scale, 120.0);
        controls.fontWeight(theme::kRoleButton)->setCurrentText(QStringLiteral("700"));
        QCOMPARE(theme::current().fonts[theme::kRoleButton].weight, QStringLiteral("700"));
        // the gauges always draw bold
        QVERIFY(!controls.fontWeight(theme::kRoleGauge)->isEnabled());
        // reset: back to the app's own, and the controls with it
        controls.resetTheme();
        QCOMPARE(controls.fontSize(theme::kRoleMain)->value(), 100);
        QCOMPARE(theme::current().fonts[theme::kRoleButton].weight, QStringLiteral("Original"));
    }

    void clicksOnTheDisplayIdentifyWhatsThere() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        display.showView(QStringLiteral("Init"));
        controls.tabs()->setCurrentWidget(controls.colorsTab());
        QPushButton* start = display.setupScreen()->startButton();
        // (taken by the viewer: not a click on the button)
        QSignalSpy started(display.setupScreen(), &SetupScreen::startRequested);
        start->setEnabled(true);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(start, &press));
        QVERIFY(controls.statusLabel()->text().startsWith(QStringLiteral("Clicked ")));
        QVERIFY(controls.statusLabel()->text().contains(QStringLiteral("font row \"Buttons + stats\"")));
        QVERIFY(controls.statusLabel()->text().contains(QStringLiteral("; colors ")));
        QCOMPARE(started.count(), 0);
        // the gauges' own colors aren't themeable
        display.showView(QStringLiteral("Workout"));
        controls.identify(display.workoutHud()->powerGauge());
        QVERIFY(controls.statusLabel()->text().contains(QStringLiteral("font row \"Gauge numbers\"; its colors are drawn by the gauge")));
        // on the other tabs clicks go through
        controls.tabs()->setCurrentIndex(0);
        QApplication::sendEvent(start, &press);
        QCOMPARE(started.count(), 0);  // (not taken: the button has it, and a press alone is no click)
        // where a color is used in this view
        controls.showUses(QStringLiteral("#9fc5d1"));
        QVERIFY(controls.statusLabel()->text().contains(QStringLiteral("part(s) of this view")));
    }

    void exampleMessageAndScreenshot() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        controls.messageCheck()->setChecked(true);
        QCOMPARE(display.workoutHud()->workoutMessageLabel()->text(), kExampleMessage);
        // a rebuild keeps it
        controls.progressSlider()->setValue(60);
        QTRY_VERIFY(!controls.building());
        QCOMPARE(display.workoutHud()->workoutMessageLabel()->text(), kExampleMessage);
        controls.messageCheck()->setChecked(false);
        QCOMPARE(display.workoutHud()->workoutMessageLabel()->text(), QString());
        display.showView(QStringLiteral("Synopsis"));
        controls.screenshot();
        QVERIFY(QFile::exists(QStringLiteral("layoutScreenshots/screen_%1x%2_Synopsis.png")
                                  .arg(controls.availableSize().width())
                                  .arg(controls.availableSize().height())));
    }

    // golden/theme.json: the files the Python app saves for the same edits. The viewer's are the same, so either app
    // loads the other's
    void themeFilesAreThePythonAppsOwn() {
        const QJsonObject python = awotest::golden(QStringLiteral("theme.json")).toObject();
        DisplayWindow display;
        ControlWindow controls(&display, theme::registerFonts());
        QComboBox* main = controls.fontFamily(theme::kRoleMain);
        // (a static font: offscreen, a variable one's family isn't named as GDI names it)
        QVERIFY(main->findText(QStringLiteral("Orbitron")) >= 0);
        main->setCurrentText(QStringLiteral("Orbitron"));
        controls.fontSize(theme::kRoleMain)->setValue(120);
        controls.fontSize(theme::kRoleButton)->setValue(85);
        controls.fontWeight(theme::kRoleButton)->setCurrentText(QStringLiteral("700"));
        controls.setColor(QStringLiteral("#9fc5d1"), QStringLiteral("#123456"));
        controls.setColor(QStringLiteral("#000000"), QStringLiteral("#F4F4F8"));
        // (as the FTP Colors tab's swatches set them)
        zones::setZoneColor(2, QStringLiteral("#00aa00"));
        zones::setFreeColor(QStringLiteral("#abcdef"));
        controls.saveThemeTo(QStringLiteral("theme.json"));
        controls.savePartTo(QStringLiteral("colors"), QStringLiteral("colors.json"));
        controls.savePartTo(QStringLiteral("ftpColors"), QStringLiteral("gradients.json"));
        QCOMPARE(theme::readFile(QStringLiteral("theme.json")), python.value(QStringLiteral("theme")).toObject());
        QCOMPARE(theme::readFile(QStringLiteral("colors.json")), python.value(QStringLiteral("colors")).toObject());
        QCOMPARE(theme::readFile(QStringLiteral("gradients.json")), python.value(QStringLiteral("ftpColors")).toObject());
        // and the Python app's files load as the same theme
        const theme::Theme made = theme::current();
        controls.resetTheme();
        awotest::writeFile(QStringLiteral("python.json"),
                           QJsonDocument(python.value(QStringLiteral("theme")).toObject()).toJson());
        controls.loadThemeFrom(QStringLiteral("python.json"));
        QVERIFY(theme::current() == made);
        QCOMPARE(controls.fontSize(theme::kRoleButton)->value(), 85);
        controls.resetTheme();
        awotest::writeFile(QStringLiteral("python-gradients.json"),
                           QJsonDocument(python.value(QStringLiteral("ftpColors")).toObject()).toJson());
        controls.loadPartFrom(QStringLiteral("ftpColors"), QStringLiteral("python-gradients.json"));
        QCOMPARE(zones::zoneColors()[2], QStringLiteral("#00aa00"));
        QCOMPARE(zones::freeColor(), QStringLiteral("#abcdef"));
        zones::setZoneColor(2, std::nullopt);
        zones::setFreeColor(std::nullopt);
    }

    void themeFilesSavedAndLoaded() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.setColor(QStringLiteral("#9fc5d1"), QStringLiteral("#123456"));
        controls.fontSize(theme::kRoleMain)->setValue(90);
        zones::setZoneColor(2, QStringLiteral("#00aa00"));
        controls.saveThemeTo(QStringLiteral("mine.json"));
        controls.savePartTo(QStringLiteral("colors"), QStringLiteral("colors.json"));
        controls.savePartTo(QStringLiteral("ftpColors"), QStringLiteral("gradients.json"));
        controls.saveDefaultTheme(false);
        QVERIFY(QFile::exists(theme::kDefaultThemeFile));
        QCOMPARE(theme::fileKind(theme::readFile(QStringLiteral("colors.json"))), QStringLiteral("colors"));
        QCOMPARE(theme::fileKind(theme::readFile(QStringLiteral("gradients.json"))), QStringLiteral("ftpColors"));
        const theme::Theme saved = theme::current();
        controls.resetTheme();
        QVERIFY(!(theme::current() == saved));
        controls.loadThemeFrom(QStringLiteral("mine.json"));
        QVERIFY(theme::current() == saved);
        QCOMPARE(controls.fontSize(theme::kRoleMain)->value(), 90);
        // just the colors: of a colors file, or out of a whole theme
        controls.resetTheme();
        controls.loadPartFrom(QStringLiteral("colors"), QStringLiteral("mine.json"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#123456"));
        QCOMPARE(theme::current().fonts[theme::kRoleMain].scale, 100.0);
        controls.resetTheme();
        controls.loadPartFrom(QStringLiteral("colors"), QStringLiteral("colors.json"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#123456"));
        // a gradients file has no colors
        controls.loadPartFrom(QStringLiteral("colors"), QStringLiteral("gradients.json"));
        QVERIFY(controls.statusLabel()->text().startsWith(QStringLiteral("Couldn't load gradients.json: it has no colors")));
        zones::setZoneColor(2, std::nullopt);
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_MAIN(TestLayoutViewer)
#include "tst_layout_viewer.moc"
