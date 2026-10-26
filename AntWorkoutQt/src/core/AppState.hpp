#pragma once

// The app's shared state (AppState.py): the rider's settings, the mode, the workout loaded and the ride being
// ridden. One object for now, read and written as the Python app's class attributes were; it moves into the objects
// that own each part as the screens are ported (PLAN.md §4.1).

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>

#include <optional>

#include "FtpColors.hpp"
#include "FtpTest.hpp"
#include "Session.hpp"
#include "WorkoutGrading.hpp"
#include "WorkoutTypes.hpp"

namespace awo {

// A song of the ride, in workout time, for the music player
struct MusicTrack {
    QString path;
    QString title;
    double start = 0;
    double duration = 0;
};

// A sensor's readings for the workout screen's graph, (secondsPassed, value), the graph's window of them
struct SavedValues {
    QList<double> times;
    QList<double> values;

    void clear() {
        times.clear();
        values.clear();
    }
};

// A song as it was heard: its track and when, in secondsPassed
struct SongHeard {
    int track = 0;
    double start = 0;
    double end = 0;
};

struct AppState {
    // the rider (0: not set)
    int ftp = 0;
    int maxHr = 0;
    double wheelCircumference = 0;  // meters
    // the zones' limits: the power zones, or Musical Cadence Mode's cadence zones while it's selected
    ftpcolors::ZoneRanges ftpRanges;

    // the mode: workout (a file, an FTP test or Musical Cadence Mode) or free
    bool workoutMode = true;
    QString ftpTestMode;  // an FTP test's key; workoutMode is set for these too
    std::optional<ftptest::Result> ftpTestResult;
    // Musical Cadence Mode: the targets are cadences (RPM) worked out from music, which plays while riding; the
    // workout's "power" values hold the RPM
    bool cadenceMode = false;
    QList<MusicTrack> musicTracks;
    QMap<int, QString> musicSongStarts;  // {workout entry index: the song's header line} for each song's first block
    QList<SongHeard> musicPlayed;
    QList<double> musicPulses;  // beat pulse times (workout seconds), one per pedal stroke

    // the workout
    QString workoutFilename;
    QString workoutFilenameShort;
    QString workoutTitle;  // from its T: line, shown in place of the filename
    int workoutDurationSeconds = 0;
    QList<WorkoutEntry> workoutList;
    QList<Message> workoutMessages;

    // the ride
    double secondsPassed = 0;
    QList<double> workoutPowerRequestsSeconds;
    QList<double> workoutPowerRequestsWatts;
    QList<QDateTime> workoutPowerRequestsSysTime;
    grading::Grades blockGrades;
    QDateTime startTimestamp;  // when the workout screen was entered
    int totalRevolutions = 0;  // wheel revolutions, for the distance
    SavedValues savedPowerVals;
    SavedValues savedHRVals;
    SavedValues savedCadenceVals;

    // the session's rides, what the ride being ridden kept from START, and whether it has joined the session
    QList<Ride> sessionRides;
    std::optional<RideStart> rideStart;
    bool rideRecorded = false;

    // the ANT devices in use
    bool antHRM = false;
    bool antSpeedCadence = false;
    bool antCadence = false;
    bool antSpeed = false;

    // top of the free mode watts slider with an FE-C trainer
    int maxWatts = 1200;
    // workout mode with an FE-C trainer: the trainer is released while the workout asks for more than this
    int powerDriveCutoff = 0;

    // graph
    int graphSecondsShown = 720;
    int graphBreakpoint = 240;

    // its title if the file has one, otherwise its filename
    QString workoutDisplayName() const { return workoutTitle.isEmpty() ? workoutFilenameShort : workoutTitle; }
    // any speed and/or cadence source (combined 121, or discrete 122/123)
    bool anySpeedCadence() const { return antSpeedCadence || antCadence || antSpeed; }
    // the workout screen has a cadence gauge: a speed/cadence source, or Musical Cadence Mode (which falls back to
    // the power meter's own cadence without one)
    bool showsCadence() const { return anySpeedCadence() || (workoutMode && cadenceMode); }
    // the unit of the workout's targets
    QString targetUnit() const { return cadenceMode && workoutMode ? QStringLiteral("RPM") : QStringLiteral("Watts"); }
    bool hasSpeedSensor() const { return antSpeedCadence || antSpeed; }
    bool hasCadenceSensor() const { return antSpeedCadence || antCadence; }
};

AppState& appState();

}  // namespace awo
