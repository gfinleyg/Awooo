// LayoutViewer (src/viewer) built offscreen: every view in every mode on the stand-in bridge, the views shown and
// their titles, Play, the screen sizes and the fit report, the font and color lists changing the theme live, what a
// click on the display identifies, the example message, a screenshot, and theme files saved and loaded.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>

#include "AnalogGauge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "BlockHighlight.hpp"
#include "CustomTitleBar.hpp"
#include "GradeWidgets.hpp"
#include "HaloLabel.hpp"
#include "HudScreen.hpp"
#include "LongVLabels.hpp"
#include "MaxStar.hpp"
#include "MusicSelectDialog.hpp"
#include "PreviewRide.hpp"
#include "SetupScreen.hpp"
#include "SoapBubble.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ZoneLegend.hpp"
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
                // with the blocks ridden so far's bubbles on the graph
                QVERIFY(!display.workoutHud()->blockBubbles().isEmpty());
                // the lit block's slide and swell, as the app has them
                QVERIFY(display.workoutHud()->blockHighlight());
                // and the coming up list's blocks gliding up as one finishes
                QVERIFY(display.workoutHud()->remainingList()->sliding());
                // (the ride shown at rest: no Up Next box held part way to the current block's place, whatever the
                // point in the block it stops at; playing it, the boxes move ahead of each block's end as the app's do)
                QVERIFY(!display.workoutHud()->upNextHandoffHeld());
                QVERIFY(!display.workoutHud()->upNextLeaving());
            } else {
                QVERIFY(display.workoutHud()->blockBubbles().isEmpty());
                QVERIFY(!display.workoutHud()->blockHighlight());
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
            if (BlockHighlight* lit = display.workoutHud()->blockHighlight()) {
                // over the block's labels, wherever the layout puts them (once the window's up: a hidden one's moves
                // and resizes wait for it)
                display.show();
                QApplication::processEvents();
                QCOMPARE(lit->parentWidget(), display.workoutHud());
                QVERIFY(lit->isVisible());
                QVERIFY(lit->geometry().contains(display.workoutHud()->blockLabel()->parentWidget()->geometry()));
                display.hide();
            }
        }
        // Musical Cadence Mode: the made up songs, the song playing in the title
        display.build(QStringLiteral("cadence"), kWorkoutFile, 40);
        display.showView(QStringLiteral("Workout"));
        QCOMPARE(appState().musicTracks.size(), kViewerSongs.size());
        QVERIFY2(titleText(display).contains(QStringLiteral(": The Layouts - Tempo Change : ")), qPrintable(titleText(display)));
        // Workout Mode: the workout's name in the title on the workout screen and the synopsis
        display.build(QStringLiteral("workout"), kWorkoutFile, 40);
        display.showView(QStringLiteral("Synopsis"));
        QCOMPARE(titleText(display), QStringLiteral("   Awooo - ") + appState().workoutDisplayName());
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

    // the music picker's made up songs: one from an older version, one unreadable, both reprocessed (test_screens.py)
    void musicPickerReprocesses() {
        DisplayWindow display;
        display.build(QStringLiteral("cadence"), kWorkoutFile, 40);
        auto* picker = qobject_cast<MusicSelectDialog*>(display.view(QStringLiteral("Music Picker")));
        QVERIFY(picker);
        QCOMPARE(picker->list()->item(3)->text(), QStringLiteral("Some Artist - Unchecked Song   4m 51s   (older version)"));
        QCOMPARE(picker->summary(), QStringLiteral("Ride: 3 songs, 10m 51s   ·   1 can't be read   ·   1 from an older version"
                                                   "   ·   2 missing songs removed   ·   1 playlist entry not found"));
        QVERIFY(picker->reprocessButton()->isEnabled());
        picker->reprocessButton()->click();
        QVERIFY(!picker->list()->item(3)->text().endsWith(QStringLiteral("(older version)")));
        QVERIFY(!picker->summary().contains(QStringLiteral("older version")));
        // (the broken one still can't be read: it can be tried again)
        QVERIFY(picker->summary().contains(QStringLiteral("1 can't be read")) && picker->reprocessButton()->isEnabled());
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

    // frozen well into the workout's last block, the grades show with its still to come
    void lastBlockGradesShowInTheWorkoutView() {
        DisplayWindow display;
        display.build(QStringLiteral("workout"), kWorkoutFile, 99);
        HudScreen* hud = display.workoutHud();
        QVERIFY(hud->engine().started());
        QVERIFY(hud->showingGrades());
        const QList<GradeList::Row> rows = hud->gradeList()->rows();
        QVERIFY(std::any_of(rows.begin(), rows.end(), [](const GradeList::Row& row) { return row.grade == grading::kPendingGrade; }));
        QVERIFY(hud->gradeList()->blinking());
        QVERIFY(hud->gradeList()->averagePending());
        // part way through, the column as usual
        display.build(QStringLiteral("workout"), kWorkoutFile, 40);
        QVERIFY(!display.workoutHud()->showingGrades());
    }

    // the side gauges follow the layout's scaling on any screen, in both workout modes: one size, on one line, mirrored
    // about the middle gauge, and the bubbles and legend beside them clear of them
    void sideGaugesFitEveryScale() {
        DisplayWindow display;
        for (const QString& mode : {QStringLiteral("workout"), QStringLiteral("cadence")}) {
            display.build(mode, kWorkoutFile, 40);
            for (const double scale : {1.0, 0.667, 1.833}) {
                QWidget holder;
                auto* holderLayout = new QVBoxLayout(&holder);
                auto* hud = new HudScreen(display.bridge(), nullptr, [] { return 1000.0; });
                holderLayout->addWidget(hud);
                layout::applyLayout(&holder, {{QStringLiteral("Workout"), hud}}, scale);
                holder.setAttribute(Qt::WA_DontShowOnScreen);
                holder.resize(static_cast<int>(1920 * scale), static_cast<int>(1116 * scale));
                holder.show();
                for (int i = 0; i < 6; ++i) {
                    QCoreApplication::processEvents();
                }
                hud->stopTimers();
                const Gauge* left = mode == u"cadence" ? hud->powerGauge() : hud->cadenceGauge();
                const Gauge* middleGauge = mode == u"cadence" ? hud->cadenceGauge() : hud->powerGauge();
                const Gauge* right = hud->heartRateGauge();
                const QString where = QStringLiteral("%1 at %2: %3 across").arg(mode).arg(scale).arg(left->width());
                QVERIFY2(left->width() > 200 * scale, qPrintable(where));
                QVERIFY2(left->geometry().size() == right->geometry().size(), qPrintable(where));
                QVERIFY2(left->geometry().center().y() == right->geometry().center().y(), qPrintable(where));
                const QRect middle(middleGauge->gauge()->mapTo(hud, QPoint(0, 0)), middleGauge->gauge()->size());
                const int fromLeft = middle.center().x() - left->geometry().center().x();
                const int fromRight = right->geometry().center().x() - middle.center().x();
                QVERIFY2(std::abs(fromLeft - fromRight) <= 2, qPrintable(where));
                const auto clear = [&](const Gauge* gauge, const QWidget* widget) {
                    const QRect rect(widget->mapTo(hud, QPoint(0, 0)), widget->size());
                    const QPoint at = gauge->geometry().center();
                    const int dx = std::max({rect.left() - at.x(), 0, at.x() - rect.right()});
                    const int dy = std::max({rect.top() - at.y(), 0, at.y() - rect.bottom()});
                    return std::hypot(dx, dy) >= gauge->width() / 2.0;
                };
                QVERIFY2(clear(left, hud->findChild<ZoneLegend*>()), qPrintable(where));
                QVERIFY2(clear(right, hud->remainingTimeLabel()), qPrintable(where));
                QList<QFrame*> bubbles = hud->findChildren<QFrame*>(QStringLiteral("infoBubble"));
                std::sort(bubbles.begin(), bubbles.end(), [](const QFrame* a, const QFrame* b) { return a->y() < b->y(); });
                // spaced like the timer rows above them, and as tall at least, so the same radius rounds them
                const int timerGap = hud->workoutTimeLabel()->parentWidget()->layout()->spacing();
                const int timerHeight = hud->workoutTimeLabel()->height();
                for (qsizetype i = 0; i < bubbles.size(); ++i) {
                    QVERIFY2(clear(right, bubbles[i]), qPrintable(where));
                    const QString row = where + QStringLiteral(": bubble %1 is %2 tall (timers %3)").arg(i).arg(bubbles[i]->height()).arg(timerHeight);
                    QVERIFY2(bubbles[i]->height() >= timerHeight, qPrintable(row));
                    if (i > 0) {
                        const int rowGap = bubbles[i]->geometry().top() - bubbles[i - 1]->geometry().bottom() - 1;
                        QVERIFY2(rowGap == timerGap, qPrintable(where + QStringLiteral(": gap %1, timers' %2").arg(rowGap).arg(timerGap)));
                    }
                }
            }
        }
    }

    // switching modes back and forth, the screens being replaced still getting their last events; and every mode's
    // screens fit every screen size
    void modesSwitchedBackAndForthFitEveryScreen() {
        // (with the fonts and theme the viewer starts with: what fits depends on them)
        QFile::copy(awotest::pythonAppDir() + QStringLiteral("/defaultTheme.json"), QStringLiteral("defaultTheme.json"));
        theme::registerFonts();
        theme::clear();
        DisplayWindow display;
        ControlWindow controls(&display, {});
        // (on the main screen, as main() puts it: not whichever the window system picks)
        controls.setHomeScreen(QGuiApplication::primaryScreen());
        display.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft());
        controls.rebuild();
        display.show();
        // (red for a moment in the report counts too)
        QStringList flashes;
        bool watching = true;
        const auto watch = [&](const QString& when) {
            const QString text = controls.fitLabel()->text();
            if (watching && text.contains(QStringLiteral("#b00020")) && !flashes.contains(when + u' ' + text)) {
                flashes.append(when + u' ' + text);
            }
        };
        const auto settle = [&] {
            for (int i = 0; i < 10; ++i) {
                QCoreApplication::processEvents();
                watch(QStringLiteral("pass %1 of %2:").arg(i).arg(controls.modeCombo()->currentData().toString()));
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        const auto pick = [&](const QString& mode) {
            controls.modeCombo()->setCurrentIndex(controls.modeCombo()->findData(mode));
            watch(QStringLiteral("right after picking %1:").arg(mode));
            settle();
            // (from the workout page, as it's being looked at)
            display.showView(QStringLiteral("Workout"));
            settle();
        };
        for (int round = 0; round < 2; ++round) {
            for (const QString& mode : {QStringLiteral("workout"), QStringLiteral("cadence"), QStringLiteral("free"),
                                        QStringLiteral("cadence")}) {
                pick(mode);
            }
        }
        QVERIFY2(flashes.isEmpty(), qPrintable(flashes.join(QStringLiteral("; "))));
        // (changing the screen's size redoes the layout in steps of its own)
        watching = false;
        QStringList tooBig;
        QStringList bubbleFaults;
        QMap<QString, int> timerGaps;
        // the room kept for a workout message: its two lines, whatever size a screen was built at (any more pushes Ride
        // At up over the middle gauge's units)
        for (const QString& mode : {QStringLiteral("workout"), QStringLiteral("cadence"), QStringLiteral("free")}) {
            pick(mode);
            for (int i = 0; i < controls.screenCombo()->count(); ++i) {
                const QString size = controls.screenCombo()->itemData(i).toString();
                if (size == u"custom") {
                    continue;
                }
                controls.screenCombo()->setCurrentIndex(i);
                settle();
                // (the sizes picked: the screen's own can be under the smallest scale, on a test machine)
                if (const HudScreen* hud = i > 0 ? display.workoutHud() : nullptr) {
                    const QLabel* message = hud->workoutMessageLabel();
                    const int lines = 2 * message->fontMetrics().lineSpacing();
                    QVERIFY2(message->minimumHeight() <= lines * 1.25,
                             qPrintable(QStringLiteral("%1 %2: %3 kept for %4").arg(mode, size).arg(message->minimumHeight()).arg(lines)));
                    // the bubbles under the timers: rounded (Qt squares the corners of a radius over half the height),
                    // and spaced like the timer rows (free ride has none: as the workout screen's at this size)
                    if (const QLabel* timer = hud->workoutTimeLabel()) {
                        timerGaps[size] = timer->parentWidget()->layout()->spacing();
                    }
                    QList<QFrame*> bubbles = hud->findChildren<QFrame*>(QStringLiteral("infoBubble"));
                    std::sort(bubbles.begin(), bubbles.end(), [](const QFrame* a, const QFrame* b) { return a->y() < b->y(); });
                    for (qsizetype row = 0; row < bubbles.size(); ++row) {
                        const QString sheet = bubbles[row]->styleSheet();
                        const int radius = QRegularExpression(QStringLiteral("border-radius: *([0-9]+)px")).match(sheet).captured(1).toInt();
                        if (radius == 0 || bubbles[row]->height() < 2 * radius) {
                            bubbleFaults.append(QStringLiteral("%1 %2 bubble %3: %4 tall, radius %5").arg(mode, size).arg(row).arg(bubbles[row]->height()).arg(radius));
                        }
                        const int rowGap = row > 0 ? bubbles[row]->geometry().top() - bubbles[row - 1]->geometry().bottom() - 1 : 0;
                        if (row > 0 && rowGap != timerGaps.value(size)) {
                            bubbleFaults.append(QStringLiteral("%1 %2 bubble %3: gap %4, timers' %5").arg(mode, size).arg(row).arg(rowGap).arg(timerGaps.value(size)));
                        }
                    }
                }
                for (const ControlWindow::Fit& fit : controls.fitResults()) {
                    if (!fit.fits) {
                        tooBig.append(QStringLiteral("%1 %2 %3 needs %4x%5")
                                          .arg(mode, size, fit.name)
                                          .arg(fit.need.width())
                                          .arg(fit.need.height()));
                    }
                }
            }
        }
        QVERIFY2(bubbleFaults.isEmpty(), qPrintable(bubbleFaults.join(QStringLiteral("; "))));
        QVERIFY2(tooBig.isEmpty(), qPrintable(tooBig.join(QStringLiteral("; "))));
    }

    // the workout graph's bubbles on the screen sizes it previews: live, in one font, readable on a small laptop
    void graphBubblesFollowTheScreenSize() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        display.build(QStringLiteral("workout"), kWorkoutFile, 40);
        display.showView(QStringLiteral("Workout"));
        display.setAttribute(Qt::WA_DontShowOnScreen);
        display.show();
        HudScreen* hud = display.workoutHud();
        const auto shownMax = [hud]() -> MaxBubble* {
            for (MaxBubble* bubble : hud->maxBubbles()) {
                if (bubble->isVisible() && !bubble->popping()) {
                    return bubble;
                }
            }
            return nullptr;
        };
        controls.screenCombo()->setCurrentIndex(controls.screenCombo()->findData(QStringLiteral("1920x1200")));
        QTRY_VERIFY(shownMax() && shownMax()->font().pixelSize() == 10);
        controls.screenCombo()->setCurrentIndex(controls.screenCombo()->findData(QStringLiteral("1280x720")));
        QTRY_VERIFY(shownMax() && shownMax()->font() == MaxBubble::textFont());
        QCOMPARE(shownMax()->font().pixelSize(), MaxBubble::kMinTextPixels);
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
        // and its bubbles' soap film colors, which a theme's colors can change
        for (const QString& color : soap::colors()) {
            QVERIFY(colors.contains(color));
            QVERIFY(controls.usesText(color).contains(QStringLiteral("Workout")));
        }
        controls.setColor(soap::kGlintColor, QStringLiteral("#123456"));
        QCOMPARE(theme::current().colors.value(soap::kGlintColor), QStringLiteral("#123456"));
        QCOMPARE(theme::current().toJson(QStringLiteral("colors"))[QStringLiteral("colors")].toObject()[soap::kGlintColor].toString(),
                 QStringLiteral("#123456"));
        controls.setColor(soap::kGlintColor, std::nullopt);
        QVERIFY(!theme::current().colors.contains(soap::kGlintColor));
        // the heart rate gauge's hearts' colors, and changing one recolors the hearts
        for (const QString& heart : {kHeartPink, kHeartRed, kHeartDark}) {
            QVERIFY(colors.contains(heart));
            QVERIFY(controls.usesText(heart).contains(QStringLiteral("Workout")));
        }
        Gauge* hrGauge = display.workoutHud()->heartRateGauge();
        hrGauge->gauge()->setValue(40);
        QCOMPARE(hrGauge->heartColor(), QColor(kHeartPink));
        controls.setColor(kHeartPink, QStringLiteral("#123456"));
        QTRY_COMPARE(hrGauge->heartColor(), QColor(QStringLiteral("#123456")));
        // (the gauge's outer arc too)
        QTRY_COMPARE(hrGauge->gauge()->scalePolygonColors().front().second, QColor(QStringLiteral("#123456")));
        controls.setColor(kHeartPink, std::nullopt);
        QTRY_COMPARE(hrGauge->heartColor(), QColor(kHeartPink));
        // the zone colors are the FTP Colors tab's, not on this list unless something else uses them
        QVERIFY(!colors.contains(zones::zoneColors()[2].toLower()));
        // a color changed: everywhere it's used, once the restyle runs
        controls.setColor(QStringLiteral("#9fc5d1"), QStringLiteral("#123456"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#123456"));
        QTRY_VERIFY(display.styleSheet().contains(QStringLiteral("#123456")));
        controls.setColor(QStringLiteral("#9fc5d1"), std::nullopt);
        QVERIFY(!theme::current().colors.contains(QStringLiteral("#9fc5d1")));
        QTRY_VERIFY(!display.styleSheet().contains(QStringLiteral("#123456")));
        // the Up Next headers' text is haloed where it's lost in the gradient, and not once it stands out
        const QList<HaloLabel*> headers = display.view(QStringLiteral("Workout"))->findChildren<HaloLabel*>();
        QVERIFY(headers.size() >= 2);
        HaloLabel* upNext = headers[0];
        QVERIFY(upNext->haloed());
        controls.setColor(QStringLiteral("#000000"), QStringLiteral("#ffffff"));
        QTRY_VERIFY(!upNext->haloed());
        controls.setColor(QStringLiteral("#000000"), std::nullopt);
        QTRY_VERIFY(upNext->haloed());
        // as are the setup screen's device headers
        const QList<HaloLabel*> setupHeaders = display.view(QStringLiteral("Init"))->findChildren<HaloLabel*>();
        QCOMPARE(setupHeaders.size(), 2);
        QVERIFY(setupHeaders[0]->text().contains(QStringLiteral("Required Power Device")));
        QVERIFY(setupHeaders[1]->text().startsWith(QStringLiteral("Optional")));
    }

    // the workout's intervals with their caps in the setup screen's preview, the caps' zone colors left to the FTP
    // Colors tab (C++ only)
    void intervalCapsShowInThePreview() {
        DisplayWindow display;
        ControlWindow controls(&display, {});
        controls.rebuild();
        display.showView(QStringLiteral("Init"));
        display.show();
        QApplication::processEvents();
        LongVLabels* preview = display.setupScreen()->workoutDisplay();
        preview->renderLines();
        QList<QLabel*> caps;
        for (QLabel* cap : preview->caps()) {
            if (cap->isVisibleTo(preview)) {
                caps.append(cap);
            }
        }
        QVERIFY(!caps.isEmpty());
        QVERIFY2(caps.front()->text().endsWith(u'x'), qPrintable(caps.front()->text()));
        QVERIFY(caps.front()->property("awoFtpColored").toBool());
        QVERIFY(!controls.listedColors().contains(zones::zoneColors()[0].toLower()));
        display.hide();
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
