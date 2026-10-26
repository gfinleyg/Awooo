#pragma once

// The one event log, antWorkoutLog.txt in the working folder (ClassAntLogger.py): ANT connection and device events,
// recovery, settings and workout problems, Qt's warnings and crashes. One "<timestamp> [<category>] <message>" line
// each, UTF-8, safe to write from any thread, and it never throws: a log that can't be written mustn't take the
// ANT connection or the screens down with it.
//
// Once it passes kMaxLogBytes its oldest kTrimLines lines are dropped (a read and rewrite, but only every ~50 lines
// once it's that big), so Show Logs reads one file and Clear Logs clears one file.

#include <QString>

namespace awo::log {

extern const QString kLogFile;
constexpr qint64 kMaxLogBytes = 1'000'000;
constexpr int kTrimLines = 50;

void event(const QString& category, const QString& message);
// empties the log, leaving a line that says so (so Show Logs right after reads as intentional)
void clear();
// up to the last maxLines lines, oldest first
QString read(int maxLines = 500);

// Qt's warnings and errors, uncaught C++ exceptions and crashes (with a minidump beside the log,
// AntWorkout_crash.dmp) into the log: the app runs without a console
void installErrorLogging();
// Qt's message handler back to the one before installErrorLogging (tests)
void uninstallErrorLogging();

// (tests: a smaller cap to see the trimming)
void setMaxLogBytesForTesting(qint64 bytes);

}  // namespace awo::log
