// The theme picker (src/ui/dialogs/ThemeSelectDialog) over the real main window (tests/test_app_theme_picker.py): its
// colors and gradients lists tried on the app as they're picked, Cancel going back to the look from before, Use This
// Look saving it, and its workout screen preview (RidePreview) ridden in its own AppState and WorkoutData, leaving the
// app's untouched.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QScreen>
#include <QtTest>

#include "AntBridge.hpp"
#include "AppState.hpp"
#include "FakeAnt.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "MainWindow.hpp"
#include "Performance.hpp"
#include "PreviewRide.hpp"
#include "SavedSettings.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ThemeSelectDialog.hpp"
#include "Units.hpp"
#include "WorkoutData.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

const QByteArray kColors = R"({"colors": {"#9fc5d1": "#2a2a3a", "#000000": "#f4f4f8"}})";
// (zone "1" dark navy: it stands out on the default background, but has to be moved on Night City's dark one)
const QByteArray kGradients = R"({"ftpColors": {"zones": {"1": "#112233"}}})";

QStringList texts(const QListWidget* list) {
    QStringList out;
    for (int i = 0; i < list->count(); ++i) {
        out.append(list->item(i)->text());
    }
    return out;
}

void pick(ThemeSelectDialog& dialog, const QString& part, int row) {
    dialog.list(part)->setCurrentRow(row);
    dialog.applyChoice();
}

bool sameSnapshot(const theme::Snapshot& a, const theme::Snapshot& b) {
    return a.data == b.data && a.parts == b.parts && a.file == b.file;
}

}  // namespace

class TestThemePicker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { keys::install(qApp); }

    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::forgetFiles();
        theme::setUiScale(1.0);
        Performance::current().reset();
        Units::current().reset();
        appState() = AppState();
        workoutData().clear();
        QDir().mkpath(QStringLiteral("themes/colors"));
        QDir().mkpath(QStringLiteral("themes/gradients"));
        awotest::writeFile(QStringLiteral("themes/colors/01-night-city.json"), kColors);
        awotest::writeFile(QStringLiteral("themes/gradients/01-plasma.json"), kGradients);
        awotest::writeFile(QStringLiteral("themes/gradients/02-broken.json"), "{not json");
        // the preview's workout
        QDir().mkpath(QStringLiteral("awoWorkouts"));
        QFile::copy(awotest::pythonAppDir() + QStringLiteral("/") + kPreviewWorkout, kPreviewWorkout);
        ant_ = std::make_unique<awotest::FakeAnt>();
        window_ = std::make_unique<MainWindow>(ant_->makeBridge());
    }

    void cleanup() {
        window_.reset();
        ant_.reset();
        theme::current().reset();
        theme::forgetFiles();
        dir_.reset();
    }

    void listsShowTheThemeFiles() {
        ThemeSelectDialog dialog(window_.get(), false);
        QCOMPARE(texts(dialog.list(QStringLiteral("colors"))), (QStringList{"Base Theme's Colors", "Night City"}));
        QCOMPARE(texts(dialog.list(QStringLiteral("ftpColors"))), (QStringList{"Base Theme's Gradients", "Plasma", "Broken"}));
        // picking dark colors: the gradients' swatches fitted to them, a gradient moved to stand out marked
        pick(dialog, QStringLiteral("colors"), 1);
        QListWidgetItem* plasma = dialog.list(QStringLiteral("ftpColors"))->item(1);
        QCOMPARE(plasma->text(), QStringLiteral("Plasma") + themepicker::kAdjustedMark);
        QVERIFY(plasma->toolTip().contains(QStringLiteral("zone 2 moved")));
        QCOMPARE(dialog.list(QStringLiteral("ftpColors"))->item(0)->text(), QStringLiteral("Base Theme's Gradients"));
        pick(dialog, QStringLiteral("colors"), 0);
        QCOMPARE(dialog.list(QStringLiteral("ftpColors"))->item(1)->text(), QStringLiteral("Plasma"));
        // nothing on top of the theme: the base rows picked
        QCOMPARE(dialog.list(QStringLiteral("colors"))->currentRow(), 0);
        QCOMPARE(dialog.list(QStringLiteral("ftpColors"))->currentRow(), 0);
        // it fills the screen
        QCOMPARE(dialog.geometry(), window_->screen()->availableGeometry());
        QCOMPARE(themepicker::displayName(QStringLiteral("05-night-city.json")), QStringLiteral("Night City"));
        QCOMPARE(themepicker::displayName(QStringLiteral("my_theme's-own.json")), QStringLiteral("My Theme'S Own"));
    }

    void picksAreTriedOnAndCancelGoesBack() {
        const theme::Snapshot before = theme::snapshot();
        ThemeSelectDialog dialog(window_.get(), false);
        pick(dialog, QStringLiteral("colors"), 1);
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#2a2a3a"));
        pick(dialog, QStringLiteral("ftpColors"), 1);
        QCOMPARE(zones::zoneColors()[1], QStringLiteral("#112233"));
        // a file that can't be read is marked, and the look stays as it was
        pick(dialog, QStringLiteral("ftpColors"), 2);
        QCOMPARE(zones::zoneColors()[1], QStringLiteral("#112233"));
        QCOMPARE(dialog.list(QStringLiteral("ftpColors"))->item(2)->foreground().color(), QColor(Qt::darkRed));
        // back to the base theme's colors
        pick(dialog, QStringLiteral("colors"), 0);
        QVERIFY(!theme::current().colors.contains(QStringLiteral("#9fc5d1")));
        QVERIFY(!theme::partFiles().contains(QStringLiteral("colors")));
        dialog.reject();
        QVERIFY(sameSnapshot(theme::snapshot(), before));
    }

    void useThisLookSavesIt() {
        ThemeSelectDialog dialog(window_.get(), false);
        pick(dialog, QStringLiteral("colors"), 1);
        pick(dialog, QStringLiteral("ftpColors"), 1);
        dialog.accept();
        const QJsonObject saved = settings::load().value(QStringLiteral("themePartFiles")).toObject();
        QCOMPARE(QFileInfo(saved.value(QStringLiteral("colors")).toString()).fileName(), QStringLiteral("01-night-city.json"));
        QCOMPARE(QFileInfo(saved.value(QStringLiteral("ftpColors")).toString()).fileName(), QStringLiteral("01-plasma.json"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#9fc5d1")), QStringLiteral("#2a2a3a"));
    }

    void previewLeavesTheAppsRideAlone() {
        AppState& state = appState();
        state.workoutFilenameShort = QStringLiteral("app_flow.awo");
        state.workoutDurationSeconds = 50;
        state.secondsPassed = 12.0;
        state.savedPowerVals = {{1.0, 2.0}, {150, 160}};
        workoutData().power.append({150});
        const QList<WorkoutEntry> workoutList = state.workoutList;
        const ftpcolors::ZoneRanges ranges = state.ftpRanges;
        ThemeSelectDialog dialog(window_.get());
        dialog.show();
        dialog.buildPreview();
        QVERIFY(dialog.previewLabel()->pixmap().isNull() == false);
        // the app's own ride just as it was
        QCOMPARE(state.workoutList, workoutList);
        QCOMPARE(state.ftpRanges, ranges);
        QCOMPARE(state.workoutFilenameShort, QStringLiteral("app_flow.awo"));
        QCOMPARE(state.secondsPassed, 12.0);
        QCOMPARE(state.savedPowerVals.values, (QList<double>{150, 160}));
        QCOMPARE(workoutData().power.column(), (QList<int>{150}));
        // a new look redraws it, still apart from the app's ride
        const QImage first = dialog.previewLabel()->pixmap().toImage();
        pick(dialog, QStringLiteral("colors"), 1);
        QVERIFY(dialog.previewLabel()->pixmap().toImage() != first);
        QCOMPARE(state.secondsPassed, 12.0);
        dialog.reject();
    }

    // the preview's own ride: the workout part way through, in its sandbox
    void previewRidesItsOwnWorkout() {
        appState().workoutFilenameShort = QStringLiteral("app_flow.awo");
        RidePreview preview(QSize(1600, 900));
        QVERIFY(preview.hud()->engine().workoutSecondsPassed() > 0);
        QCOMPARE(appState().workoutFilenameShort, QStringLiteral("app_flow.awo"));
        const QPixmap picture = preview.picture();
        QCOMPARE(picture.size(), QSize(1600, 900));
        QCOMPARE(appState().workoutFilenameShort, QStringLiteral("app_flow.awo"));
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<awotest::FakeAnt> ant_;
    std::unique_ptr<MainWindow> window_;
};

QTEST_MAIN(TestThemePicker)
#include "tst_theme_picker.moc"
