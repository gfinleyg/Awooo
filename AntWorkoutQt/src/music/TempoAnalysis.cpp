#include "TempoAnalysis.hpp"

#include <QFile>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "NumpyLike.hpp"
#include "PyCompat.hpp"
#include "miniaudio.h"

namespace awo::music {

namespace {

using np::Array;

// the lags (frames) for every tempo class at each of its octaves: [octave][class]
std::vector<Array> classLags() {
    std::vector<Array> lags;
    for (const int octave : kClassOctaves) {
        Array row(kClassSteps);
        for (int c = 0; c < kClassSteps; ++c) {
            const double tempoClass = 60 * std::pow(2.0, static_cast<double>(c) / kClassSteps);
            row[c] = 60 * kFps / (tempoClass * std::pow(2.0, octave));
        }
        lags.push_back(row);
    }
    return lags;
}

QString errorText(const char* kind, const QString& what) {
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(kind), what);
}

}  // namespace

QJsonObject Analysis::toJson() const {
    if (error) {
        return {{"error", *error}};
    }
    QJsonArray classesJson, confidencesJson, beatsJson, silencesJson;
    for (const double c : classes) {
        classesJson.append(c);
    }
    for (const double c : confidences) {
        confidencesJson.append(c);
    }
    for (const int b : beats) {
        beatsJson.append(b);
    }
    for (const auto& [start, end] : silences) {
        silencesJson.append(QJsonArray{start, end});
    }
    return {{"duration", duration}, {"classes", classesJson}, {"confidences", confidencesJson},
            {"beats", beatsJson}, {"silences", silencesJson}};
}

Analysis Analysis::fromJson(const QJsonObject& data) {
    Analysis analysis;
    if (data.contains(QStringLiteral("error"))) {
        analysis.error = data.value(QStringLiteral("error")).toString();
        return analysis;
    }
    analysis.duration = data.value(QStringLiteral("duration")).toDouble();
    for (const QJsonValue& c : data.value(QStringLiteral("classes")).toArray()) {
        analysis.classes.push_back(c.toDouble());
    }
    for (const QJsonValue& c : data.value(QStringLiteral("confidences")).toArray()) {
        analysis.confidences.push_back(c.toDouble());
    }
    for (const QJsonValue& b : data.value(QStringLiteral("beats")).toArray()) {
        analysis.beats.push_back(b.toInt());
    }
    for (const QJsonValue& s : data.value(QStringLiteral("silences")).toArray()) {
        const QJsonArray pair = s.toArray();
        analysis.silences.push_back({pair.at(0).toDouble(), pair.at(1).toDouble()});
    }
    return analysis;
}

std::vector<float> decode(const QString& path) {
    // from the bytes rather than the path, as the Python app (miniaudio can't open some non-ASCII names itself)
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("FileNotFoundError: %1").arg(file.errorString()).toStdString());
    }
    const QByteArray data = file.readAll();
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, kSampleRate);
    config.ditherMode = ma_dither_mode_none;
    ma_uint64 frames = 0;
    void* memory = nullptr;
    const ma_result result = ma_decode_memory(data.constData(), static_cast<size_t>(data.size()), &config, &frames, &memory);
    if (result != MA_SUCCESS) {
        throw std::runtime_error(
            QStringLiteral("DecodeError: ('failed to decode data', %1)").arg(static_cast<int>(result)).toStdString());
    }
    const float* samples = static_cast<const float*>(memory);
    std::vector<float> decoded(samples, samples + frames);
    ma_free(memory, nullptr);
    return decoded;
}

// Spectral flux of the log magnitude spectrum, one value a kHop samples
std::vector<double> onsetEnvelope(const std::vector<float>& samples) {
    if (samples.size() < static_cast<std::size_t>(kNfft)) {
        return {};
    }
    const std::size_t frames = (samples.size() - kNfft) / kHop + 1;
    const Array hann = np::hanning(kNfft);
    std::vector<float> window(kNfft);
    for (int i = 0; i < kNfft; ++i) {
        window[i] = static_cast<float>(hann[i]);
    }
    constexpr std::size_t bins = kNfft / 2 + 1;
    std::vector<float> flux(frames);
    std::vector<float> previous(bins), spectrum(bins), diffs(bins);
    Array frame(kNfft);
    for (std::size_t f = 0; f < frames; ++f) {
        const float* in = samples.data() + f * kHop;
        for (int i = 0; i < kNfft; ++i) {
            // (float32 frame times the float32 window, then to double for the transform)
            frame[i] = static_cast<double>(in[i] * window[i]);
        }
        const Array transformed = np::rfft(frame.data(), kNfft, kNfft);
        for (std::size_t b = 0; b < bins; ++b) {
            const double magnitude = std::hypot(transformed[2 * b], transformed[2 * b + 1]);
            spectrum[b] = static_cast<float>(std::log1p(100 * magnitude));
        }
        if (f == 0) {
            previous = spectrum;
        }
        for (std::size_t b = 0; b < bins; ++b) {
            diffs[b] = std::max(spectrum[b] - previous[b], 0.0f);
        }
        flux[f] = np::sum(diffs.data(), bins);
        previous = spectrum;
    }
    // the slowly moving loudness taken out, so only the hits are left
    const int width = static_cast<int>(kFps / 2);
    const Array kernel(width, 1.0 / width);
    const Array fluxDouble(flux.begin(), flux.end());
    const Array local = np::convolveSame(fluxDouble, kernel);
    Array envelope(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        envelope[f] = std::max(fluxDouble[f] - local[f], 0.0);
    }
    return envelope;
}

std::pair<std::vector<double>, std::vector<double>> tempoTrack(const std::vector<double>& envelope) {
    std::size_t width = static_cast<std::size_t>(kWindowSeconds * kFps);
    const std::size_t step = static_cast<std::size_t>(kStepSeconds * kFps);
    if (envelope.size() < width) {
        if (static_cast<double>(envelope.size()) < 2 * kFps) {
            return {};
        }
        width = envelope.size();
    }
    const std::vector<Array> lags = classLags();
    Array positions(width);
    for (std::size_t i = 0; i < width; ++i) {
        positions[i] = static_cast<double>(i);
    }
    std::vector<double> classes, confidences;
    for (std::size_t start = 0; start + width <= envelope.size(); start += step) {
        Array segment(envelope.begin() + start, envelope.begin() + start + width);
        const double segmentMean = np::mean(segment);
        for (double& v : segment) {
            v -= segmentMean;
        }
        Array spectrum = np::rfft(segment.data(), width, 2 * width);
        for (std::size_t b = 0; b < spectrum.size() / 2; ++b) {
            const double magnitude = std::hypot(spectrum[2 * b], spectrum[2 * b + 1]);
            spectrum[2 * b] = magnitude * magnitude;
            spectrum[2 * b + 1] = 0;
        }
        Array acf = np::irfft(spectrum);
        acf.resize(width);
        if (acf[0] <= 0) {
            classes.push_back(0.0);
            confidences.push_back(0.0);
            continue;
        }
        // unbiased, then normalized so acf[0] is 1; lightly smoothed so a slightly off lag still counts
        for (std::size_t i = 0; i < width; ++i) {
            acf[i] = acf[i] / static_cast<double>(width - i);
        }
        const double first = acf[0];
        for (double& v : acf) {
            v = v / first;
        }
        acf = np::convolveSame(acf, {0.25, 0.5, 0.25});
        Array strength(kClassSteps, 0.0);
        for (int o = 0; o < 4; ++o) {
            for (int c = 0; c < kClassSteps; ++c) {
                const double lag = lags[o][c];
                const double valid = lag < static_cast<double>(width - 1) ? 1.0 : 0.0;
                // (numpy: the (octaves, classes) array summed over its first axis, row by row)
                const double value = np::interp(lag, positions, acf) * valid * kClassOctaveWeights[o];
                strength[c] = o == 0 ? 0.0 + value : strength[c] + value;
            }
        }
        const std::size_t best = np::argmax(strength.data(), strength.size());
        const double spread = np::stddev(strength);
        // a parabolic peak, wrapping around the octave
        const double left = strength[(best + kClassSteps - 1) % kClassSteps];
        const double right = strength[(best + 1) % kClassSteps];
        const double denominator = left - 2 * strength[best] + right;
        const double offset = denominator < 0 ? 0.5 * (left - right) / denominator : 0.0;
        classes.push_back(py::mod((static_cast<double>(best) + offset) / kClassSteps, 1.0));
        confidences.push_back(spread > 0 ? (strength[best] - np::mean(strength)) / spread : 0.0);
    }
    return {classes, confidences};
}

std::vector<double> unwrap(const std::vector<double>& classes) {
    std::vector<double> line;
    for (double c : classes) {
        if (!line.empty()) {
            c += np::roundHalfEven(line.back() - c);
        }
        line.push_back(c);
    }
    return line;
}

std::vector<double> medianFilter(const std::vector<double>& values, int width) {
    const int half = width / 2;
    std::vector<double> padded(half, values.front());
    padded.insert(padded.end(), values.begin(), values.end());
    padded.insert(padded.end(), half, values.back());
    std::vector<double> out(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = np::median(padded.data() + i, width);
    }
    return out;
}

// The beats' envelope frames, at the tempo the windows' classes follow (60-120 BPM, the unwrapped line held to one
// octave so the beat level doesn't flip where the class wraps around)
std::vector<int> trackBeats(const std::vector<double>& envelope, const std::vector<double>& classes) {
    if (classes.empty() || static_cast<double>(envelope.size()) < 2 * kFps) {
        return {};
    }
    std::vector<double> line = medianFilter(unwrap(classes), kMedianWindows);
    const double shift = std::floor(np::median(line));
    for (double& v : line) {
        v -= shift;
    }
    const double window = std::min(kWindowSeconds, static_cast<double>(envelope.size()) / kFps);
    Array centers(classes.size());
    for (std::size_t i = 0; i < classes.size(); ++i) {
        centers[i] = (static_cast<double>(i) * kStepSeconds + window / 2) * kFps;
    }
    const std::size_t n = envelope.size();
    Array periods(n);
    for (std::size_t t = 0; t < n; ++t) {
        const double bpm = std::clamp(60 * std::pow(2.0, np::interp(static_cast<double>(t), centers, line)), 40.0, 240.0);
        periods[t] = 60 * kFps / bpm;
    }
    // a beat lands on (or just after) an onset: the envelope lightly smoothed
    const double spread = np::stddev(envelope);
    const double scale = spread != 0 ? spread : 1.0;
    Array scaled(n);
    for (std::size_t t = 0; t < n; ++t) {
        scaled[t] = envelope[t] / scale;
    }
    Array kernel = np::hanning(7);
    const double kernelSum = np::sum(kernel);
    for (double& v : kernel) {
        v = v / kernelSum;
    }
    const Array local = np::convolveSame(scaled, kernel);
    Array score(local);
    std::vector<long long> backlink(n, -1);
    std::map<long long, std::pair<std::vector<long long>, Array>> penalties;
    for (std::size_t t = 0; t < n; ++t) {
        const long long period = static_cast<long long>(np::roundHalfEven(periods[t]));
        auto found = penalties.find(period);
        if (found == penalties.end()) {
            std::vector<long long> gaps;
            Array penalty;
            for (long long g = std::max(1LL, period / 2); g <= 2 * period; ++g) {
                gaps.push_back(g);
                const double logRatio = std::log(static_cast<double>(g) / static_cast<double>(period));
                penalty.push_back(-kBeatTightness * (logRatio * logRatio));
            }
            found = penalties.emplace(period, std::make_pair(gaps, penalty)).first;
        }
        const auto& [gaps, penalty] = found->second;
        double best = 0;
        long long bestGap = -1;
        for (std::size_t g = 0; g < gaps.size() && gaps[g] <= static_cast<long long>(t); ++g) {
            const double candidate = score[t - gaps[g]] + penalty[g];
            if (bestGap < 0 || candidate > best) {
                best = candidate;
                bestGap = gaps[g];
            }
        }
        if (bestGap < 0) {
            continue;
        }
        score[t] = local[t] + best;
        backlink[t] = static_cast<long long>(t) - bestGap;
    }
    // the last beat: the best scoring frame in the final period, then back through the chain
    const std::size_t tail = static_cast<std::size_t>(np::roundHalfEven(periods.back()));
    long long beat = static_cast<long long>(n - tail + np::argmax(score.data() + n - tail, tail));
    std::vector<int> beats;
    while (beat >= 0) {
        beats.push_back(static_cast<int>(beat));
        beat = backlink[beat];
    }
    std::reverse(beats.begin(), beats.end());
    return beats;
}

std::vector<std::pair<double, double>> silences(const std::vector<float>& samples) {
    const std::size_t step = static_cast<std::size_t>(kSilenceStepSeconds * kSampleRate);
    const std::size_t count = samples.size() / step;
    if (count == 0) {
        return {};
    }
    Array db(count);
    Array squares(step);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = 0; j < step; ++j) {
            const double v = samples[i * step + j];
            squares[j] = v * v;
        }
        const double rms = std::sqrt(np::sum(squares) / static_cast<double>(step));
        db[i] = 20 * std::log10(std::max(rms, 1e-9));
    }
    const double threshold = std::min(kSilenceDb, np::percentile(db, 95) - kSilenceBelowLoud);
    std::vector<std::pair<double, double>> stretches;
    std::size_t i = 0;
    while (i < count) {
        if (db[i] < threshold) {
            std::size_t j = i;
            while (j < count && db[j] < threshold) {
                ++j;
            }
            // (a song's last partial step goes with its last whole one)
            const double end = j == count ? static_cast<double>(samples.size()) / kSampleRate
                                          : static_cast<double>(j) * kSilenceStepSeconds;
            if (end - static_cast<double>(i) * kSilenceStepSeconds >= kSilenceMinSeconds) {
                stretches.push_back({py::round(static_cast<double>(i) * kSilenceStepSeconds, 2), py::round(end, 2)});
            }
            i = j;
        } else {
            ++i;
        }
    }
    return stretches;
}

std::vector<double> beatSeconds(const Analysis& analysis) {
    std::vector<double> seconds;
    long long frame = 0;
    for (const int gap : analysis.beats) {
        frame += gap;
        seconds.push_back((static_cast<double>(frame) * kHop + kNfft / 2.0) / kSampleRate);
    }
    return seconds;
}

Analysis analyzeFile(const QString& path) {
    Analysis analysis;
    try {
        const std::vector<float> samples = decode(path);
        const std::vector<double> envelope = onsetEnvelope(samples);
        const auto [classes, confidences] = tempoTrack(envelope);
        const std::vector<int> beats = trackBeats(envelope, classes);
        analysis.duration = static_cast<double>(samples.size()) / kSampleRate;
        for (const double c : classes) {
            analysis.classes.push_back(py::round(c, 4));
        }
        for (const double c : confidences) {
            analysis.confidences.push_back(py::round(c, 2));
        }
        int previous = 0;
        for (const int b : beats) {
            analysis.beats.push_back(b - previous);
            previous = b;
        }
        analysis.silences = silences(samples);
    } catch (const std::exception& e) {
        // a corrupt or unreadable file just can't be used
        analysis = Analysis{};
        analysis.error = QString::fromStdString(e.what());
    }
    return analysis;
}

}  // namespace awo::music
