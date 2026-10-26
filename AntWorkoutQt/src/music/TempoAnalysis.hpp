#pragma once

// A song's tempo all the way through (MusicCadence.py's analysis), computed as the Python app's numpy computes it (see
// NumpyLike), so the two apps share one analysis cache:
//   1. decoded to mono at kSampleRate (miniaudio, as the Python app's pyminiaudio)
//   2. onset envelope: spectral flux of the log magnitude spectrum, kFps frames a second
//   3. every kStepSeconds, the envelope's autocorrelation over kWindowSeconds; each candidate tempo's strength summed
//      over its octaves, so a beat counted at half or double speed lands in the same "tempo class"
//   4. each window's class (octaves above 60 BPM, 0 to 1) and how clearly it stands out (confidence)
//   5. silent stretches (kSilenceMinSeconds or longer)
//   6. the beats themselves at that tempo: dynamic programming over the onset envelope, for the workout screen's beat

#include <QJsonObject>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace awo::music {

constexpr int kSampleRate = 11025;
constexpr int kNfft = 512;
constexpr int kHop = 128;
constexpr double kFps = static_cast<double>(kSampleRate) / kHop;  // ~86 envelope frames a second
constexpr double kWindowSeconds = 8.0;
constexpr double kStepSeconds = 2.0;
// tempo classes: 60 to 120 BPM, kClassSteps an octave, each checked at 30-60, 60-120, 120-240, 240-480 BPM
constexpr int kClassSteps = 120;
constexpr int kClassOctaves[4] = {-1, 0, 1, 2};
constexpr double kClassOctaveWeights[4] = {0.5, 1.0, 1.0, 0.5};
// silence: kSilenceStepSeconds stretches quieter than kSilenceDb (dBFS) and kSilenceBelowLoud dB under the song's loud
// parts, for at least kSilenceMinSeconds in a row
constexpr double kSilenceStepSeconds = 0.5;
constexpr double kSilenceDb = -50;
constexpr double kSilenceBelowLoud = 30;
constexpr double kSilenceMinSeconds = 3.0;
constexpr int kMedianWindows = 5;
// how strongly the beat tracker keeps to the local tempo (higher: steadier beats, fewer jumps onto off beat hits)
constexpr double kBeatTightness = 100;
// bump when the analysis changes, so cached results from the old one are redone (see cacheVersion)
constexpr int kAnalysisVersion = 3;

// A song's cacheable analysis: duration (seconds), each window's tempo class and confidence (rounded as cached), the
// beats as envelope frames each the gap from the one before, and the silent stretches (start, end seconds); or why it
// couldn't be analyzed
struct Analysis {
    double duration = 0;
    std::vector<double> classes;
    std::vector<double> confidences;
    std::vector<int> beats;
    std::vector<std::pair<double, double>> silences;
    std::optional<QString> error;

    QJsonObject toJson() const;
    static Analysis fromJson(const QJsonObject& data);
};

// mono float samples at kSampleRate; throws std::runtime_error when the file can't be read or decoded
std::vector<float> decode(const QString& path);
std::vector<double> onsetEnvelope(const std::vector<float>& samples);
// (classes, confidences) every kStepSeconds, unrounded
std::pair<std::vector<double>, std::vector<double>> tempoTrack(const std::vector<double>& envelope);
std::vector<int> trackBeats(const std::vector<double>& envelope, const std::vector<double>& classes);
std::vector<std::pair<double, double>> silences(const std::vector<float>& samples);
// a cached analysis' beats, in seconds into the song
std::vector<double> beatSeconds(const Analysis& analysis);
Analysis analyzeFile(const QString& path);

// MusicCadence._unwrap and _median_filter (the beat tracker and the segments use them)
std::vector<double> unwrap(const std::vector<double>& classes);
std::vector<double> medianFilter(const std::vector<double>& values, int width);

}  // namespace awo::music
