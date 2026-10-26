#pragma once

// Zwift workout files (.zwo) as .awo lines (ZwoConvert.py): the app reads a .zwo through this as the .awo it converts
// to, and zwo2awo writes those lines to .awo files.
//
//   <name>                              T:<name>
//   <FreeRide>/<Freeride>, <MaxEffort>  0:<duration>               (MaxEffort with a "Max effort!" message)
//   <SteadyState>                       1:<duration>,<power>
//   <Warmup>/<Cooldown>/<Ramp>          2:<duration>,<PowerLow>,<PowerHigh>  (start to end, as Zwift rides them)
//   <IntervalsT>                        3:<Repeat>;1:<OnDuration>,<OnPower>;1:<OffDuration>,<OffPower>
//   <textevent> inside a block          |<message>|<seconds>|<start> on that block
//   Cadence="<rpm>" on a block          a "<rpm> RPM" message, and seated high/low cadence for fast/slow ones
//   <ftpOverride>                       the powers as explicit watts (<power> x the override): the workout was
//                                       written for that FTP, whatever the rider's
//
// .awo limits are kept: blocks longer than kMaxBlockSeconds are split, a "ramp" with the same start and end is a
// steady block, powers are clamped to what a workout file allows, and messages lose any | or ; (the .awo field
// separators). An interval's messages in its first rep go on its blocks (an .awo interval shows them every rep); one
// with messages later on is written out rep by rep instead, so each shows once, as in Zwift.

#include <QString>
#include <QStringList>

namespace awo {

struct ZwoConversion {
    QStringList lines;
    QString title;
    QStringList warnings;
    QString error;  // why it couldn't be converted, empty when it could

    bool ok() const { return error.isEmpty(); }
};

ZwoConversion convertZwo(const QString& path);
ZwoConversion convertZwoData(const QByteArray& data);

// The .awo file beside a .zwo file
QString awoPathFor(const QString& zwoPath);

// Converts a .zwo file to an .awo file (beside it when awoPath is empty); the conversion, with its error if it
// couldn't be read or the .awo couldn't be written. writtenPath gets the .awo file's path.
ZwoConversion writeAwo(const QString& zwoPath, const QString& awoPath = {}, QString* writtenPath = nullptr);

}  // namespace awo
