#pragma once

// The rides since the app was opened, or since Clear Session on the setup screen (Session.py), kept in memory only.
// The synopsis shows them all, joined end to end with a divider where each starts, and the workout screen's spiral
// ring carries on from them.
//
// A ride joins the session when its synopsis opens (Exit to Synopsis); one left with Back to Setup doesn't. Each
// keeps the FTP it was ridden at: an FTP changed after it (typed in, or an FTP test's) doesn't recolor it. The zones'
// colors (the palette, the theme) are the same for every ride.

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>

#include <optional>
#include <utility>

#include "FtpColors.hpp"
#include "FtpTest.hpp"
#include "WorkoutData.hpp"
#include "WorkoutGrading.hpp"
#include "WorkoutTypes.hpp"

namespace awo {

enum class RideKind { Free, Workout, Cadence, FtpTest };

// One second (or less) of the spiral's ring: the power and cadence (none when there was none to show) at seconds
struct RingSample {
    double seconds = 0;
    double power = 0;
    std::optional<double> cadence;

    bool operator==(const RingSample&) const = default;
};

struct Ring {
    double seconds = 0;
    QList<RingSample> samples;
};

// What a ride keeps from when START was pressed
struct RideStart {
    int ftp = 0;
    ftpcolors::ZoneRanges powerRanges;   // the power zones; the ride is drawn with these
    ftpcolors::ZoneRanges targetRanges;  // the zones its targets were colored by (cadence ones in Musical Cadence Mode)
};

// A song heard, in AppState secondsPassed
struct SongSpan {
    QString title;
    double start = 0;
    double end = 0;
};

// A ride in the session; times are seconds from its own start unless said otherwise
struct Ride {
    RideKind kind = RideKind::Free;
    QString name;  // what the title bar showed for it, empty for a free ride
    bool graded = false;
    QString ftpTestMode;  // the FTP test's key, else empty
    std::optional<ftptest::Result> ftpTestResult;
    double start = 0;          // where it starts in the session (the seconds of the rides before it)
    double seconds = 0;        // how long it is in the session: until it ended, or its last reading if that was later
    double secondsPassed = 0;  // AppState secondsPassed as it ended
    QDateTime startTimestamp;
    double rideOffset = 0;  // what to add to an AppState secondsPassed time for its seconds
    WorkoutData data;       // its readings
    QList<double> powerRequestSeconds;
    QList<double> powerRequestWatts;
    QList<QDateTime> powerRequestTimes;
    QList<WorkoutEntry> workoutList;
    grading::Grades blockGrades;
    QList<SongSpan> songs;
    QMap<int, QString> songHeaders;  // {workout entry index: song title} of each song's first block, for the grade list
    int ftp = 0;
    ftpcolors::ZoneRanges powerRanges;
    ftpcolors::ZoneRanges targetRanges;
    bool hrm = false;
    bool speed = false;
    bool cadenceSensor = false;
    bool speedCadence = false;
    double wheelCircumference = 0;  // meters
    std::optional<Ring> ring;  // what a workout's ring had, a free ride's from its readings (filled by the synopsis)
};

namespace session {

constexpr double kRingPowerSeconds = 3.0;
constexpr double kRingCadenceSeconds = 1.0;

QList<Ride>& rides();
// how long the session's rides are, one after the other
double seconds();
void clear();

// What a ride keeps from START, from the app's state now
RideStart startValues();
// START on the setup screen: a new ride, not in the session until its synopsis opens
void beginRide();
RideKind rideKind();
// The ride just ridden (the app's state and readings) joins the session, once; ring: the workout screen's spiral's
void addCurrentRide(std::optional<Ring> ring = std::nullopt);

QString kindName(RideKind kind);
// A ride's heading: its kind and name
QString heading(const Ride& ride);
// What its heading row says at the right: how long it was, and if it wasn't graded
QString note(const Ride& ride);
std::optional<double> average(const Ride& ride);

// The rides' rings, with each one's power zones, for the spiral to start with
struct RingHistory {
    double seconds;
    QList<RingSample> samples;
    ftpcolors::ZoneRanges powerRanges;
};
QList<RingHistory> ringHistory();

// (seconds, power, cadence) samples as one a second: each second's averages (cadence none if none had one)
QList<RingSample> perSecond(const QList<RingSample>& samples);
// A ride's ring from its readings ((times, values) each, in its own seconds): the power and cadence as the spiral
// would have taken them
Ring ringFromReadings(double length, const std::pair<QList<double>, QList<double>>& power,
                      const std::pair<QList<double>, QList<double>>& cadence, bool hasCadence);
// The readings' average over the seconds up to now, the last one if none came in that time, or none after
// grading::kDropoutSeconds without any
std::optional<double> recentAverage(const QList<double>& times, const QList<double>& values, double now, double seconds);

}  // namespace session

}  // namespace awo
