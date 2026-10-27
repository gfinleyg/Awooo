// simulant_app: the whole C++ app against SimulANT+ (AntWorkout\SimulantTests\run_app_test.py, PLAN.md Phase 10).
// Where simulant_ride checks the readings on the workout screen in Free Mode, this drives the app the way a rider
// would - the real MainWindow, setup screen, workout screen and synopsis - against the SimulANT+ scripts' steady
// "pairing" values (no timeline):
//
//   trainer   FE-C trainer + heart rate: Workout Mode on the trainer (ERG following the blocks, a ramp, intervals, a
//             block over the Power Drive Cutoff, a difficulty change, a pause), the synopsis, back to setup, exit
//   sensors   power meter + speed/cadence + heart rate: Musical Cadence Mode with music analyzed and played (silently),
//             a shortened 20m FTP test and Apply FTP, Free Mode, exit
//
//   simulant_app RUN_FOLDER [--wait MINUTES]
//
// app_test.py (in this folder) starts it in RUN_FOLDER, having written app_setup.json there (the scenario, the device
// types it needs, the pairing values the scripts send, the trainer's workout or the songs to ride to) and the app's
// settings; the trainer script logs what it was sent to simulant_fe.csv there. The checks and report.txt are this
// one's, as run_app_test.py's are its own.

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSlider>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>

#include "AnalogGauge.hpp"
#include "AnalysisCache.hpp"
#include "AntBridge.hpp"
#include "AppState.hpp"
#include "AppStyle.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "DeviceDialogs.hpp"
#include "Dialogs.hpp"
#include "FtpTest.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "MainWindow.hpp"
#include "MusicPlayer.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "SetupScreen.hpp"
#include "SoundEffects.hpp"
#include "SynopsisScreen.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutGrading.hpp"

using namespace awo;

namespace {

void say(const QString& text) {
    std::printf("%s %s\n", qPrintable(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))), qPrintable(text));
    std::fflush(stdout);
}

double now() {
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

QString rounded(double value, int digits = 1) {
    return py::str(py::round(value, digits));
}

struct Step {
    QString name;
    double seconds;
    std::function<bool()> run;
};

// the test went wrong in a way that ends it (a check can't be made)
struct Abort {
    QString why;
};

}  // namespace

int main(int argc, char* argv[]) {
    QString runDir;
    double waitMinutes = 10;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == u"--wait" && i + 1 < argc) {
            waitMinutes = QString::fromLocal8Bit(argv[++i]).toDouble();
        } else {
            runDir = arg;
        }
    }
    if (runDir.isEmpty()) {
        std::fprintf(stderr, "usage: simulant_app RUN_FOLDER [--wait MINUTES]\n");
        return 2;
    }
    QApplication app(argc, argv);
    // the app's own files (settings, log, caches) in the run folder, as app_test.py left them
    QDir::setCurrent(runDir);
    QFile setupFile(QStringLiteral("app_setup.json"));
    if (!setupFile.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "no app_setup.json in %s\n", qPrintable(runDir));
        return 2;
    }
    const QJsonObject config = QJsonDocument::fromJson(setupFile.readAll()).object();
    const QString scenario = config.value(QStringLiteral("scenario")).toString();
    std::set<int> needs;
    for (const QJsonValue& type : config.value(QStringLiteral("needs")).toArray()) {
        needs.insert(type.toInt());
    }
    const double pairingPower = config.value(QStringLiteral("pairingPower")).toDouble();
    const double pairingSpeedKmh = config.value(QStringLiteral("pairingSpeedKmh")).toDouble();
    const bool trainer = scenario == u"trainer";

    // the same start up as the app's (src/app/main.cpp)
    log::installErrorLogging();
    keys::install(&app);
    sound::init(&app);
    theme::setDefaultFont(QApplication::font());
    theme::setAppStyleSheet(style::kAppStyleSheet);
    theme::clear();
    Units::current().load();
    auto* window = new MainWindow;
    window->show();
    ant::AntBridge* bridge = window->bridge();
    SetupScreen* setup = window->setupScreen();
    const auto hud = [window] { return window->hudScreen(); };
    const auto synopsis = [window] { return window->synopsisScreen(); };

    QList<std::pair<bool, QString>> results;
    const auto check = [&results](bool ok, const QString& text) {
        results.append({ok, text});
        say((ok ? QStringLiteral("PASS  ") : QStringLiteral("FAIL  ")) + text);
    };

    // ---- the test as a list of steps, each run every 100 ms until it returns true (done) or its deadline passes ----
    QList<Step> steps;
    const auto step = [&steps](const QString& name, double seconds, std::function<bool()> run) {
        steps.append({name, seconds, std::move(run)});
    };
    struct Context {
        bool announced = false;
        QString roles;
        double rideStart = 0, hudStart = 0, backAt = 0, synopsisAt = 0, ended = 0;
        std::optional<double> difficulty, paused, resumed, completed, freeStart;
        std::optional<std::pair<double, double>> musicAt;
        QString titleSeen;
        grading::Grades grades, cadenceGrades;
        bool back = false;
    } ctx;

    const auto startRide = [&] {
        // the setup screen's START, then the workout screen's START (workout modes)
        const AppState& state = appState();
        check(setup->startButton()->isEnabled(),
              QStringLiteral("setup START enabled (%1)")
                  .arg(state.workoutMode ? (state.workoutDisplayName().isEmpty() ? QStringLiteral("workout")
                                                                                  : state.workoutDisplayName())
                                         : QStringLiteral("Free Mode")));
        setup->startButton()->click();
        if (appState().workoutMode) {
            hud()->startEndButton()->click();
        }
        ctx.rideStart = now();
    };
    const auto endRide = [&](bool escape) {
        // END (or Esc, which must bring up the same End Workout prompt), then Exit to Synopsis
        if (escape) {
            window->escapePressed();
            ConfirmEndWorkoutWindow* prompt = hud()->endPrompt();
            check(prompt && prompt->isVisible(), QStringLiteral("Esc on the workout screen: End Workout prompt"));
        } else {
            hud()->startEndButton()->click();
        }
        hud()->endPrompt()->synopsisButton()->click();
    };
    const auto backToSetup = [&] {
        // the synopsis' RESTART: back to the setup screen, which disconnects and scans again
        synopsis()->restartButton()->click();
        ctx.backAt = now();
    };
    const auto screens = [&] {
        QStringList showing;
        const std::pair<QString, QWidget*> all[] = {{QStringLiteral("setup"), setup},
                                                    {QStringLiteral("workout"), hud()},
                                                    {QStringLiteral("synopsis"), synopsis()}};
        for (const auto& [name, widget] : all) {
            if (widget && widget->isVisible()) {
                showing.append(name);
            }
        }
        return showing;
    };
    const auto onlySetupShowing = [&] {
        // (checked once the synopsis' own timer a second after it's built has been: it used to show itself again)
        check(screens() == QStringList{QStringLiteral("setup")},
              QStringLiteral("back on setup: only the setup screen showing (%1)").arg(screens().join(QStringLiteral(", "))));
    };
    const auto pressConnect = [&] {
        setup->connectSelectedDevices();
        if (auto* prompt = qobject_cast<ConnectDevicesWindow*>(setup->popup())) {
            prompt->connectButton()->click();
        }
    };
    const auto reconnected = [&] {
        // after going back to setup: connected again (all by itself with a speed/cadence sensor; the trainer test
        // presses Connect Selected Devices as before)
        if (!setup->isVisible()) {
            return false;
        }
        if (trainer && !bridge->isConnectRequested() && bridge->scanSettleRemaining() == 0) {
            pressConnect();
        }
        if (bridge->isReady() && now() - ctx.backAt > 1.5) {
            setup->initAntScanConnect();
            onlySetupShowing();
            return true;
        }
        return false;
    };

    step(QStringLiteral("ANT stick open and scanning"), 60, [&] { return bridge->isInitialized(); });
    step(QStringLiteral("SimulANT+ devices heard"), waitMinutes * 60, [&] {
        std::set<int> found;
        for (const QString& entry : bridge->foundDevices()) {
            found.insert(entry.split(u' ').first().toInt());
        }
        if (!ctx.announced) {
            ctx.announced = true;
            QStringList types;
            for (int type : needs) {
                types.append(QString::number(type));
            }
            say(QStringLiteral("Waiting for [%1] (only these running in SimulANT+, sending pairing values)")
                    .arg(types.join(QStringLiteral(", "))));
        }
        for (const int power : {11, 17}) {
            if (found.count(power) && !needs.count(power)) {
                throw Abort{QStringLiteral("Also hearing %1: turn that simulator off for this scenario").arg(power)};
            }
        }
        return std::includes(found.begin(), found.end(), needs.begin(), needs.end()) && bridge->scanSettleRemaining() == 0;
    });
    if (trainer) {
        step(QStringLiteral("setup screen auto-selects the trainer and heart rate monitor"), 20, [&] {
            setup->initAntScanConnect();
            return setup->powerSelected() && setup->hrmSelected();
        });
        step(QStringLiteral("Connect Selected Devices (no speed/cadence sensor, so not automatic)"), 5, [&] {
            pressConnect();
            return true;
        });
    }
    step(QStringLiteral("connected"), 30, [&] { return bridge->isReady(); });
    step(QStringLiteral("setup screen ready"), 10, [&] {
        setup->initAntScanConnect();
        QStringList roles;
        for (const ant::Role role : bridge->selectedRoles()) {
            roles.append(ant::roleName(role));
        }
        roles.sort();
        ctx.roles = roles.join(QStringLiteral(", "));
        return true;
    });

    // ---- trainer: a workout ridden on ERG ----
    const auto checkTrainerCommands = [&] {
        const double start = ctx.hudStart;
        struct Command {
            double t;
            QString event;
            double value;
        };
        QList<Command> log;
        QFile file(QStringLiteral("simulant_fe.csv"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                const QStringList parts = in.readLine().split(u',');
                if (parts.size() >= 3 && (parts[1] == u"rx_target" || parts[1] == u"rx_resistance")) {
                    log.append({parts[0].toDouble() - start, parts[1], parts.mid(2).join(u',').toDouble()});
                }
            }
        }
        QList<std::pair<double, double>> targets;
        for (const Command& command : log) {
            if (command.event == u"rx_target") {
                targets.append({command.t, command.value});
            }
        }
        const auto got = [&targets](double watts, double after, double before = 1e9, double tolerance = 0.6) {
            for (const auto& [t, w] : targets) {
                if (after <= t && t <= before && std::abs(w - watts) <= tolerance) {
                    return std::optional<double>(t);
                }
            }
            return std::optional<double>();
        };
        check(got(150, 0, 10).has_value(), QStringLiteral("first block: trainer got 150W"));
        std::set<long long> ramp;
        for (const auto& [t, w] : targets) {
            if (150 < w && w < 250) {
                ramp.insert(static_cast<long long>(py::round(w)));
            }
        }
        check(ramp.size() >= 10,
              QStringLiteral("ramp: trainer followed it up (%1 steps between 150 and 250W)").arg(ramp.size()));
        const double difficulty = ctx.difficulty.value_or(1e9) - start;
        bool afterDifficulty = false;
        for (const auto& [t, w] : targets) {
            afterDifficulty = afterDifficulty || (std::abs(w - std::round(w)) < 0.01 && t > difficulty);
        }
        check(afterDifficulty, QStringLiteral("targets keep coming after the difficulty change"));
        const double pause = ctx.paused.value_or(1e9) - start;
        check(got(0, pause - 1, pause + 4).has_value(), QStringLiteral("pause: trainer target 0W"));
        // intervals at FTP 260: 286W / 130W (under the 300W Power Drive Cutoff)
        check(got(286, 60).has_value() && got(130, 60).has_value(),
              QStringLiteral("intervals: trainer got 286W and 130W (difficulty +2)"));
        std::optional<double> released;
        for (const Command& command : log) {
            if (command.event == u"rx_resistance" && command.value == 0 && !released) {
                released = command.t;
            }
        }
        check(released.has_value(), QStringLiteral("a block over the Power Drive Cutoff (364W > 300W) released the trainer") +
                                        (released ? QStringLiteral(" at %1s").arg(py::round(*released)) : QStringLiteral(" - never")));
        check(!got(364, 0).has_value(), QStringLiteral("the trainer was never asked for the 364W over the cutoff"));
        check(got(208, 100).has_value(), QStringLiteral("last block: trainer got 208W"));
        const double end = ctx.ended - start;
        check(got(0, end - 1, end + 5).has_value(), QStringLiteral("end of the ride: trainer target 0W"));
    };

    if (trainer) {
        step(QStringLiteral("Workout Mode: load the test workout"), 10, [&] {
            const QString path = QDir::current().absoluteFilePath(QStringLiteral("app_test.awo"));
            QFile workout(path);
            workout.open(QIODevice::WriteOnly | QIODevice::Text);
            workout.write(config.value(QStringLiteral("trainerWorkout")).toString().toUtf8());
            workout.close();
            // (as the workout picker's Ride This Workout does)
            setup->pickWorkoutFile(path);
            check(!appState().workoutFilenameShort.isEmpty() && appState().workoutDurationSeconds == 150,
                  QStringLiteral("workout loaded, %1s").arg(appState().workoutDurationSeconds));
            return true;
        });
        step(QStringLiteral("ride starts"), 10, [&] {
            startRide();
            ctx.hudStart = now();
            return true;
        });
        step(QStringLiteral("riding (difficulty +2 at 25s, a pause from 42s to 50s)"), 200, [&] {
            HudScreen* h = hud();
            const double t = h->engine().workoutSecondsPassed();
            if (t >= 25 && !ctx.difficulty) {
                ctx.difficulty = now();
                h->difficultySlider()->setValue(2);
                say(QStringLiteral("difficulty +2 (FTP 260)"));
            }
            if (t >= 42 && !ctx.paused) {
                ctx.paused = now();
                h->pauseButton()->click();
                say(QStringLiteral("paused"));
            }
            if (ctx.paused && !ctx.resumed && now() - *ctx.paused >= 8) {
                ctx.resumed = now();
                h->pauseButton()->click();
                say(QStringLiteral("resumed"));
            }
            if (h->engine().completeShown()) {
                ctx.completed = now();
                return true;
            }
            return false;
        });
        step(QStringLiteral("cool down a few seconds, then END to the synopsis"), 20, [&] {
            if (now() - *ctx.completed < 4) {
                return false;
            }
            ctx.grades = appState().blockGrades;
            endRide(true);
            ctx.ended = now();
            return true;
        });
        step(QStringLiteral("synopsis"), 10, [&] {
            SynopsisScreen* s = synopsis();
            if (!s || !s->isVisible()) {
                return false;
            }
            const std::optional<double> average = grading::workoutAverage(ctx.grades);
            QList<double> pcts;
            for (const auto& [key, grade] : ctx.grades.items()) {
                if (grade.pct) {
                    pcts.append(*grade.pct);
                }
            }
            std::sort(pcts.begin(), pcts.end());
            // every block and interval rep graded; the one over the Power Drive Cutoff (trainer released, riding its
            // free 120W against 364W asked) grades low, the rest were ridden at the target by the trainer
            check(pcts.size() == 8, QStringLiteral("workout graded: %1 of 8 blocks/reps, average %2% (%3)")
                                        .arg(pcts.size())
                                        .arg(average ? rounded(*average) : QStringLiteral("None"), grading::letter(average)));
            const auto whole = [](double pct) { return QString::number(static_cast<long long>(py::round(pct))); };
            QStringList rest;
            bool restAtTarget = true;
            for (int i = 1; i < pcts.size(); ++i) {
                rest.append(whole(pcts[i]));
                restAtTarget = restAtTarget && pcts[i] >= 90;
            }
            check(!pcts.isEmpty() && pcts.first() < 60 && restAtTarget,
                  QStringLiteral("grades: released block %1%, the rest [%2]")
                      .arg(pcts.isEmpty() ? QStringLiteral("-") : whole(pcts.first()), rest.join(QStringLiteral(", "))));
            QStringList best;
            for (const double power : s->bestPowers().mid(0, 3)) {
                best.append(py::str(power));
            }
            check(!s->bestPowers().isEmpty() && !s->dropoutsKeepAverage(),
                  QStringLiteral("synopsis best power [%1]").arg(best.join(QStringLiteral(", "))));
            ctx.synopsisAt = now();
            return true;
        });
        step(QStringLiteral("trainer commands"), 15, [&] {
            // (the stop's 0 W gets a moment to arrive)
            if (now() - ctx.synopsisAt < 4) {
                return false;
            }
            checkTrainerCommands();
            return true;
        });
    }

    // ---- sensors: Musical Cadence Mode, an FTP test, Free Mode ----
    if (!trainer) {
        step(QStringLiteral("Musical Cadence Mode: songs analyzed"), 120, [&] {
            MusicSelection selection;
            QStringList paths;
            for (const QJsonValue& song : config.value(QStringLiteral("songs")).toArray()) {
                const QJsonArray pair = song.toArray();
                selection.songs.append({pair[0].toString(), pair[1].toString()});
                paths.append(pair[0].toString());
            }
            selection.analyses = music::analyzeSongs(paths);
            int bad = 0;
            for (const music::Analysis& analysis : selection.analyses) {
                bad += analysis.error ? 1 : 0;
            }
            check(bad == 0 && selection.analyses.size() == 2,
                  QStringLiteral("songs analyzed (%1, unreadable %2)").arg(selection.analyses.size()).arg(bad));
            setup->setMusicSelection(selection);
            setup->modeButton(QStringLiteral("cadence"))->click();
            const AppState& state = appState();
            std::set<int> cadences;
            for (const WorkoutEntry& entry : state.workoutList) {
                if (entry.type == WorkoutEntry::Type::Block && entry.block.type == BlockType::Steady) {
                    cadences.insert(entry.block.power[0]);
                }
            }
            QStringList cadenceText;
            for (int cadence : cadences) {
                cadenceText.append(QString::number(cadence));
            }
            check(state.cadenceMode && !state.workoutList.isEmpty() && state.musicTracks.size() == 2,
                  QStringLiteral("music workout built: %1s, cadences [%2]")
                      .arg(state.workoutDurationSeconds)
                      .arg(cadenceText.join(QStringLiteral(", "))));
            return true;
        });
        step(QStringLiteral("music ride starts"), 10, [&] {
            startRide();
            return true;
        });
        step(QStringLiteral("riding to the music"), 120, [&] {
            HudScreen* h = hud();
            if (!ctx.musicAt && h->musicPlayer() && h->engine().workoutSecondsPassed() > 5) {
                ctx.musicAt = {now(), h->engine().workoutSecondsPassed()};
            }
            if (window->windowTitle().contains(QStringLiteral("App Test - "))) {
                ctx.titleSeen = window->windowTitle().trimmed();
            }
            return h->engine().completeShown();
        });
        step(QStringLiteral("music ride ends"), 20, [&] {
            HudScreen* h = hud();
            if (!ctx.musicAt) {
                throw Abort{QStringLiteral("the music never started")};
            }
            const double elapsed = now() - ctx.musicAt->first;
            const double moved = appState().workoutDurationSeconds - ctx.musicAt->second;
            check(std::abs(elapsed - moved) < 2.5, QStringLiteral("the workout clock followed the music (%1s of ride in %2s)")
                                                       .arg(rounded(moved), rounded(elapsed)));
            check(!h->musicPlayer() || h->musicPlayer()->skipped().isEmpty(), QStringLiteral("no song skipped"));
            check(!ctx.titleSeen.isEmpty(), QStringLiteral("title bar showed the song playing (%1)").arg(ctx.titleSeen));
            ctx.cadenceGrades = appState().blockGrades;
            endRide(true);
            return true;
        });
        step(QStringLiteral("music synopsis"), 10, [&] {
            SynopsisScreen* s = synopsis();
            if (!s || !s->isVisible()) {
                return false;
            }
            const bool cadence = s->rideCount() > 0 && s->rideOf(static_cast<int>(s->rideCount() - 1)).kind == RideKind::Cadence;
            check(cadence && !ctx.cadenceGrades.isEmpty(),
                  QStringLiteral("cadence ride synopsis, %1 blocks graded").arg(ctx.cadenceGrades.items().size()));
            backToSetup();
            return true;
        });
        step(QStringLiteral("back on the setup screen: connected again"), 30, reconnected);
        step(QStringLiteral("20m FTP test (shortened: 20s warmup, 40s effort)"), 15, [&] {
            check(setup->blinkTimer()->isActive(), QStringLiteral("setup screen blinking again after the ride"));
            ftptest::shortenForTests(QStringLiteral("20m"), {QStringLiteral("1:20,0.5")}, 40);
            setup->modeButton(QStringLiteral("20m"))->click();
            check(appState().ftpTestMode == u"20m" && appState().workoutDurationSeconds == 60,
                  QStringLiteral("FTP test built, %1s").arg(appState().workoutDurationSeconds));
            startRide();
            return true;
        });
        step(QStringLiteral("FTP test effort"), 90, [&] { return hud()->engine().completeShown(); });
        step(QStringLiteral("FTP test result"), 15, [&] {
            const std::optional<ftptest::Result> result = appState().ftpTestResult;
            const std::optional<double> average = result ? result->average : std::nullopt;
            check(result && result->completed && average && std::abs(*average - pairingPower) <= 5,
                  QStringLiteral("FTP test average %1W (the power meter sends %2W)")
                      .arg(average ? rounded(*average) : QStringLiteral("None"))
                      .arg(pairingPower));
            endRide(false);
            return true;
        });
        step(QStringLiteral("FTP test synopsis and Apply FTP"), 10, [&] {
            SynopsisScreen* s = synopsis();
            if (!s || !s->isVisible()) {
                return false;
            }
            const int expected = ftptest::ftpFromAverage(QStringLiteral("20m"), *appState().ftpTestResult->average);
            check(s->dropoutsKeepAverage(), QStringLiteral("20m test: best power keeps its average through dropouts"));
            check(s->newFtp() == expected && s->applyFtpButton()->isEnabled(),
                  QStringLiteral("new FTP %1 offered").arg(s->newFtp() ? QString::number(*s->newFtp()) : QStringLiteral("None")));
            s->applyFtpButton()->click();
            check(appState().ftp == expected && settings::load().value(QStringLiteral("ftp")).toInt() == expected,
                  QStringLiteral("Apply FTP: FTP is %1 and saved").arg(appState().ftp));
            backToSetup();
            return true;
        });
        step(QStringLiteral("back on the setup screen: connected again "), 30, reconnected);
        step(QStringLiteral("Free Mode ride (15s)"), 40, [&] {
            if (!ctx.freeStart) {
                setup->modeButton(QStringLiteral("free"))->click();
                startRide();
                ctx.freeStart = now();
                return false;
            }
            if (now() - *ctx.freeStart < 15) {
                return false;
            }
            HudScreen* h = hud();
            const double power = h->powerGauge()->gauge()->reading();
            check(power == pairingPower, QStringLiteral("free ride power %1W").arg(power));
            check(std::abs(h->speedMph() - pairingSpeedKmh / 1.609344) < 0.6,
                  QStringLiteral("free ride speed %1 mph").arg(h->speedMph()));
            endRide(false);
            return true;
        });
        step(QStringLiteral("free ride synopsis"), 10, [&] { return synopsis() && synopsis()->isVisible(); });
    }

    // ---- back to setup, and out ----
    step(QStringLiteral("back to setup"), 15, [&] {
        if (!ctx.back) {
            ctx.back = true;
            backToSetup();
            return false;
        }
        if (!setup->isVisible() || now() - ctx.backAt < 1.5) {
            return false;
        }
        onlySetupShowing();
        check(setup->blinkTimer()->isActive() && setup->checkTimer()->isActive(),
              QStringLiteral("setup screen timers running again"));
        return true;
    });

    int index = 0;
    std::optional<double> deadline;
    bool finished = false;
    QTimer timer;
    const auto finish = [&](int code) {
        if (finished) {
            return;
        }
        finished = true;
        timer.stop();
        const QString log = log::read(5000);
        const qsizetype unhandled = log.count(QStringLiteral("Unhandled exception"));
        check(unhandled == 0, QStringLiteral("no crashes or uncaught exceptions (%1 in the app's log)").arg(unhandled));
        int passed = 0;
        QStringList lines{QStringLiteral("AntWorkoutQt full app test: %1   %2")
                              .arg(scenario, QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                          QStringLiteral("Devices: [%1]").arg(ctx.roles), QString()};
        for (const auto& [ok, text] : results) {
            passed += ok ? 1 : 0;
            lines.append((ok ? QStringLiteral("PASS  ") : QStringLiteral("FAIL  ")) + text);
        }
        lines << QString() << QStringLiteral("%1/%2 checks passed").arg(passed).arg(results.size());
        const QString report = lines.join(u'\n');
        QFile file(QStringLiteral("report.txt"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write((report + u'\n').toUtf8());
        }
        std::printf("\n%s\n", qPrintable(report));
        std::fflush(stdout);
        app.exit(code ? code : (passed == results.size() ? 0 : 1));
    };

    step(QStringLiteral("exit (the app's own: ANT stick closed, then out of the event loop)"), 15, [&] {
        // (Esc on the setup screen is the X: the exit prompt)
        if (trainer) {
            window->escapePressed();
        } else {
            window->close();
        }
        ConfirmExitWindow* prompt = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* exit = qobject_cast<ConfirmExitWindow*>(widget); exit && exit->isVisible()) {
                prompt = exit;
            }
        }
        check(prompt != nullptr, QStringLiteral("exit prompt shown"));
        if (!prompt) {
            throw Abort{QStringLiteral("no exit prompt")};
        }
        // Yes, Exit: the ANT stick closed, every window hidden, out of the event loop
        prompt->yesButton()->click();
        check(!bridge->running(), QStringLiteral("ANT bridge stopped"));
        bool anyShowing = false;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            anyShowing = anyShowing || widget->isVisible();
        }
        check(!anyShowing, QStringLiteral("every window hidden"));
        // (the report now: the event loop ends before the next step would run)
        index = static_cast<int>(steps.size());
        finish(0);
        return true;
    });

    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (index >= steps.size()) {
            finish(0);
            return;
        }
        const Step& current = steps[index];
        if (!deadline) {
            deadline = now() + current.seconds;
            say(QStringLiteral("- ") + current.name);
        }
        bool done = false;
        try {
            done = current.run();
        } catch (const Abort& abort) {
            check(false, QStringLiteral("%1: %2").arg(current.name, abort.why));
            finish(3);
            return;
        } catch (const std::exception& e) {
            check(false, QStringLiteral("Test error in '%1': %2").arg(current.name, QString::fromLocal8Bit(e.what())));
            finish(3);
            return;
        }
        if (finished) {
            return;
        }
        if (done) {
            ++index;
            deadline.reset();
        } else if (now() > *deadline) {
            check(false, current.name + QStringLiteral(": timed out"));
            finish(2);
        }
    });
    timer.start(100);
    int code = app.exec();
    if (!finished) {
        // (the event loop ended before the test did)
        check(false, QStringLiteral("app quit early, in step '%1'").arg(index < steps.size() ? steps[index].name : QString()));
        finish(1);
        code = 1;
    }
    if (bridge->running()) {
        bridge->stop();
    }
    sound::shutdown();
    return code;
}
