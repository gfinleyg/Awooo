#include "CadenceWorkout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "NumpyLike.hpp"
#include "Playlist.hpp"
#include "PyCompat.hpp"
#include "WorkoutHelpers.hpp"

namespace awo::music {

namespace {

double medianOf(const std::vector<double>& line, int first, int last) {
    return np::median(line.data() + first, static_cast<std::size_t>(last - first + 1));
}

// how far apart two tempo classes are, the short way around the octave
double classDistance(double a, double b) {
    const double d = std::fmod(std::abs(a - b), 1.0);
    return std::min(d, 1 - d);
}

}  // namespace

std::vector<bool> beatWindows(const Analysis& analysis) {
    const std::vector<double>& classes = analysis.classes;
    const int count = static_cast<int>(classes.size());
    std::vector<bool> beat(count);
    for (int i = 0; i < count; ++i) {
        const double confidence = i < static_cast<int>(analysis.confidences.size()) ? analysis.confidences[i] : 0.0;
        if (confidence >= kMinConfidence) {
            beat[i] = true;
        } else if (confidence >= kSteadyMinConfidence) {
            int agreeing = 0;
            for (const int offset : kSteadyOffsets) {
                for (const int j : {i - offset, i + offset}) {
                    if (j >= 0 && j < count && classDistance(classes[i], classes[j]) <= kSegmentTolerance) {
                        ++agreeing;
                    }
                }
            }
            beat[i] = agreeing >= kSteadyAgreeing;
        }
    }
    return beat;
}

std::vector<Segment> songSegments(const Analysis& analysis, double minSegmentSeconds) {
    const double duration = analysis.duration;
    const std::vector<double>& classes = analysis.classes;
    if (classes.empty()) {
        return {{0.0, duration, std::nullopt}};
    }
    const int count = static_cast<int>(classes.size());
    const std::vector<bool> beat = beatWindows(analysis);
    // each window speaks for the time around its center
    std::vector<double> centers(count);
    for (int i = 0; i < count; ++i) {
        centers[i] = i * kStepSeconds + std::min(kWindowSeconds, duration) / 2;
    }
    const int minWindows = std::max(1, static_cast<int>(np::roundHalfEven(minSegmentSeconds / kStepSeconds)));

    // runs of beat/no beat; no beat runs shorter than the minimum are just bridged over
    std::vector<bool> labels = beat;
    bridgeShortRuns(labels, false, minWindows);
    if (std::none_of(labels.begin(), labels.end(), [](bool b) { return b; })) {
        return {{0.0, duration, std::nullopt}};
    }
    // the tempo line through the beat windows only, filled across the gaps from the nearest beat window
    std::vector<int> beatIndex;
    for (int i = 0; i < count; ++i) {
        if (beat[i]) {
            beatIndex.push_back(i);
        }
    }
    std::vector<double> nearestClasses(count);
    for (int i = 0; i < count; ++i) {
        // (argmin: the first of the nearest)
        int best = beatIndex.front();
        for (const int b : beatIndex) {
            if (std::abs(i - b) < std::abs(i - best)) {
                best = b;
            }
        }
        nearestClasses[i] = classes[best];
    }
    const std::vector<double> line = medianFilter(unwrap(nearestClasses), kMedianWindows);

    // cut into segments: a new one whenever the line leaves the running segment's tempo, and at beat changes
    struct Piece {
        int first, last;
        bool isBeat;
    };
    std::vector<Piece> pieces;
    for (int i = 0; i < count; ++i) {
        if (!pieces.empty() && pieces.back().isBeat == labels[i] &&
            (!labels[i] || std::abs(line[i] - medianOf(line, pieces.back().first, i - 1)) <= kSegmentTolerance)) {
            pieces.back().last = i;
        } else {
            pieces.push_back({i, i, labels[i]});
        }
    }
    // short pieces go to whichever neighbor is closest in tempo (a short no beat piece: to its longer neighbor)
    while (pieces.size() > 1) {
        std::vector<int> lengths;
        for (const Piece& p : pieces) {
            lengths.push_back(p.last - p.first + 1);
        }
        const int shortest = static_cast<int>(std::min_element(lengths.begin(), lengths.end()) - lengths.begin());
        if (lengths[shortest] >= minWindows) {
            break;
        }
        std::vector<int> neighbors;
        for (const int k : {shortest - 1, shortest + 1}) {
            if (k >= 0 && k < static_cast<int>(pieces.size())) {
                neighbors.push_back(k);
            }
        }
        const auto distance = [&](int k) {
            if (pieces[k].isBeat != pieces[shortest].isBeat) {
                return 10 + (1.0 / lengths[k]);
            }
            return std::abs(medianOf(line, pieces[k].first, pieces[k].last) -
                            medianOf(line, pieces[shortest].first, pieces[shortest].last));
        };
        int target = neighbors.front();
        for (const int k : neighbors) {
            if (distance(k) < distance(target)) {
                target = k;
            }
        }
        // the joined piece is beat or not as the longer of the two was
        const bool keepBeat = lengths[target] >= lengths[shortest] ? pieces[target].isBeat : pieces[shortest].isBeat;
        const int low = std::min(shortest, target), high = std::max(shortest, target);
        const Piece joined{pieces[low].first, pieces[high].last, keepBeat};
        pieces.erase(pieces.begin() + low, pieces.begin() + high + 1);
        pieces.insert(pieces.begin() + low, joined);
    }
    // neighbors that ended up within the tolerance of each other join up
    const auto tempo = [&](const Piece& p) { return medianOf(line, p.first, p.last); };
    std::size_t k = 0;
    while (k + 1 < pieces.size()) {
        const Piece a = pieces[k], b = pieces[k + 1];
        if (a.isBeat == b.isBeat && (!a.isBeat || std::abs(tempo(a) - tempo(b)) <= kSegmentTolerance)) {
            pieces.erase(pieces.begin() + k, pieces.begin() + k + 2);
            pieces.insert(pieces.begin() + k, Piece{a.first, b.last, a.isBeat});
        } else {
            ++k;
        }
    }
    std::vector<Segment> segments;
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        const auto& [first, last, isBeat] = pieces[p];
        const double start = p == 0 ? 0.0 : (centers[first - 1] + centers[first]) / 2;
        const double end = p == pieces.size() - 1 ? duration : (centers[last] + centers[last + 1]) / 2;
        std::optional<double> bpm;
        if (isBeat) {
            bpm = 60 * std::pow(2.0, py::mod(medianOf(line, first, last), 1.0));
        }
        segments.push_back({start, end, bpm});
    }
    return withSilences(segments, analysis.silences);
}

std::vector<Segment> withSilences(const std::vector<Segment>& segments,
                                  const std::vector<std::pair<double, double>>& silentStretches) {
    std::vector<Segment> pieces;
    for (const Segment& segment : segments) {
        double position = segment.start;
        for (const auto& [s, e] : silentStretches) {
            if (!(s < segment.end && e > segment.start)) {
                continue;
            }
            const double cutStart = std::max(s, segment.start), cutEnd = std::min(e, segment.end);
            if (cutStart > position) {
                pieces.push_back({position, cutStart, segment.bpm});
            }
            pieces.push_back({cutStart, cutEnd, std::nullopt});
            position = cutEnd;
        }
        if (position < segment.end) {
            pieces.push_back({position, segment.end, segment.bpm});
        }
    }
    std::vector<Segment> joined;
    for (const Segment& piece : pieces) {
        if (!joined.empty() && (piece.end - piece.start < kSilenceMinSeconds || joined.back().bpm == piece.bpm)) {
            joined.back().end = piece.end;
        } else if (!joined.empty() && joined.back().end - joined.back().start < kSilenceMinSeconds) {
            joined.back().end = piece.end;
            joined.back().bpm = piece.bpm;
        } else {
            joined.push_back(piece);
        }
    }
    return joined;
}

void bridgeShortRuns(std::vector<bool>& labels, bool value, int minLength) {
    std::size_t i = 0;
    while (i < labels.size()) {
        if (labels[i] == value) {
            std::size_t j = i;
            while (j < labels.size() && labels[j] == value) {
                ++j;
            }
            if (static_cast<int>(j - i) < minLength) {
                std::fill(labels.begin() + i, labels.begin() + j, !value);
            }
            i = j;
        } else {
            ++i;
        }
    }
}

double foldRpm(double bpm, std::optional<double> previous, double preferred, double low, double high) {
    std::vector<double> options;
    double value = bpm;
    while (value > high) {
        value /= 2;
    }
    while (value * 2 <= high) {
        value *= 2;
    }
    while (value >= low) {
        options.push_back(value);
        value /= 2;
    }
    // (only if high < 2 low)
    if (options.empty()) {
        options.push_back(std::min(std::max(bpm, low), high));
    }
    // (Python's `previous or preferred`: a previous of 0 is none)
    const double anchor = previous && *previous != 0 ? *previous : preferred;
    double best = options.front();
    for (const double v : options) {
        if (std::abs(std::log2(v / anchor)) < std::abs(std::log2(best / anchor))) {
            best = v;
        }
    }
    return best;
}

std::vector<Segment> mergeCloseCadences(std::vector<Segment> sections, double within) {
    const auto rounded = [](const Segment& s) { return np::roundHalfEven(*s.bpm); };
    while (true) {
        // the closest neighbors (the first of them on a tie)
        std::size_t best = sections.size();
        double bestGap = 0;
        for (std::size_t k = 0; k + 1 < sections.size(); ++k) {
            const Segment &a = sections[k], &b = sections[k + 1];
            if (!a.bpm || !b.bpm) {
                continue;
            }
            const double gap = std::abs(rounded(a) - rounded(b));
            if (gap <= within && (best == sections.size() || gap < bestGap)) {
                best = k;
                bestGap = gap;
            }
        }
        if (best == sections.size()) {
            return sections;
        }
        const Segment &a = sections[best], &b = sections[best + 1];
        const double aSeconds = a.end - a.start, bSeconds = b.end - b.start;
        const Segment joined{a.start, b.end, (*a.bpm * aSeconds + *b.bpm * bSeconds) / (aSeconds + bSeconds)};
        sections.erase(sections.begin() + static_cast<std::ptrdiff_t>(best) + 1);
        sections[best] = joined;
    }
}

std::vector<double> pulses(const std::vector<double>& beats, double start, double end, double rpm) {
    std::vector<double> inside;
    for (const double b : beats) {
        if (b >= start && b < end) {
            inside.push_back(b);
        }
    }
    if (inside.size() < 2 || !rpm) {
        return {};
    }
    std::vector<double> gaps;
    for (std::size_t i = 1; i < inside.size(); ++i) {
        gaps.push_back(inside[i] - inside[i - 1]);
    }
    const double beatRpm = 60 / np::median(gaps);
    const int octaves = static_cast<int>(np::roundHalfEven(std::log2(rpm / beatRpm)));
    if (octaves < 0) {
        std::vector<double> every;
        const std::size_t stride = std::size_t(1) << -octaves;
        for (std::size_t i = 0; i < inside.size(); i += stride) {
            every.push_back(inside[i]);
        }
        return every;
    }
    std::vector<double> result = inside;
    for (int o = 0; o < octaves; ++o) {
        std::vector<double> doubled;
        for (std::size_t i = 0; i + 1 < result.size(); ++i) {
            doubled.push_back(result[i]);
            doubled.push_back((result[i + 1] + result[i]) / 2);
        }
        doubled.push_back(result.back());
        result = doubled;
    }
    return result;
}

std::optional<double> songCadence(const Analysis& analysis, double preferred, double minSegmentSeconds) {
    std::optional<double> previous;
    double weighted = 0, seconds = 0;
    for (const Segment& segment : songSegments(analysis, minSegmentSeconds)) {
        if (segment.bpm) {
            previous = foldRpm(*segment.bpm, previous, preferred);
            weighted += *previous * (segment.end - segment.start);
            seconds += segment.end - segment.start;
        }
    }
    return seconds ? std::optional<double>(weighted / seconds) : std::nullopt;
}

CadenceRide buildWorkout(const QList<std::pair<QString, std::optional<QString>>>& songs,
                         const QHash<QString, Analysis>& analyses, double preferred, double minSegmentSeconds) {
    CadenceRide ride;
    double start = 0.0;
    int edge = 0;
    for (const auto& [path, title] : songs) {
        const auto found = analyses.find(path);
        if (found == analyses.end() || found->error || found->duration <= 0) {
            continue;
        }
        const Analysis& analysis = *found;
        const int trackIndex = static_cast<int>(ride.tracks.size());
        ride.tracks.append({path, title && !title->isEmpty() ? *title : songTitle(path), start, analysis.duration});
        const std::vector<double> beats = beatSeconds(analysis);
        // the song's segments at their cadences
        std::vector<Segment> sections = songSegments(analysis, minSegmentSeconds);
        std::optional<double> previous;
        for (Segment& section : sections) {
            if (section.bpm) {
                section.bpm = np::roundHalfEven(foldRpm(*section.bpm, previous, preferred));
                previous = section.bpm;
            }
        }
        bool firstBlock = true;
        for (const Segment& segment : mergeCloseCadences(sections)) {
            int rpm = 0;
            if (segment.bpm) {
                rpm = static_cast<int>(np::roundHalfEven(*segment.bpm));
                for (const double p : pulses(beats, segment.start, segment.end, rpm)) {
                    ride.pulses.append(start + p);
                }
            }
            const int roundedEnd = static_cast<int>(np::roundHalfEven(start + segment.end));
            if (roundedEnd <= edge) {
                continue;
            }
            if (!firstBlock && ride.blocks.back().second == rpm) {
                ride.blocks.back().first += roundedEnd - edge;
            } else {
                if (firstBlock) {
                    ride.songStarts.insert(static_cast<int>(ride.blocks.size()), trackIndex);
                    firstBlock = false;
                }
                ride.blocks.append({roundedEnd - edge, rpm});
            }
            edge = roundedEnd;
        }
        start += analysis.duration;
    }
    return ride;
}

QString songHeader(const MusicTrack& track) {
    return minimizedDuration(py::round(track.duration)) + QStringLiteral(" : ") + track.title;
}

void applyRide(const CadenceRide& ride) {
    AppState& state = appState();
    state.workoutList.clear();
    state.workoutMessages.clear();
    state.workoutDurationSeconds = 0;
    for (const auto& [duration, rpm] : ride.blocks) {
        Block block;
        block.seconds = duration;
        if (rpm != 0) {
            block.type = BlockType::Steady;
            block.power.fill(rpm);
        }
        state.workoutList.append(WorkoutEntry::ofBlock(block));
        state.workoutDurationSeconds += duration;
    }
    state.musicTracks = ride.tracks;
    state.musicSongStarts.clear();
    for (auto it = ride.songStarts.cbegin(); it != ride.songStarts.cend(); ++it) {
        state.musicSongStarts.insert(it.key(), songHeader(ride.tracks[it.value()]));
    }
    state.musicPulses = ride.pulses;
    for (const MusicTrack& track : ride.tracks) {
        const int messageStart = static_cast<int>(track.start);
        const int messageEnd = std::min(messageStart + 8, state.workoutDurationSeconds);
        if (messageEnd > messageStart) {
            state.workoutMessages.append({messageStart, messageEnd, QStringLiteral("Now playing: ") + track.title});
        }
    }
    // (the title bar shows the song being played after it, see HudScreen)
    state.workoutTitle = QStringLiteral("Musical Cadence Mode");
}

}  // namespace awo::music
