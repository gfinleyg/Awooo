// ant_console: the app's ANT layer (AntBridge + ReadingPipeline) against real sensors, from a console, before any
// screen exists (PLAN.md Phase 2). It scans, waits for the device types asked for, connects one device of each kind,
// prints what the workout screen would show every second, and checks it against what's expected.
//
//   ant_console --wait-for 11,121,120 --seconds 60 --expect power=180,cadence=80,speed=17.4,hr=110
//   ant_console --wait-for 17,120 --target 150 --expect power=150,hr=110
//
//   --wait-for TYPES   device types to wait for before connecting (up to --wait seconds, default 300)
//   --seconds N        how long to read once connected (default 60)
//   --target W         an FE-C trainer's target power, sent once connected
//   --expect K=V,...   the median of what's shown (power W, hr BPM, cadence RPM, speed MPH) must be within 3% of V
//   --wheel M          wheel circumference in meters (default 2.1, the SimulANT+ scripts')
// Exits with 0 when every expectation is met, 1 otherwise. The log goes to antWorkoutLog.txt in the current folder.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMap>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>

#include <windows.h>

#include "AntBridge.hpp"
#include "AppState.hpp"
#include "Logger.hpp"
#include "ReadingPipeline.hpp"

using namespace awo;
using namespace awo::ant;

namespace {

QTextStream& out() {
    static QTextStream stream(stdout);
    return stream;
}

double median(QList<double> values) {
    if (values.isEmpty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// Runs the event loop until done() or seconds pass; false on a timeout
bool waitFor(const std::function<bool()>& done, double seconds) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > seconds * 1000) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    SetConsoleOutputCP(CP_UTF8);
    log::installErrorLogging();

    QList<int> waitTypes;
    double waitSeconds = 300;
    double seconds = 60;
    std::optional<double> target;
    QMap<QString, double> expected;
    double wheel = 2.1;
    const QStringList args = app.arguments();
    for (qsizetype i = 1; i + 1 < args.size(); i += 2) {
        const QString name = args[i];
        const QString value = args[i + 1];
        if (name == u"--wait-for") {
            for (const QString& type : value.split(u',')) {
                waitTypes << type.toInt();
            }
        } else if (name == u"--wait") {
            waitSeconds = value.toDouble();
        } else if (name == u"--seconds") {
            seconds = value.toDouble();
        } else if (name == u"--target") {
            target = value.toDouble();
        } else if (name == u"--expect") {
            for (const QString& pair : value.split(u',')) {
                expected.insert(pair.section(u'=', 0, 0), pair.section(u'=', 1).toDouble());
            }
        } else if (name == u"--wheel") {
            wheel = value.toDouble();
        }
    }

    AntBridge bridge;
    QObject::connect(&bridge, &AntBridge::statusMessage, [](const QString& text) { out() << "status: " << text << Qt::endl; });
    QObject::connect(&bridge, &AntBridge::errorOccurred, [](const QString& text) { out() << "error: " << text << Qt::endl; });
    QObject::connect(&bridge, &AntBridge::deviceDiscovered,
                     [](int type, int number) { out() << "discovered " << type << ' ' << number << Qt::endl; });
    bridge.start();

    if (!waitFor([&] { return bridge.isInitialized(); }, 60)) {
        out() << "FAIL: the ANT stick didn't initialize" << Qt::endl;
        return 1;
    }
    // A stick can replay what it buffered from its last session as it opens (devices that aren't there any more):
    // a rescan a few seconds in forgets them, as the setup screen's Disconnect and Rescan does
    waitFor([] { return false; }, 3);
    bridge.requestDisconnect();
    waitFor([&] { return bridge.scanSettleRemaining() < AntBridge::kScanSettleSeconds; }, 10);
    out() << "is scanning on ANT stick" << Qt::endl;
    const auto foundTypes = [&] {
        QSet<int> types;
        for (const QString& device : bridge.foundDevices()) {
            types.insert(device.section(u' ', 0, 0).toInt());
        }
        return types;
    };
    if (!waitFor([&] {
            const QSet<int> types = foundTypes();
            return std::all_of(waitTypes.begin(), waitTypes.end(), [&](int type) { return types.contains(type); });
        }, waitSeconds)) {
        out() << "FAIL: not every device type was heard" << Qt::endl;
        return 1;
    }
    waitFor([&] { return bridge.scanSettleRemaining() <= 0; }, AntBridge::kScanSettleSeconds + 1);

    // one device of each kind (a trainer over a power meter when there's a target to send it)
    Selection selection;
    for (const QString& device : bridge.foundDevices()) {
        const int type = device.section(u' ', 0, 0).toInt();
        const int number = device.section(u' ', 1).toInt();
        const std::optional<Role> role = roleForDeviceType(type);
        if (!role || (!waitTypes.isEmpty() && !waitTypes.contains(type))) {
            continue;
        }
        const auto existing = selection.find(*role);
        if (existing == selection.end() || (target && type == kDeviceTypeTrainer)) {
            selection[*role] = {type, number};
        }
    }
    AppState& state = appState();
    state.workoutMode = false;
    state.wheelCircumference = wheel;
    for (const auto& [role, identity] : selection) {
        out() << "connecting " << roleName(role) << " = " << identity.first << ' ' << identity.second << Qt::endl;
        state.antHRM = state.antHRM || role == Role::Hrm;
        state.antSpeedCadence = state.antSpeedCadence || role == Role::SpeedCadence;
        state.antCadence = state.antCadence || role == Role::Cadence;
        state.antSpeed = state.antSpeed || role == Role::Speed;
    }
    bridge.requestConnect(selection);
    if (!waitFor([&] { return bridge.isReady(); }, 30)) {
        out() << "FAIL: didn't connect" << Qt::endl;
        return 1;
    }

    ReadingPipeline pipeline(&bridge);
    QMap<QString, QList<double>> shown;
    QMap<QString, double> latest;
    const auto record = [&](const QString& name, double value) {
        shown[name] << value;
        latest[name] = value;
    };
    QObject::connect(&pipeline, &ReadingPipeline::powerShown, [&](int watts) { record(QStringLiteral("power"), watts); });
    QObject::connect(&pipeline, &ReadingPipeline::heartRateShown, [&](int bpm) { record(QStringLiteral("hr"), bpm); });
    QObject::connect(&pipeline, &ReadingPipeline::cadenceShown, [&](double rpm, int) { record(QStringLiteral("cadence"), rpm); });
    QObject::connect(&pipeline, &ReadingPipeline::speedShown, [&](double mph) { record(QStringLiteral("speed"), mph); });
    QObject::connect(&pipeline, &ReadingPipeline::distanceShown, [&](double miles) { latest[QStringLiteral("distance")] = miles; });
    pipeline.start();
    if (target) {
        if (bridge.trainerSelected()) {
            bridge.setTrainerTargetPower(*target);
            out() << "trainer target " << *target << " W" << Qt::endl;
        } else {
            out() << "no trainer selected for the target" << Qt::endl;
        }
    }

    QElapsedTimer elapsed;
    elapsed.start();
    int second = 0;
    while (elapsed.elapsed() < seconds * 1000) {
        waitFor([&] { return elapsed.elapsed() >= (second + 1) * 1000; }, 2);
        ++second;
        QString line = QStringLiteral("t=%1s").arg(second, 3);
        for (const QString& name : {QStringLiteral("power"), QStringLiteral("hr"), QStringLiteral("cadence"),
                                    QStringLiteral("speed"), QStringLiteral("distance")}) {
            if (latest.contains(name)) {
                line += QStringLiteral("  %1=%2").arg(name).arg(latest[name], 0, 'f', name == u"distance" ? 3 : 1);
            }
        }
        for (const auto& [role, identity] : selection) {
            if (const std::optional<DeviceReading> device = bridge.selectedDevice(role)) {
                if (device->state != u"ACTIVE" || device->dataStalled) {
                    line += QStringLiteral("  [%1 %2%3]").arg(roleName(role), device->state,
                                                              device->dataStalled ? QStringLiteral(" stalled") : QString());
                }
            }
        }
        out() << line << Qt::endl;
    }
    pipeline.stop();

    bool ok = true;
    out() << "--- summary" << Qt::endl;
    for (auto it = shown.cbegin(); it != shown.cend(); ++it) {
        out() << it.key() << ": " << it.value().size() << " readings, median " << median(it.value()) << Qt::endl;
    }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        // (the second half of the run: a trainer takes a moment to reach its target)
        const QList<double> values = shown.value(it.key()).mid(shown.value(it.key()).size() / 2);
        const double value = median(values);
        const bool met = !values.isEmpty() && std::abs(value - it.value()) <= 0.03 * it.value();
        ok = ok && met;
        out() << (met ? "PASS " : "FAIL ") << it.key() << ": median " << value << ", expected " << it.value() << Qt::endl;
    }
    bridge.stop();
    bridge.wait(10000);
    return ok ? 0 : 1;
}
