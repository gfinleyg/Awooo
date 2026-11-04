// Rides a workout live on the real display, with everything on its real timers (LayoutViewer's Play), timing every
// event the UI thread handles, and prints what took the time (TimedApplication). A window opens maximized on the main
// screen for the length of the run. See README.md.
//
//     perf_ride.exe [--seconds 60] [--warmup 5] [--progress 40] [--mode workout] [--workout FILE] [--theme FILE] [--gauge-fps FPS]
//                   [--out FILE]

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QScreen>
#include <QTimer>

#include <cstdio>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AppStyle.hpp"
#include "Performance.hpp"
#include "PreviewRide.hpp"
#include "Theme.hpp"
#include "TimedApplication.hpp"
#include "ViewerDisplay.hpp"
#include "WorkoutHelpers.hpp"

using namespace awo;

int main(int argc, char* argv[]) {
    TimedApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption secondsOption("seconds", "Seconds timed", "seconds", "60");
    const QCommandLineOption warmupOption("warmup", "Seconds ridden first, not timed", "seconds", "5");
    const QCommandLineOption progressOption("progress", "Percent of the way through the ride to start", "percent", "40");
    const QCommandLineOption modeOption("mode", "workout, free, 20m, 1h, ramp10, ramp20 or cadence", "mode", "workout");
    const QCommandLineOption workoutOption("workout", "The workout file (Workout Mode)", "file");
    const QCommandLineOption themeOption("theme", "A theme .json (none: the app's no theme look)", "file");
    const QCommandLineOption fpsOption("gauge-fps", "The Performance setting for this ride (put back after)", "fps");
    const QCommandLineOption outOption("out", "Write the report here too", "file");
    parser.addOptions({secondsOption, warmupOption, progressOption, modeOption, workoutOption, themeOption, fpsOption,
                       outOption});
    parser.process(app);
    // (from where it was started, before moving to the exe's folder)
    const auto startedPath = [&parser](const QCommandLineOption& option) {
        return parser.isSet(option) ? QDir::current().absoluteFilePath(parser.value(option)) : QString();
    };
    const QString out = startedPath(outOption);
    const QString themeFile = startedPath(themeOption);
    const QString chosenWorkout = startedPath(workoutOption);
    const int warmupMs = parser.value(warmupOption).toInt() * 1000;
    const int timedMs = parser.value(secondsOption).toInt() * 1000;

    // settings, workouts and themes are all beside the exe, as the app's are
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    const QString workoutFile = chosenWorkout.isEmpty() ? kPreviewWorkout : chosenWorkout;
    if (parser.value(modeOption) == u"workout" && !QFileInfo(workoutFile).isFile()) {
        std::fprintf(stderr, "No workout file: %s\n", qPrintable(workoutFile));
        return 1;
    }
    // (the views are built with stand-in devices picked: the app's own picks are put back on the way out)
    const QStringList selected = selectedDevices::read();
    const int savedFps = Performance::current().gaugeFps();
    if (parser.isSet(fpsOption)) {
        Performance::current().setGaugeFps(parser.value(fpsOption).toInt());
    }
    theme::setDefaultFont(QApplication::font());
    theme::registerFonts();
    theme::setAppStyleSheet(style::kAppStyleSheet);
    if (themeFile.isEmpty()) {
        theme::clear();
    } else {
        theme::loadFile(themeFile);
    }

    int code = 0;
    {
        viewer::DisplayWindow display;
        display.build(parser.value(modeOption), workoutFile, parser.value(progressOption).toInt());
        QScreen* screen = QGuiApplication::primaryScreen();
        display.setScreen(screen);
        display.move(screen->availableGeometry().topLeft());
        // scaled to the screen, as the app does
        const QSize available = screen->availableGeometry().size();
        QMap<QString, QWidget*> screens;
        for (const auto& [name, view] : display.views()) {
            screens.insert(name, view);
        }
        layout::applyLayout(&display, screens, layout::scaleFor(available), available);
        display.showView(QStringLiteral("Workout"));
        display.showMaximized();

        const auto finish = [&display, &app, out, screen] {
            const QString report = QStringLiteral("Window %1x%2 at device ratio %3, gauge fps %4, %5 at %6 s\n")
                                       .arg(display.width())
                                       .arg(display.height())
                                       .arg(screen->devicePixelRatio())
                                       .arg(Performance::current().gaugeFps())
                                       .arg(display.mode())
                                       .arg(appState().secondsPassed, 0, 'f', 0) +
                                   app.report();
            app.setTiming(false);
            display.pause();
            std::printf("%s", qPrintable(report));
            if (!out.isEmpty()) {
                QFile file(out);
                if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    file.write(report.toUtf8());
                }
            }
            QCoreApplication::quit();
        };
        QTimer::singleShot(500, &display, [&display, &app, warmupMs, timedMs, finish] {
            display.play();
            QTimer::singleShot(warmupMs, &display, [&display, &app, timedMs, finish] {
                app.setTiming(true);
                QTimer::singleShot(timedMs, &display, finish);
            });
        });
        code = app.exec();
    }
    Performance::current().setGaugeFps(savedFps);
    if (selected.isEmpty()) {
        selectedDevices::clear();
    } else {
        selectedDevices::write(selected);
    }
    return code;
}
