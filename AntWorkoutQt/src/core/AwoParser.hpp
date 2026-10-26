#pragma once

// Reading .awo workout files (and .zwo ones, through ZwoConvert) into a Workout. The Python app did this inside the
// setup screen (WorkoutSetupWindow.process_workout_lines and the checks it calls) while awo-verifier.py repeated the
// rules; both use this now.
//
// Each line is one block (power is a multiple of FTP, 0.05 to 3, or explicit watts like 250W):
//   T:<title>                                          the workout's title (one at most)
//   0:<seconds>[,<position>]                           free ride
//   1:<seconds>,<power>[,<position>]                   steady
//   2:<seconds>,<start power>,<end power>[,<position>] ramp (the same start and end is a steady block)
//   3:<repeats>;<block>[;<block>[;<block>]]            an interval of 1 to 3 blocks, each a 0, 1 or 2 line (one block
//                                                      on its own has to be a free ride or steady block, and is
//                                                      ridden as one block that long)
// Any block (an interval's blocks each) can end with timed messages:
//   <block>|<message>[|<seconds shown>[|<start second in block>]]
//   <block>|<message>|<seconds>|<start>|<message>|<seconds>|<start>...   (more than one: all three fields each)

#include <QList>
#include <QString>
#include <QStringList>

#include "WorkoutTypes.hpp"

namespace awo {

constexpr int kMaxBlockSeconds = 1800;
constexpr double kMinFtpMultiplier = 0.05;
constexpr double kMaxFtpMultiplier = 3;
constexpr int kMinExplicitWatts = 1;
constexpr int kMaxExplicitWatts = 2000;
// each difficulty step moves the targets this fraction of FTP (or of an explicit power)
constexpr double kDifficultyStep = 0.02;
// (the Python app took any number, and building a huge one never finished)
constexpr int kMaxIntervalRepeats = 1000;

// Why a workout was rejected: line 0 for the file as a whole, otherwise the line number (from 1) and the text
// the check was looking at
struct AwoError {
    QString reason;
    int line = 0;
    QString text;
};

struct AwoOptions {
    int ftp = 0;
    // longest block allowed (an FTP test's effort can be longer than a workout file's blocks)
    int maxBlockSeconds = kMaxBlockSeconds;
    // every bad line, as awo-verifier reports them; otherwise reading stops at the first
    bool allErrors = false;
};

struct AwoResult {
    Workout workout;  // only to be used when ok()
    QList<AwoError> errors;
    QStringList warnings;  // a .zwo file's conversion notes
    bool ok() const { return errors.isEmpty(); }
};

AwoResult parseAwoLines(const QStringList& lines, const AwoOptions& options);

// A .awo or .zwo file
AwoResult readWorkoutFile(const QString& path, const AwoOptions& options);

// A file's text as lines: \r\n and \r read as \n, without the empty line after a final newline
QStringList splitLines(const QString& contents);

}  // namespace awo
