// simulant_ride: the C++ side of the SimulANT+ hardware test (AntWorkout\SimulantTests\run_simulant_test.py's ride,
// PLAN.md Phase 6 and 10). The app's real ANT bridge, readings pipeline and workout screen (Free Mode) on one ANT
// stick while SimulANT+ simulates the sensors on the other, following the timeline in the SimulANT+ scripts'
// awo_timeline.py. Every 100 ms it records what the workout screen shows, as the Python harness does.
//
//   simulant_ride RUN_FOLDER [--offscreen] [--wait MINUTES]
//
// simulant_test.py (in this folder) starts it: it writes RUN_FOLDER\ride_setup.json (the scenario, the device
// types it needs, the timeline's phases and the trainer's action in each, the control file the scripts follow) and
// afterwards checks RUN_FOLDER\app_samples.csv against what the scripts logged, with the Python harness's own checks.
// This writes the control file's epoch once the devices are connected (the scripts' timeline starts there), then
// app_samples.csv and ride.json (the stick, the devices, the CPU used) at the end. The app's log goes in RUN_FOLDER.

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QSlider>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

#include <windows.h>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppState.hpp"
#include "AppStyle.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "ReadingPipeline.hpp"
#include "SoundEffects.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

// --offscreen: the workout screen isn't put on the screen
bool hidden = false;

void say(const QString& text) {
    std::printf("%s %s\n", qPrintable(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))), qPrintable(text));
    std::fflush(stdout);
}

double wallSeconds() {
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

double cpuSeconds() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto seconds = [](const FILETIME& time) {
        return (static_cast<quint64>(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 1e7;
    };
    return seconds(kernel) + seconds(user);
}

struct Phase {
    QString name;
    double start = 0, end = 0;
    QJsonValue action;  // the trainer's: watts, "release" or "stop"; none for the sensors scenario
};

std::optional<ant::Role> roleNamed(const QString& name) {
    if (name == u"power") {
        return ant::Role::Power;
    }
    if (name == u"hrm") {
        return ant::Role::Hrm;
    }
    if (name == u"speed_cadence") {
        return ant::Role::SpeedCadence;
    }
    return std::nullopt;
}

// a value for the CSV: empty for none
QString cell(const std::optional<double>& value) {
    return value ? QString::number(*value, 'g', 12) : QString();
}

QString csvText(QString text) {
    if (text.contains(u',') || text.contains(u'"')) {
        text = u'"' + text.replace(u'"', QStringLiteral("\"\"")) + u'"';
    }
    return text;
}

class SimulantRide : public QObject {
public:
    SimulantRide(const QString& runDir, double waitMinutes) : runDir_(runDir) {
        QFile file(QDir(runDir).filePath(QStringLiteral("ride_setup.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("no ride_setup.json in the run folder");
        }
        const QJsonObject setup = QJsonDocument::fromJson(file.readAll()).object();
        scenario_ = setup[QStringLiteral("scenario")].toString();
        controlFile_ = setup[QStringLiteral("controlFile")].toString();
        wheelMeters_ = setup[QStringLiteral("wheelMm")].toDouble() / 1000;
        const QJsonObject needs = setup[QStringLiteral("needs")].toObject();
        for (auto it = needs.begin(); it != needs.end(); ++it) {
            needNames_.append(it.key());
            needs_[it.key()] = it.value().toInt();
        }
        for (const QJsonValue& value : setup[QStringLiteral("phases")].toArray()) {
            const QJsonArray phase = value.toArray();
            phases_.append({phase[0].toString(), phase[1].toDouble(), phase[2].toDouble(), phase[3]});
        }
        total_ = phases_.isEmpty() ? 0 : phases_.back().end;
        waitUntil_ = wallSeconds() + waitMinutes * 60;

        connect(&bridge_, &ant::AntBridge::errorOccurred, this, [](const QString& error) {
            say(QStringLiteral("ANT error: ") + error);
            QCoreApplication::exit(2);
        });
        bridge_.start();
        connect(&timer_, &QTimer::timeout, this, &SimulantRide::step);
        timer_.start(100);
    }

private:
    void writeControl(std::optional<double> epoch) {
        const QString temp = controlFile_ + QStringLiteral(".tmp");
        QFile file(temp);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(QStringLiteral("run=%1\nscenario=%2\nepoch=%3\n")
                       .arg(QDir::toNativeSeparators(runDir_), scenario_,
                            epoch ? QString::number(*epoch, 'f', 3) : QStringLiteral("none"))
                       .toUtf8());
        file.close();
        MoveFileExW(reinterpret_cast<LPCWSTR>(temp.utf16()), reinterpret_cast<LPCWSTR>(controlFile_.utf16()),
                    MOVEFILE_REPLACE_EXISTING);
    }

    void step() {
        const double now = wallSeconds();
        if (stage_ == Stage::Init) {
            if (bridge_.isInitialized()) {
                const std::optional<int> stick = bridge_.manager().device_number();
                stick_ = stick ? QString::number(*stick) : QStringLiteral("?");
                say(QStringLiteral("AntWorkout is scanning on ANT stick %1; waiting for SimulANT+'s devices").arg(stick_));
                stage_ = Stage::Scan;
            } else if (now > waitUntil_) {
                fail(QStringLiteral("ANT stick never initialized"));
            }
            return;
        }
        if (stage_ == Stage::Scan) {
            QMap<int, QList<int>> found;
            for (const QString& entry : bridge_.foundDevices()) {
                const QStringList parts = entry.split(u' ');
                found[parts[0].toInt()].append(parts[1].toInt());
            }
            QStringList missing;
            for (const QString& role : needNames_) {
                if (!found.contains(needs_[role])) {
                    missing.append(role);
                }
            }
            if (!missing.isEmpty() || bridge_.scanSettleRemaining() > 0) {
                if (now > waitUntil_) {
                    fail(QStringLiteral("Timed out waiting for SimulANT+ devices: ") + missing.join(QStringLiteral(", ")));
                }
                return;
            }
            ant::Selection selection;
            QStringList picked;
            for (const QString& role : needNames_) {
                const QList<int>& numbers = found[needs_[role]];
                if (numbers.size() > 1) {
                    say(QStringLiteral("WARNING: several type %1 devices heard, using %2").arg(needs_[role]).arg(numbers.back()));
                }
                selection[*roleNamed(role)] = {needs_[role], numbers.back()};
                identities_[role] = {needs_[role], numbers.back()};
                picked.append(QStringLiteral("%1=(%2, %3)").arg(role).arg(needs_[role]).arg(numbers.back()));
            }
            selection_ = picked.join(QStringLiteral(", "));
            say(QStringLiteral("Found everything, connecting: ") + selection_);
            bridge_.requestConnect(selection);
            stage_ = Stage::Connect;
            return;
        }
        if (stage_ == Stage::Connect) {
            if (!bridge_.isReady()) {
                return;
            }
            const QSet<ant::Role> roles = bridge_.selectedRoles();
            AppState& state = appState();
            state.antHRM = roles.contains(ant::Role::Hrm);
            state.antSpeedCadence = roles.contains(ant::Role::SpeedCadence);
            state.antCadence = state.antSpeed = false;
            state.workoutMode = false;
            state.cadenceMode = false;
            state.ftpTestMode.clear();
            state.ftp = 250;
            state.maxHr = 185;
            state.wheelCircumference = wheelMeters_;
            state.maxWatts = 1200;
            state.ftpRanges = zones::powerRanges();
            state.startTimestamp = QDateTime::currentDateTime();
            window_ = std::make_unique<QMainWindow>();
            hud_ = new HudScreen(&bridge_);
            window_->setCentralWidget(hud_);
            window_->resize(1400, 900);
            window_->setAttribute(Qt::WA_DontShowOnScreen, hidden);
            window_->show();
            hud_->startReadings();
            epoch_ = now + 2.0;
            writeControl(epoch_);
            cpu0_ = {cpuSeconds(), now};
            say(QStringLiteral("Connected - timeline starts now (%1s)").arg(total_, 0, 'f', 0));
            stage_ = Stage::Run;
            return;
        }
        if (stage_ == Stage::Run) {
            const double t = now - epoch_;
            QString name = t >= total_ ? QStringLiteral("done") : QStringLiteral("pre");
            const Phase* phase = nullptr;
            for (const Phase& p : phases_) {
                if (p.start <= t && t < p.end) {
                    phase = &p;
                    name = p.name;
                }
            }
            if (name != phaseName_) {
                phaseName_ = name;
                say(QStringLiteral("t=%1s  phase: %2").arg(t, 6, 'f', 1).arg(name));
                if (scenario_ == u"trainer" && phase && !phase->action.isNull()) {
                    if (phase->action.toString() == u"release") {
                        bridge_.releaseTrainerPower();
                    } else if (phase->action.toString() == u"stop") {
                        hud_->stopReadings();
                    } else {
                        const int watts = phase->action.toInt();
                        hud_->difficultySlider()->setValue(watts);
                        // (the same value again doesn't move the slider, so command it directly too)
                        bridge_.setTrainerTargetPower(watts);
                    }
                }
            }
            record(t, name);
            if (t >= total_ + 3) {
                finish();
            }
        }
    }

    QString device(ant::Role role) const {
        const std::optional<ant::DeviceReading> reading = bridge_.selectedDevice(role);
        return reading ? reading->state + (reading->dataStalled ? QStringLiteral("/stalled") : QString())
                       : QStringLiteral("-");
    }

    // messages heard from it so far (a stick reset can make a new sensor: summed over every match)
    double packets(const QString& role) {
        const ant::DeviceId identity = identities_[role];
        double count = 0;
        for (const auto& sensor : bridge_.manager().get_devices()) {
            if (sensor->device_type() == identity.first && sensor->device_number() == identity.second) {
                count += static_cast<double>(sensor->get(&antnative::Sensor::packet_count));
            }
        }
        return count;
    }

    void record(double t, const QString& phaseName) {
        constexpr double kKphPerMph = 1.609344;
        const Gauge* power = hud_->powerGauge();
        const Gauge* cadence = hud_->cadenceGauge();
        const Gauge* hr = hud_->heartRateGauge();
        const ant::ReadingPipeline* readings = hud_->readings();
        const std::optional<ant::AntBridge::TrainerTarget> target = bridge_.trainerTarget();
        QString mode = QStringLiteral("none");
        switch (bridge_.manager().mode()) {
        case antnative::ChannelMode::Scan:
            mode = QStringLiteral("scan");
            break;
        case antnative::ChannelMode::Channels:
            mode = QStringLiteral("channels");
            break;
        default:
            break;
        }
        QStringList row = {
            QString::number(std::round(t * 100) / 100, 'f', 2),
            csvText(phaseName),
            cell(power->gauge()->value()),
            cell(power->gauge()->reading()),
            cadence ? cell(cadence->gauge()->value()) : QString(),
            hr ? cell(hr->gauge()->value()) : QString(),
            hr ? cell(hr->gauge()->minValue()) : QString(),
            hr ? cell(hr->gauge()->reading()) : QString(),
            QString::number(std::round(hud_->speedMph() * kKphPerMph * 1000) / 1000, 'f', 3),
            QString::number(std::round(hud_->distanceMiles() * kKphPerMph * 10000) / 10000, 'f', 4),
            readings ? readings->cadenceSource() : QString(),
            readings ? readings->speedSource() : QString(),
            device(ant::Role::Power),
            device(ant::Role::SpeedCadence),
            device(ant::Role::Hrm),
            target && target->watts ? QString::number(*target->watts) : QString(),
            mode,
            QString::number(bridge_.manager().channel_devices().size()),
        };
        for (const QString& role : needNames_) {
            row.append(cell(packets(role)));
        }
        rows_.append(row.join(u','));
    }

    void finish() {
        stage_ = Stage::Done;
        const double cpu = (cpuSeconds() - cpu0_.first) / (wallSeconds() - cpu0_.second) * 100;
        hud_->stopTimers();
        hud_->stopReadings();
        say(QStringLiteral("Timeline finished"));
        QFile samples(QDir(runDir_).filePath(QStringLiteral("app_samples.csv")));
        samples.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QStringList header = {QStringLiteral("t"),        QStringLiteral("phase"),         QStringLiteral("power"),
                              QStringLiteral("powerReading"), QStringLiteral("cadence"),   QStringLiteral("hr"),
                              QStringLiteral("hrMin"),    QStringLiteral("hrReading"),     QStringLiteral("speedKph"),
                              QStringLiteral("distKm"),   QStringLiteral("cadenceSource"), QStringLiteral("speedSource"),
                              QStringLiteral("power_dev"), QStringLiteral("sc_dev"),       QStringLiteral("hr_dev"),
                              QStringLiteral("trainerTarget"), QStringLiteral("antMode"),  QStringLiteral("antChannels")};
        for (const QString& role : needNames_) {
            header.append(QStringLiteral("pkts_") + role);
        }
        samples.write((header.join(u',') + QStringLiteral("\r\n") + rows_.join(QStringLiteral("\r\n")) +
                       QStringLiteral("\r\n")).toUtf8());
        samples.close();
        QFile ride(QDir(runDir_).filePath(QStringLiteral("ride.json")));
        ride.open(QIODevice::WriteOnly | QIODevice::Truncate);
        ride.write(QJsonDocument(QJsonObject{{"stick", stick_}, {"selection", selection_}, {"cpu", cpu}}).toJson());
        ride.close();
        bridge_.stop();
        bridge_.wait(5000);
        QCoreApplication::exit(0);
    }

    void fail(const QString& text) {
        say(QStringLiteral("Harness error: ") + text);
        stage_ = Stage::Done;
        bridge_.stop();
        bridge_.wait(5000);
        QCoreApplication::exit(3);
    }

    enum class Stage { Init, Scan, Connect, Run, Done };

    QString runDir_;
    QString scenario_;
    QString controlFile_;
    double wheelMeters_ = 2.1;
    QStringList needNames_;
    QMap<QString, int> needs_;
    QMap<QString, ant::DeviceId> identities_;
    QList<Phase> phases_;
    double total_ = 0;
    double waitUntil_ = 0;
    ant::AntBridge bridge_;
    QTimer timer_;
    Stage stage_ = Stage::Init;
    QString stick_, selection_, phaseName_;
    std::unique_ptr<QMainWindow> window_;
    HudScreen* hud_ = nullptr;
    double epoch_ = 0;
    std::pair<double, double> cpu0_;
    QStringList rows_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QString runDir;
    double waitMinutes = 10;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == u"--offscreen") {
            // (the window built but never shown: dist has only Qt's windows platform plugin, not the offscreen one)
            hidden = true;
        } else if (arg == u"--wait" && i + 1 < argc) {
            waitMinutes = QString::fromLocal8Bit(argv[++i]).toDouble();
        } else {
            runDir = QDir(arg).absolutePath();
        }
    }
    if (runDir.isEmpty()) {
        std::fprintf(stderr, "usage: simulant_ride RUN_FOLDER [--offscreen] [--wait MINUTES]\n");
        return 2;
    }
    QApplication app(argc, argv);
    // the app's own files (its log, settings) in the run folder
    QDir::setCurrent(runDir);
    log::installErrorLogging();
    keys::install(&app);
    sound::init(&app);
    theme::setDefaultFont(QApplication::font());
    theme::setAppStyleSheet(style::kAppStyleSheet);
    theme::clear();
    try {
        SimulantRide ride(runDir, waitMinutes);
        const int code = app.exec();
        sound::shutdown();
        return code;
    } catch (const std::exception& error) {
        say(QStringLiteral("Harness error: ") + QString::fromLocal8Bit(error.what()));
        return 3;
    }
}
