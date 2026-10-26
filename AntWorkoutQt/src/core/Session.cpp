#include "Session.hpp"

#include <algorithm>
#include <map>

#include "AppState.hpp"
#include "PyCompat.hpp"
#include "WorkoutHelpers.hpp"

namespace awo {

AppState& appState() {
    static AppState state;
    return state;
}

namespace session {

QList<Ride>& rides() {
    return appState().sessionRides;
}

double seconds() {
    double total = 0;
    for (const Ride& ride : rides()) {
        total += ride.seconds;
    }
    return total;
}

void clear() {
    rides().clear();
}

RideStart startValues() {
    const AppState& state = appState();
    RideStart start;
    start.ftp = state.ftp;
    start.powerRanges = ftpcolors::powerRanges(ftpcolors::zoneFtp(state.ftp, state.ftpTestMode));
    start.targetRanges = state.ftpRanges;
    start.targetRanges.cadence = state.workoutMode && state.cadenceMode;
    return start;
}

void beginRide() {
    appState().rideStart = startValues();
    appState().rideRecorded = false;
}

RideKind rideKind() {
    const AppState& state = appState();
    if (!state.workoutMode) {
        return RideKind::Free;
    }
    if (!state.ftpTestMode.isEmpty()) {
        return RideKind::FtpTest;
    }
    return state.cadenceMode ? RideKind::Cadence : RideKind::Workout;
}

void addCurrentRide(std::optional<Ring> ring) {
    AppState& state = appState();
    if (state.rideRecorded) {
        return;
    }
    state.rideRecorded = true;
    const RideStart begun = state.rideStart ? *state.rideStart : startValues();
    const RideKind kind = rideKind();

    Ride ride;
    ride.kind = kind;
    ride.data = workoutData();
    const double startSeconds = state.startTimestamp.toMSecsSinceEpoch() / 1000.0;
    double lastReading = 0.0;
    bool anyReading = false;
    for (const SensorHistory* history : {&ride.data.power, &ride.data.heartRate, &ride.data.speedCadence,
                                         &ride.data.cadence, &ride.data.speed, &ride.data.fallbackCadence}) {
        if (!history->isEmpty()) {
            const double last = history->times().last() - startSeconds;
            lastReading = anyReading ? std::max(lastReading, last) : last;
            anyReading = true;
        }
    }
    ride.powerRequestSeconds = state.workoutPowerRequestsSeconds;
    ride.powerRequestWatts = state.workoutPowerRequestsWatts;
    ride.powerRequestTimes = state.workoutPowerRequestsSysTime;
    if (!ride.powerRequestSeconds.isEmpty() && !ride.powerRequestTimes.isEmpty()) {
        ride.rideOffset = state.startTimestamp.msecsTo(ride.powerRequestTimes.first()) / 1000.0 -
                          ride.powerRequestSeconds.first();
    }
    if (kind == RideKind::Cadence) {
        for (const SongHeard& heard : state.musicPlayed) {
            if (heard.track < state.musicTracks.size()) {
                ride.songs.append({state.musicTracks[heard.track].title, heard.start, heard.end});
            }
        }
        // (Musical Cadence Mode's song headers are "<length> : <Artist> - <Title>")
        for (auto it = state.musicSongStarts.cbegin(); it != state.musicSongStarts.cend(); ++it) {
            if (isSongHeader(it.value(), state.cadenceMode)) {
                ride.songHeaders.insert(it.key(), it.value().section(QStringLiteral(" : "), 1));
            }
        }
    }
    ride.name = state.workoutMode ? state.workoutDisplayName() : QString();
    ride.graded = kind == RideKind::Workout || kind == RideKind::Cadence;
    if (kind == RideKind::FtpTest) {
        ride.ftpTestMode = state.ftpTestMode;
        ride.ftpTestResult = state.ftpTestResult.value_or(ftptest::Result{});
    }
    ride.start = seconds();
    ride.seconds = std::max(state.secondsPassed, lastReading);
    ride.secondsPassed = state.secondsPassed;
    ride.startTimestamp = state.startTimestamp;
    ride.workoutList = state.workoutList;
    ride.blockGrades = state.blockGrades;
    ride.ftp = begun.ftp;
    ride.powerRanges = begun.powerRanges;
    ride.targetRanges = begun.targetRanges;
    ride.hrm = state.antHRM;
    ride.speed = state.hasSpeedSensor();
    ride.cadenceSensor = state.hasCadenceSensor();
    ride.speedCadence = state.anySpeedCadence();
    ride.wheelCircumference = state.wheelCircumference;
    if (state.workoutMode) {
        ride.ring = ring;
    }
    state.sessionRides.append(ride);
}

QString kindName(RideKind kind) {
    switch (kind) {
    case RideKind::Free:
        return QStringLiteral("Free Ride");
    case RideKind::Workout:
        return QStringLiteral("Workout");
    case RideKind::Cadence:
        return QStringLiteral("Musical Cadence");
    case RideKind::FtpTest:
        return QStringLiteral("FTP Test");
    }
    return {};
}

QString heading(const Ride& ride) {
    const QString kind = kindName(ride.kind);
    if (ride.kind == RideKind::Free || ride.name.isEmpty()) {
        return kind;
    }
    // (an FTP test's name says it's one)
    return ride.kind == RideKind::FtpTest ? ride.name : kind + QStringLiteral(": ") + ride.name;
}

QString note(const Ride& ride) {
    const QString text = minimizedDuration(py::round(ride.secondsPassed));
    return ride.graded ? text : text + QStringLiteral(" · not graded");
}

std::optional<double> average(const Ride& ride) {
    return ride.graded ? grading::workoutAverage(ride.blockGrades) : std::nullopt;
}

QList<RingHistory> ringHistory() {
    QList<RingHistory> history;
    for (const Ride& ride : rides()) {
        if (ride.ring) {
            history.append({ride.ring->seconds, ride.ring->samples, ride.powerRanges});
        }
    }
    return history;
}

QList<RingSample> perSecond(const QList<RingSample>& samples) {
    struct Sums {
        double power = 0;
        int count = 0;
        double cadence = 0;
        int cadenceCount = 0;
    };
    std::map<long long, Sums> seconds;
    for (const RingSample& sample : samples) {
        Sums& sums = seconds[static_cast<long long>(sample.seconds)];
        sums.power += sample.power;
        ++sums.count;
        if (sample.cadence) {
            sums.cadence += *sample.cadence;
            ++sums.cadenceCount;
        }
    }
    QList<RingSample> result;
    for (const auto& [second, sums] : seconds) {
        result.append({second + 0.5, sums.power / sums.count,
                       sums.cadenceCount ? std::optional<double>(sums.cadence / sums.cadenceCount) : std::nullopt});
    }
    return result;
}

Ring ringFromReadings(double length, const std::pair<QList<double>, QList<double>>& power,
                      const std::pair<QList<double>, QList<double>>& cadence, bool hasCadence) {
    Ring ring{length, {}};
    for (int second = 0; second < static_cast<int>(length); ++second) {
        const double now = second + 1.0;
        const std::optional<double> powerNow = recentAverage(power.first, power.second, now, kRingPowerSeconds);
        std::optional<double> cadenceNow;
        if (hasCadence) {
            cadenceNow = recentAverage(cadence.first, cadence.second, now, kRingCadenceSeconds);
            if (!cadenceNow && !powerNow) {
                cadenceNow = 0;
            }
        }
        ring.samples.append({second + 0.5, powerNow.value_or(0), cadenceNow});
    }
    return ring;
}

std::optional<double> recentAverage(const QList<double>& times, const QList<double>& values, double now, double seconds) {
    const qsizetype end = std::upper_bound(times.begin(), times.end(), now) - times.begin();
    if (end == 0 || now - times[end - 1] > grading::kDropoutSeconds) {
        return std::nullopt;
    }
    const qsizetype start = std::upper_bound(times.begin(), times.begin() + end, now - seconds) - times.begin();
    if (start == end) {
        return values[end - 1];
    }
    double sum = 0;
    for (qsizetype i = start; i < end; ++i) {
        sum += values[i];
    }
    return sum / static_cast<double>(end - start);
}

}  // namespace session

}  // namespace awo
