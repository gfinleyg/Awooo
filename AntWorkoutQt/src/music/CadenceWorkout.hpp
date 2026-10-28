#pragma once

// A playlist's ride of target cadences (MusicCadence.py): each song's analysis cut into segments of one tempo (or
// none: free ride), each segment's tempo folded into a cadence, and the songs one after the other as blocks.
//   - the classes are unwrapped into one continuous tempo line (so 119 -> 121 BPM doesn't flip to 60.5), median
//     filtered, cut wherever the tempo moves more than kSegmentTolerance, and anything shorter than the minimum segment
//     is merged into its nearest neighbor
//   - stretches with no clear beat (confidence under kMinConfidence, and no steady tempo either: see beatWindows)
//     become free ride, and so does silence
//   - each segment's tempo is doubled/halved into kMinRpm..kMaxRpm, keeping to the previous segment's octave within a
//     song, else the one nearest the preferred cadence; neighbors that end up within kMergeRpm of each other are
//     ridden as one section at their average

#include <QHash>
#include <QMap>
#include <QString>

#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "AppState.hpp"
#include "TempoAnalysis.hpp"

namespace awo::music {

constexpr double kMinRpm = 40, kMaxRpm = 120;
constexpr double kDefaultPreferredRpm = 85;
constexpr double kDefaultMinSegmentSeconds = 8;
// windows with less than this confidence have no clear beat...
constexpr double kMinConfidence = 2.5;
// ...unless the tempo holds steady around them: a window with at least kSteadyMinConfidence whose tempo is within
// kSegmentTolerance of at least kSteadyAgreeing of the windows kSteadyOffsets either side of it (far enough away that
// they share none of its audio, so they can't agree just by hearing the same notes)
constexpr double kSteadyMinConfidence = 1.5;
constexpr int kSteadyOffsets[] = {4, 5, 6};
constexpr int kSteadyAgreeing = 2;
// a new segment starts when the tempo moves more than this (octaves, ~3%)
constexpr double kSegmentTolerance = 0.043;
// neighboring sections of a song whose cadences are this close (RPM) are ridden as one (see mergeCloseCadences)
constexpr double kMergeRpm = 2;

struct Segment {
    double start = 0, end = 0;  // seconds into the song
    std::optional<double> bpm;  // none: no clear beat (free ride); else somewhere in 60-120
    bool operator==(const Segment&) const = default;
};

// which of the analysis' windows have a clear beat: confident ones, and less confident ones whose tempo holds steady
std::vector<bool> beatWindows(const Analysis& analysis);
// The segments covering the whole song
std::vector<Segment> songSegments(const Analysis& analysis, double minSegmentSeconds = kDefaultMinSegmentSeconds);
// the segments with each silent stretch cut out of them as free ride (MusicCadence._with_silences)
std::vector<Segment> withSilences(const std::vector<Segment>& segments,
                                  const std::vector<std::pair<double, double>>& silentStretches);
// runs of value shorter than minLength flip to the other value (MusicCadence._bridge_short_runs)
void bridgeShortRuns(std::vector<bool>& labels, bool value, int minLength);
// bpm doubled/halved into low..high: the octave nearest previous when there is one, else nearest preferred
double foldRpm(double bpm, std::optional<double> previous = std::nullopt, double preferred = kDefaultPreferredRpm,
               double low = kMinRpm, double high = kMaxRpm);
// a song's sections (bpm holding each one's cadence) with neighbors whose cadences (rounded) are within RPM of each
// other joined into one at their average cadence over their time, the closest neighbors first; free ride sections
// keep the ones either side of them apart
std::vector<Segment> mergeCloseCadences(std::vector<Segment> sections, double within = kMergeRpm);
// the beats from start to end as one pulse a pedal stroke at rpm (MusicCadence._pulses)
std::vector<double> pulses(const std::vector<double>& beats, double start, double end, double rpm);
// a song's typical cadence (its beat segments' rpm averaged over their time), or none without a clear beat
std::optional<double> songCadence(const Analysis& analysis, double preferred = kDefaultPreferredRpm,
                                  double minSegmentSeconds = kDefaultMinSegmentSeconds);

struct CadenceRide {
    QList<std::pair<int, int>> blocks;  // (seconds, rpm or 0 for free ride), on whole seconds of playlist time
    QList<MusicTrack> tracks;           // the songs in playlist time (exact, for the music clock)
    QMap<int, int> songStarts;          // {block index: track index} of each song's first block
    QList<double> pulses;               // the beat pulse times, playlist seconds
};
// songs: (path, title or none for the file's); analyses by path (missing or an error: left out)
CadenceRide buildWorkout(const QList<std::pair<QString, std::optional<QString>>>& songs,
                         const QHash<QString, Analysis>& analyses, double preferred = kDefaultPreferredRpm,
                         double minSegmentSeconds = kDefaultMinSegmentSeconds);
// a song's line in the workout lists: <length> : <title>
QString songHeader(const MusicTrack& track);
// The ride as the app's workout (AppState, as the setup screen's build_music_workout): a steady block a cadence (the
// RPM where a workout file's blocks have watts, the same at every difficulty), free ride where there's no clear beat,
// each song's title as a timed message as it starts, and the songs, their headers and the beat pulses
void applyRide(const CadenceRide& ride);

}  // namespace awo::music
