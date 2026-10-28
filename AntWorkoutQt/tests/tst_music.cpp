// Musical Cadence Mode's music side (src/music) against the Python app's (golden/music.json, make_golden.py's
// music_golden): the same click tracks (made from Python's random, byte for byte) decode, analyze and build the same
// ride, down to the onset envelope; random picks and cadence folding the same. Then tests/test_music_cadence.py's
// tests: picking songs, playlists, the analysis' edge cases, segments, folding, pulses, the ride, the cache and the
// selection file.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <cmath>
#include <numeric>

#include "AnalysisCache.hpp"
#include "ClickTrack.hpp"
#include "CadenceWorkout.hpp"
#include "MusicPlayer.hpp"
#include "NumpyLike.hpp"
#include "Playlist.hpp"
#include "PyRandom.hpp"
#include "TempoAnalysis.hpp"
#include "TestSupport.hpp"


using namespace awo;
using namespace awo::music;

namespace {

quint32 fileCrc(const QString& path) {
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    const QByteArray data = file.readAll();
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : data) {
        crc ^= static_cast<unsigned char>(c);
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    return ~crc;
}

std::vector<double> doubles(const QJsonValue& value) {
    std::vector<double> out;
    for (const QJsonValue& v : value.toArray()) {
        out.push_back(v.toDouble());
    }
    return out;
}

QJsonArray segmentsJson(const std::vector<Segment>& segments) {
    QJsonArray out;
    for (const Segment& s : segments) {
        out.append(QJsonArray{s.start, s.end, s.bpm ? QJsonValue(*s.bpm) : QJsonValue()});
    }
    return out;
}

// numbers the same, to within relative (or absolute, near 0) tol
bool closeTo(double a, double b, double tol) {
    return std::abs(a - b) <= tol * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

Analysis analysisOf(const std::vector<double>& classes, std::vector<double> confidences = {},
                    std::optional<double> duration = {}, std::vector<std::pair<double, double>> silences = {}) {
    Analysis a;
    a.classes = classes;
    a.confidences = confidences.empty() ? std::vector<double>(classes.size(), 5.0) : confidences;
    a.duration = duration.value_or(static_cast<double>(classes.size()) * kStepSeconds + kWindowSeconds - kStepSeconds);
    a.silences = std::move(silences);
    return a;
}

void touch(const QString& path, const QByteArray& data = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    awotest::writeFile(path, data);
}

}  // namespace

class TestMusic : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        golden_ = awotest::golden(QStringLiteral("music.json")).toObject();
        // the click tracks, made here as the Python app made them
        for (const QJsonValue& value : golden_[QStringLiteral("tracks")].toArray()) {
            const QJsonObject track = value.toObject();
            std::optional<std::pair<double, double>> silence, change;
            if (track[QStringLiteral("silence")].isArray()) {
                const QJsonArray s = track[QStringLiteral("silence")].toArray();
                silence = {s[0].toDouble(), s[1].toDouble()};
            }
            if (track[QStringLiteral("change")].isArray()) {
                const QJsonArray c = track[QStringLiteral("change")].toArray();
                change = {c[0].toDouble(), c[1].toDouble()};
            }
            const QString name = track[QStringLiteral("name")].toString();
            const QString path = songs_.filePath(name + QStringLiteral(".wav"));
            awotest::writeClickTrack(path, track[QStringLiteral("bpm")].toDouble(), track[QStringLiteral("seconds")].toDouble(),
                            track[QStringLiteral("seed")].toInt(), silence, change, golden_[QStringLiteral("rate")].toInt());
            paths_.insert(name, QDir::toNativeSeparators(path));
            tracks_.insert(name, track);
            analyses_.insert(name, analyzeFile(paths_[name]));
        }
    }

    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        forgetCrcs();
    }
    void cleanup() { dir_.reset(); }

    // ---- The Python app's results ----

    void goldenTracks_data() {
        QTest::addColumn<QString>("name");
        for (const QString& name : {"a_100", "b_75", "c_gap", "d_change", "e_172"}) {
            QTest::newRow(qPrintable(name)) << name;
        }
    }

    // the same file, decoded the same (the same miniaudio), the same envelope, tempo track, beats and silences
    void goldenTracks() {
        QFETCH(QString, name);
        const QJsonObject track = tracks_[name];
        const QString path = paths_[name];
        QCOMPARE(fileCrc(path), static_cast<quint32>(track[QStringLiteral("crc")].toInteger()));

        const std::vector<float> samples = decode(path);
        const QJsonObject s = track[QStringLiteral("samples")].toObject();
        QCOMPARE(static_cast<qint64>(samples.size()), s[QStringLiteral("count")].toInteger());
        const std::vector<double> first = doubles(s[QStringLiteral("first")]);
        for (std::size_t i = 0; i < first.size(); ++i) {
            QCOMPARE(static_cast<double>(samples[i]), first[i]);
        }
        const std::vector<double> at = doubles(s[QStringLiteral("at")]);
        for (std::size_t i = 0; i < at.size(); ++i) {
            QCOMPARE(static_cast<double>(samples[i * 9973]), at[i]);
        }

        const std::vector<double> envelope = onsetEnvelope(samples);
        const std::vector<double> pyEnvelope = doubles(track[QStringLiteral("envelope")]);
        QCOMPARE(envelope.size(), pyEnvelope.size());
        int exact = 0;
        for (std::size_t i = 0; i < envelope.size(); ++i) {
            exact += envelope[i] == pyEnvelope[i];
            QVERIFY2(closeTo(envelope[i], pyEnvelope[i], 1e-12),
                     qPrintable(QStringLiteral("envelope %1: %2, Python %3").arg(i).arg(envelope[i], 0, 'g', 17).arg(pyEnvelope[i], 0, 'g', 17)));
        }
        qInfo("%s: %d of %d envelope values exactly the Python app's", qPrintable(name), exact, static_cast<int>(envelope.size()));

        const auto [classes, confidences] = tempoTrack(envelope);
        const std::vector<double> pyClasses = doubles(track[QStringLiteral("classes")]);
        const std::vector<double> pyConfidences = doubles(track[QStringLiteral("confidences")]);
        QCOMPARE(classes.size(), pyClasses.size());
        for (std::size_t i = 0; i < classes.size(); ++i) {
            QVERIFY2(closeTo(classes[i], pyClasses[i], 1e-9), qPrintable(QStringLiteral("class %1").arg(i)));
            QVERIFY2(closeTo(confidences[i], pyConfidences[i], 1e-9), qPrintable(QStringLiteral("confidence %1").arg(i)));
        }
        std::vector<int> pyBeats;
        for (const QJsonValue& b : track[QStringLiteral("beats")].toArray()) {
            pyBeats.push_back(b.toInt());
        }
        QCOMPARE(trackBeats(envelope, classes), pyBeats);

        // the cached analysis: exactly the Python app's (so the two share the cache)
        const QJsonObject analysis = analyses_[name].toJson();
        const QJsonObject pyAnalysis = track[QStringLiteral("analysis")].toObject();
        QVERIFY2(analysis == pyAnalysis, qPrintable(QJsonDocument(analysis).toJson(QJsonDocument::Compact).left(400)));
        // and what's worked out from it
        QCOMPARE(segmentsJson(songSegments(analyses_[name])), track[QStringLiteral("segments")].toArray());
        QCOMPARE(segmentsJson(songSegments(analyses_[name], 4)), track[QStringLiteral("segments4")].toArray());
        const auto cadence = songCadence(analyses_[name]);
        QCOMPARE(cadence ? QJsonValue(*cadence) : QJsonValue(), track[QStringLiteral("cadence")]);
        const auto cadence100 = songCadence(analyses_[name], 100);
        QCOMPARE(cadence100 ? QJsonValue(*cadence100) : QJsonValue(), track[QStringLiteral("cadence100")]);
    }

    // real songs (not kept in the repo): AWO_SONGS_JSON names a file of {path: the Python app's analyze_file} to match
    void realSongsMatchThePythonApp() {
        const QString file = qEnvironmentVariable("AWO_SONGS_JSON");
        if (file.isEmpty()) {
            QSKIP("set AWO_SONGS_JSON to check real songs");
        }
        QFile json(file);
        QVERIFY(json.open(QIODevice::ReadOnly));
        const QJsonObject songs = QJsonDocument::fromJson(json.readAll()).object();
        for (auto it = songs.begin(); it != songs.end(); ++it) {
            const QJsonObject ours = analyzeFile(it.key()).toJson();
            QVERIFY2(ours == it.value().toObject(), qPrintable(it.key()));
            qInfo("%s: the same", qPrintable(QFileInfo(it.key()).fileName()));
        }
    }

    void goldenWorkouts() {
        const QList<std::pair<QString, std::optional<QString>>> order = {
            {paths_["a_100"], std::nullopt}, {paths_["b_75"], QStringLiteral("Slow One")}, {paths_["c_gap"], std::nullopt},
            {paths_["d_change"], std::nullopt}, {paths_["e_172"], QStringLiteral("Fast")}};
        QHash<QString, Analysis> byPath;
        for (auto it = paths_.cbegin(); it != paths_.cend(); ++it) {
            byPath.insert(it.value(), analyses_[it.key()]);
        }
        for (const QJsonValue& value : golden_[QStringLiteral("workouts")].toArray()) {
            const QJsonObject py = value.toObject();
            const CadenceRide ride = buildWorkout(order, byPath, py[QStringLiteral("preferred")].toDouble(),
                                                  py[QStringLiteral("minSegment")].toDouble());
            QJsonArray blocks, tracks, pulses, headers;
            for (const auto& [seconds, rpm] : ride.blocks) {
                blocks.append(QJsonArray{seconds, rpm});
            }
            for (const MusicTrack& track : ride.tracks) {
                tracks.append(QJsonArray{QFileInfo(track.path).fileName(), track.title, track.start, track.duration});
                headers.append(songHeader(track));
            }
            QJsonObject starts;
            for (auto it = ride.songStarts.cbegin(); it != ride.songStarts.cend(); ++it) {
                starts.insert(QString::number(it.key()), it.value());
            }
            QCOMPARE(blocks, py[QStringLiteral("blocks")].toArray());
            QCOMPARE(tracks, py[QStringLiteral("tracks")].toArray());
            QCOMPARE(starts, py[QStringLiteral("songStarts")].toObject());
            QCOMPARE(headers, py[QStringLiteral("headers")].toArray());
            const std::vector<double> pyPulses = doubles(py[QStringLiteral("pulses")]);
            QCOMPARE(static_cast<std::size_t>(ride.pulses.size()), pyPulses.size());
            for (std::size_t i = 0; i < pyPulses.size(); ++i) {
                QCOMPARE(ride.pulses[static_cast<qsizetype>(i)], pyPulses[i]);
            }
        }
    }

    void goldenRandomPicksAndFolds() {
        const QJsonObject picks = golden_[QStringLiteral("picks")].toObject();
        QStringList songs;
        for (const QJsonValue& s : picks[QStringLiteral("songs")].toArray()) {
            songs.append(s.toString());
        }
        QHash<QString, double> durations;
        const QJsonObject durationsJson = picks[QStringLiteral("durations")].toObject();
        for (auto it = durationsJson.begin(); it != durationsJson.end(); ++it) {
            durations.insert(it.key(), it.value().toDouble());
        }
        QHash<QString, std::optional<double>> cadences;
        const QJsonObject cadencesJson = picks[QStringLiteral("cadences")].toObject();
        for (auto it = cadencesJson.begin(); it != cadencesJson.end(); ++it) {
            cadences.insert(it.key(), it.value().isNull() ? std::nullopt : std::optional<double>(it.value().toDouble()));
        }
        for (const QJsonValue& value : picks[QStringLiteral("cases")].toArray()) {
            const QJsonObject c = value.toObject();
            py::Random rng(static_cast<std::uint64_t>(c[QStringLiteral("seed")].toInt()));
            const QStringList picked = randomSelection(songs, c[QStringLiteral("target")].toDouble(), durations, rng,
                                                       c[QStringLiteral("cadences")].toBool() ? cadences
                                                                                             : QHash<QString, std::optional<double>>());
            QStringList pyPicked;
            for (const QJsonValue& s : c[QStringLiteral("picked")].toArray()) {
                pyPicked.append(s.toString());
            }
            QCOMPARE(picked, pyPicked);
        }
        for (const QJsonValue& value : golden_[QStringLiteral("folds")].toArray()) {
            const QJsonArray f = value.toArray();
            const std::optional<double> previous = f[1].isNull() ? std::nullopt : std::optional<double>(f[1].toDouble());
            QCOMPARE(foldRpm(f[0].toDouble(), previous, f[2].toDouble()), f[3].toDouble());
        }
        QVERIFY(cacheVersion() == golden_[QStringLiteral("cacheVersion")].toObject() ||
                QJsonDocument(cacheVersion()).toJson() == QJsonDocument(golden_[QStringLiteral("cacheVersion")].toObject()).toJson());
    }

    // numpy's arithmetic: pairwise sums, convolve, interp, percentile (values worked out with numpy 1.26)
    void numpyLike() {
        np::Array a(1000);
        for (int i = 0; i < 1000; ++i) {
            a[i] = std::sin(i * 0.37) * 1000 + i;
        }
        // np.sum: pairwise, so not the plain sum of the same numbers
        double plain = 0;
        for (const double v : a) {
            plain += v;
        }
        QVERIFY(std::abs(np::sum(a) - plain) < 1e-6);
        QCOMPARE(np::median({3.0, 1.0, 2.0, 10.0}), 2.5);
        QCOMPARE(np::percentile({1, 2, 3, 4, 5}, 95), 4.8);
        QCOMPARE(np::interp(1.5, {0, 1, 2}, {0, 10, 30}), 20.0);
        QCOMPARE(np::interp(-1, {0, 1, 2}, {0, 10, 30}), 0.0);
        QCOMPARE(np::interp(5, {0, 1, 2}, {0, 10, 30}), 30.0);
        QCOMPARE(np::convolveSame({1, 2, 3, 4}, {0.25, 0.5, 0.25}), (np::Array{1.0, 2.0, 3.0, 2.75}));
        const np::Array window = np::hanning(5);
        QCOMPARE(window, (np::Array{0.0, 0.5, 1.0, 0.5, 0.0}));
        // rfft then irfft: back again
        const np::Array spectrum = np::rfft(a.data(), 16, 16);
        const np::Array back = np::irfft(spectrum);
        for (int i = 0; i < 16; ++i) {
            QVERIFY(std::abs(back[i] - a[i]) < 1e-9);
        }
    }

    // ---- Picking songs ----

    void audioAndPlaylistNames() {
        QVERIFY(isAudio("a.MP3") && isAudio("b.flac") && isAudio("c.wav") && !isAudio("d.ogg"));
        QVERIFY(isPlaylist("x.M3U") && isPlaylist("y.m3u8") && !isPlaylist("z.pls"));
        QCOMPARE(songTitle(QStringLiteral("music\\Artist - Song.mp3")), QStringLiteral("Artist - Song"));
    }

    void folderSongsInNameOrder() {
        const QString root = dir_->path();
        for (const char* name : {"B/2.mp3", "B/1.wav", "a/z.flac", "top.mp3", "notes.txt"}) {
            touch(QDir(root).filePath(QString::fromLatin1(name)));
        }
        QStringList found;
        for (const QString& path : folderSongs(root)) {
            found.append(QDir(root).relativeFilePath(QDir::fromNativeSeparators(path)));
        }
        QCOMPARE(found, (QStringList{"top.mp3", "a/z.flac", "B/1.wav", "B/2.mp3"}));
    }

    void readsAPlaylist() {
        QDir root(dir_->path());
        touch(root.filePath("music/one.mp3"));
        touch(root.filePath("two.wav"));
        touch(root.filePath("notes.txt"), "x");
        const QString absolute = QDir::toNativeSeparators(root.filePath("two.wav"));
        awotest::writeFile(root.filePath("list.m3u8"),
                           QByteArray("\xEF\xBB\xBF") + QStringLiteral("#EXTM3U\n#EXTINF:123,Band - One\nmusic/one.mp3\n\n"
                                                                       "file:///%1\nmissing.mp3\nnotes.txt\n")
                                                            .arg(absolute)
                                                            .toUtf8());
        const PlaylistSongs result = readPlaylist(root.filePath("list.m3u8"));
        QCOMPARE(result.songs.size(), qsizetype(2));
        QCOMPARE(result.songs[0].first, normPath(root.filePath("music/one.mp3")));
        QCOMPARE(result.songs[0].second, std::optional<QString>(QStringLiteral("Band - One")));
        QCOMPARE(result.songs[1].first, normPath(absolute));
        QVERIFY(!result.songs[1].second);
        QCOMPARE(result.missing, (QStringList{"missing.mp3", "notes.txt"}));
    }

    void playlistCodepageAndMissingFile() {
        QDir root(dir_->path());
        touch(root.filePath(QStringLiteral("café.mp3")));
        awotest::writeFile(root.filePath("old.m3u"), QByteArray("caf\xe9.mp3\n"));
        const PlaylistSongs result = readPlaylist(root.filePath("old.m3u"));
        QCOMPARE(result.songs.size(), qsizetype(1));
        QCOMPARE(QFileInfo(result.songs[0].first).fileName(), QStringLiteral("café.mp3"));
        const PlaylistSongs none = readPlaylist(root.filePath("none.m3u"));
        QVERIFY(none.songs.isEmpty() && none.missing == QStringList{root.filePath("none.m3u")});
    }

    void randomSelectionComesClose() {
        const QHash<QString, double> durations = {{"a", 100}, {"b", 200}, {"c", 300}, {"d", 400}, {"none", 0}};
        py::Random rng(4);
        const QStringList picked = randomSelection({"a", "b", "c", "d", "none"}, 600, durations, rng);
        QCOMPARE(QSet<QString>(picked.begin(), picked.end()).size(), picked.size());
        QVERIFY(!picked.contains("none"));
        double total = 0;
        for (const QString& s : picked) {
            total += durations[s];
        }
        QVERIFY(std::abs(total - 600) <= 100);
        py::Random one(1);
        // (over, but closer than nothing; a tie: stopping short)
        QCOMPARE(randomSelection({"a"}, 60, durations, one), QStringList{"a"});
        QVERIFY(randomSelection({"a"}, 50, durations, one).isEmpty());
        QVERIFY(randomSelection({"d"}, 100, durations, one).isEmpty());
    }

    // nine songs at 60 RPM and one each at 80, 100 and free ride: every band gets a song before any gets a second
    void randomSelectionSpreadsCadences() {
        QStringList songs;
        for (int i = 0; i < 9; ++i) {
            songs.append(QStringLiteral("slow%1").arg(i));
        }
        songs += {"mid", "fast", "free"};
        QHash<QString, double> durations;
        QHash<QString, std::optional<double>> cadences;
        for (const QString& s : songs) {
            durations.insert(s, 100);
            cadences.insert(s, s.startsWith("slow") ? std::optional<double>(60) : std::nullopt);
        }
        cadences["mid"] = 80;
        cadences["fast"] = 100;
        for (int seed = 0; seed < 20; ++seed) {
            py::Random rng(static_cast<std::uint64_t>(seed));
            const QStringList picked = randomSelection(songs, 400, durations, rng, cadences);
            QCOMPARE(picked.size(), qsizetype(4));
            QVERIFY(picked.contains("mid") && picked.contains("fast") && picked.contains("free"));
        }
    }

    // the songs already picked come only once the rest run out, in any band, and for the last song too
    void randomSelectionPicksNewSongsFirst() {
        const QStringList songs = {"a", "b", "c", "d", "e", "f"};
        QHash<QString, double> durations;
        for (const QString& s : songs) {
            durations.insert(s, 100);
        }
        const QHash<QString, std::optional<double>> cadences = {
            {"a", 60}, {"b", 80}, {"c", 100}, {"d", 60}, {"e", 60}, {"f", 60}};
        const QSet<QString> already = {"a", "b", "c"};
        for (int seed = 0; seed < 20; ++seed) {
            py::Random rng(static_cast<std::uint64_t>(seed));
            QStringList picked = randomSelection(songs, 300, durations, rng, cadences, already);
            std::sort(picked.begin(), picked.end());
            QCOMPARE(picked, (QStringList{"d", "e", "f"}));
            const QStringList more = randomSelection(songs, 400, durations, rng, cadences, already);
            QCOMPARE(more.size(), qsizetype(4));
            QVERIFY(more.contains("d") && more.contains("e") && more.contains("f"));
            // (the last one over the target: a new song when one lands closer than stopping short)
            const QStringList over = randomSelection(songs, 260, durations, rng, cadences, {"a", "b", "c", "d", "e"});
            QCOMPARE(over.size(), qsizetype(3));
            QVERIFY(over.contains("f"));
        }
    }

    // ---- The analysis ----

    void songCadenceOfAClickTrack() {
        QVERIFY(std::abs(*songCadence(analyses_["a_100"], 100) - 100) <= 2);
        Analysis empty;
        empty.duration = 30;
        QVERIFY(!songCadence(empty));
    }

    void tempoOfAClickTrack_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<double>("bpm");
        QTest::newRow("100") << QStringLiteral("a_100") << 100.0;
        QTest::newRow("75") << QStringLiteral("b_75") << 75.0;
    }

    void tempoOfAClickTrack() {
        QFETCH(QString, name);
        QFETCH(double, bpm);
        const Analysis& analysis = analyses_[name];
        QVERIFY(!analysis.error);
        QVERIFY(std::abs(analysis.duration - 40) <= 0.01);
        const std::vector<Segment> segments = songSegments(analysis);
        QCOMPARE(segments.size(), std::size_t(1));
        QCOMPARE(segments[0].start, 0.0);
        QVERIFY(std::abs(segments[0].end - 40) <= 0.01);
        // the tempo class is per octave: 75 BPM and 150 BPM are the same class
        QVERIFY(std::abs(*segments[0].bpm / bpm - 1) <= 0.03);
    }

    void beatsOfAClickTrack() {
        const std::vector<double> beats = beatSeconds(analyses_["a_100"]);
        std::vector<double> gaps;
        for (std::size_t i = 1; i < beats.size(); ++i) {
            gaps.push_back(beats[i] - beats[i - 1]);
        }
        const double median = np::median(gaps);
        QVERIFY(std::abs(median / 0.6 - 1) <= 0.03 || std::abs(median / 0.3 - 1) <= 0.03);
        QVERIFY(beats.front() < 1.0 && beats.back() > 38);
    }

    void silenceBecomesFreeRide() {
        const Analysis& analysis = analyses_["c_gap"];
        QVERIFY(std::any_of(analysis.silences.begin(), analysis.silences.end(),
                            [](const auto& s) { return s.first <= 15.5 && s.second >= 23.5; }));
        const std::vector<Segment> segments = songSegments(analysis);
        QCOMPARE(segments.size(), std::size_t(3));
        QVERIFY(segments[0].bpm && !segments[1].bpm && segments[2].bpm);
        QVERIFY(std::abs(segments[1].start - 15) <= 0.6 && std::abs(segments[1].end - 24) <= 0.6);
    }

    void findsSilences() {
        std::vector<float> samples;
        const auto add = [&samples](double seconds, float value) {
            samples.insert(samples.end(), static_cast<std::size_t>(seconds * kSampleRate), value);
        };
        add(5, 0.3f);
        add(4, 0.0f);
        add(5, 0.3f);
        add(2, 0.0f);
        // (found to the nearest step)
        const auto found = silences(samples);
        QCOMPARE(found.size(), std::size_t(1));
        QVERIFY(std::abs(found[0].first - 5.0) <= kSilenceStepSeconds && std::abs(found[0].second - 9.0) <= 0.01);
        QVERIFY(silences(std::vector<float>(10, 0.0f)).empty());
    }

    void decodesAWav() {
        const QString path = QDir(dir_->path()).filePath("x.wav");
        awotest::writeClickTrack(path, 90, 3, 1);
        QVERIFY(std::abs(static_cast<double>(decode(path).size()) - 3 * kSampleRate) <= 2);
    }

    void unreadableSongIsAnError() {
        const QString bad = QDir(dir_->path()).filePath("bad.mp3");
        awotest::writeFile(bad, "not audio");
        QVERIFY(analyzeFile(bad).error.has_value());
        QVERIFY(analyzeFile(QDir(dir_->path()).filePath("none.mp3")).error.has_value());
    }

    void tinyInputs() {
        QVERIFY(onsetEnvelope(std::vector<float>(10, 0.0f)).empty());
        QVERIFY(tempoTrack(std::vector<double>(10, 0.0)).first.empty());
        QVERIFY(trackBeats(std::vector<double>(10, 0.0), {}).empty());
    }

    // ---- Segments from an analysis ----

    void segmentsTempoChange() {
        std::vector<double> classes(15, 0.2);
        classes.insert(classes.end(), 15, 0.6);
        const Analysis a = analysisOf(classes);
        const std::vector<Segment> segments = songSegments(a);
        QCOMPARE(segments.size(), std::size_t(2));
        QVERIFY(std::abs(*segments[0].bpm - 60 * std::pow(2, 0.2)) < 1e-9);
        QVERIFY(std::abs(*segments[1].bpm - 60 * std::pow(2, 0.6)) < 1e-9);
        QCOMPARE(segments[0].end, segments[1].start);
        QCOMPARE(segments.back().end, a.duration);
    }

    // 119 BPM and 121 BPM are 0.99 and 0.01 octaves above 60: one steady tempo
    void segmentsWrapAroundTheOctaveIsOneTempo() {
        std::vector<double> classes;
        for (int i = 0; i < 6; ++i) {
            classes.insert(classes.end(), {0.99, 0.005, 0.99, 0.005});
        }
        QCOMPARE(songSegments(analysisOf(classes)).size(), std::size_t(1));
    }

    void segmentsShortChangesAreMerged() {
        std::vector<double> classes(10, 0.2);
        classes.insert(classes.end(), 2, 0.6);
        classes.insert(classes.end(), 10, 0.2);
        QCOMPARE(songSegments(analysisOf(classes), 8).size(), std::size_t(1));
    }

    void segmentsNoClearBeat() {
        const Analysis unclear = analysisOf(std::vector<double>(10, 0.3), std::vector<double>(10, 0.5));
        QCOMPARE(songSegments(unclear), (std::vector<Segment>{{0.0, unclear.duration, std::nullopt}}));
        Analysis empty;
        empty.duration = 30;
        QCOMPARE(songSegments(empty), (std::vector<Segment>{{0.0, 30, std::nullopt}}));
        // a short unclear stretch is bridged over
        std::vector<double> confidences(8, 5.0);
        confidences.insert(confidences.end(), 2, 0.5);
        confidences.insert(confidences.end(), 10, 5.0);
        const std::vector<Segment> segments = songSegments(analysisOf(std::vector<double>(20, 0.3), confidences));
        QVERIFY(segments.size() == 1 && segments[0].bpm);
    }

    // a less confident window still has a beat when its tempo agrees with enough windows that share none of its audio
    void segmentsSteadyTempoIsABeat() {
        const std::vector<double> steady(20, 0.3);
        const Analysis held = analysisOf(steady, std::vector<double>(20, 2.0));
        const std::vector<bool> heldBeat = beatWindows(held);
        QVERIFY(std::all_of(heldBeat.begin(), heldBeat.end(), [](bool b) { return b; }));
        const std::vector<Segment> segments = songSegments(held);
        QVERIFY(segments.size() == 1 && segments[0].bpm && std::abs(*segments[0].bpm - 60 * std::pow(2, 0.3)) < 1e-9);
        // (across the octave's wrap too)
        std::vector<double> wrapping;
        for (int i = 0; i < 10; ++i) {
            wrapping.insert(wrapping.end(), {0.99, 0.005});
        }
        QVERIFY(songSegments(analysisOf(wrapping, std::vector<double>(20, 2.0)))[0].bpm);
        // too unsure even so
        QVERIFY(!songSegments(analysisOf(steady, std::vector<double>(20, 1.0)))[0].bpm);
        // a tempo that wanders: no beat
        std::vector<double> wandering;
        for (int i = 0; i < 20; ++i) {
            wandering.push_back(std::fmod(i * 0.29, 1.0));
        }
        QVERIFY(!songSegments(analysisOf(wandering, std::vector<double>(20, 2.0)))[0].bpm);
        // agreeing only with its overlapping neighbors isn't enough: windows 4-6 away are at other tempos
        std::vector<double> classes = wandering;
        classes[9] = classes[10] = classes[11] = 0.5;
        const std::vector<bool> beat = beatWindows(analysisOf(classes, std::vector<double>(20, 2.0)));
        QVERIFY(!beat[10]);
        // two of them agreeing is
        classes[4] = classes[14] = 0.5;
        QVERIFY(beatWindows(analysisOf(classes, std::vector<double>(20, 2.0)))[10]);
    }

    void cutsOutSilences() {
        QCOMPARE(withSilences({{0.0, 30.0, 100.0}, {30.0, 60.0, 120.0}}, {{25.0, 35.0}}),
                 (std::vector<Segment>{{0.0, 25.0, 100.0}, {25.0, 35.0, std::nullopt}, {35.0, 60.0, 120.0}}));
        // a leftover shorter than a silence goes to its neighbor
        QCOMPARE(withSilences({{0.0, 30.0, 100.0}}, {{1.0, 10.0}}),
                 (std::vector<Segment>{{0.0, 10.0, std::nullopt}, {10.0, 30.0, 100.0}}));
    }

    void bridgesShortRuns() {
        std::vector<bool> labels = {true, false, true, true, false, false, false, true};
        bridgeShortRuns(labels, false, 2);
        QCOMPARE(labels, (std::vector<bool>{true, true, true, true, false, false, false, true}));
    }

    void unwraps() {
        const std::vector<double> line = unwrap({0.9, 0.05, 0.1, 0.95});
        const std::vector<double> expected = {0.9, 1.05, 1.1, 0.95};
        for (std::size_t i = 0; i < line.size(); ++i) {
            QVERIFY(std::abs(line[i] - expected[i]) < 1e-12);
        }
    }

    void foldsRpm_data() {
        QTest::addColumn<double>("bpm");
        QTest::addColumn<double>("previous");  // (0: none)
        QTest::addColumn<double>("rpm");
        const double rows[][3] = {{100, 0, 100}, {60, 0, 120},  {160, 0, 80}, {200, 0, 100}, {45, 0, 90},  {30, 0, 120},
                                  {120, 0, 120}, {60, 50, 60}, {110, 60, 55}, {55, 100, 110}, {250, 0, 62.5}};
        for (const auto& row : rows) {
            QTest::newRow(qPrintable(QStringLiteral("%1 after %2").arg(row[0]).arg(row[1]))) << row[0] << row[1] << row[2];
        }
    }

    void foldsRpm() {
        QFETCH(double, bpm);
        QFETCH(double, previous);
        QFETCH(double, rpm);
        QCOMPARE(foldRpm(bpm, previous ? std::optional<double>(previous) : std::nullopt), rpm);
    }

    void foldRpmNarrowRange() {
        QCOMPARE(foldRpm(100, std::nullopt, kDefaultPreferredRpm, 90, 95), 95.0);
    }

    void pulsesAtTheCadence() {
        std::vector<double> beats;
        for (int i = 0; i < 20; ++i) {
            beats.push_back(i * 0.5);  // 120 BPM
        }
        QCOMPARE(pulses(beats, 0, 10, 120), beats);
        std::vector<double> everyOther;
        for (std::size_t i = 0; i < beats.size(); i += 2) {
            everyOther.push_back(beats[i]);
        }
        QCOMPARE(pulses(beats, 0, 10, 60), everyOther);
        QCOMPARE(pulses(beats, 0, 2, 240), (std::vector<double>{0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5}));
        QVERIFY(pulses(beats, 0, 0.4, 120).empty() && pulses(beats, 0, 10, 0).empty());
    }

    // ---- The ride ----

    void buildsTheRide() {
        Analysis gone;
        gone.error = QStringLiteral("gone");
        const QHash<QString, Analysis> byPath = {
            {paths_["a_100"], analyses_["a_100"]}, {paths_["b_75"], analyses_["b_75"]}, {paths_["c_gap"], analyses_["c_gap"]},
            {QStringLiteral("missing.wav"), gone}};
        const CadenceRide ride = buildWorkout({{paths_["a_100"], std::nullopt},
                                               {paths_["b_75"], QStringLiteral("Slow One")},
                                               {QStringLiteral("missing.wav"), std::nullopt},
                                               {paths_["c_gap"], std::nullopt}},
                                              byPath);
        QStringList titles;
        for (const MusicTrack& t : ride.tracks) {
            titles.append(t.title);
        }
        QCOMPARE(titles, (QStringList{"a_100", "Slow One", "c_gap"}));
        QVERIFY(std::abs(ride.tracks[1].start - 40) <= 0.01 && std::abs(ride.tracks[2].start - 80) <= 0.01);
        // the blocks add up to the rounded playlist length, each a whole number of seconds
        int total = 0;
        QList<int> rpms;
        for (const auto& [seconds, rpm] : ride.blocks) {
            QVERIFY(seconds > 0);
            total += seconds;
            rpms.append(rpm);
        }
        QCOMPARE(total, 120);
        QCOMPARE(rpms, (QList<int>{100, 75, 100, 0, 100}));
        QCOMPARE(ride.songStarts, (QMap<int, int>{{0, 0}, {1, 1}, {2, 2}}));
        QVERIFY(std::is_sorted(ride.pulses.begin(), ride.pulses.end()) && ride.pulses.back() < 120);
        QCOMPARE(songHeader(ride.tracks[1]), QStringLiteral("40s : Slow One"));
    }

    void rideMergesTheSameCadenceWithinASong() {
        std::vector<double> classes(15, 0.2);
        classes.insert(classes.end(), 15, 0.21);
        const CadenceRide ride = buildWorkout({{QStringLiteral("s.wav"), QStringLiteral("S")}}, {{QStringLiteral("s.wav"), analysisOf(classes)}});
        QCOMPARE(ride.blocks.size(), qsizetype(1));
    }

    // sections a song's cadences put 1 or 2 RPM apart are one, at their average over their time
    void mergesCloseCadences() {
        using Sections = std::vector<Segment>;
        QCOMPARE(mergeCloseCadences({{0, 30, 86.0}, {30, 40, 88.0}, {40, 60, 91.0}}),
                 (Sections{{0, 40, 86.5}, {40, 60, 91.0}}));
        // the closest first: 88 and 89, then 86 and their 88.5
        const Sections chain = mergeCloseCadences({{0, 10, 86.0}, {10, 20, 88.0}, {20, 30, 89.0}});
        QVERIFY(chain.size() == 1 && chain[0].end == 30 && std::abs(*chain[0].bpm - (86 + 88 + 89) / 3.0) < 1e-9);
        // 3 apart, or free ride between them: kept apart
        const Sections apart = {{0, 10, 86.0}, {10, 20, 89.0}};
        QCOMPARE(mergeCloseCadences(apart), apart);
        const Sections freeBetween = {{0, 10, 86.0}, {10, 20, std::nullopt}, {20, 30, 87.0}};
        QCOMPARE(mergeCloseCadences(freeBetween), freeBetween);
    }

    // 112 and 108 BPM are separate tempos, but ridden at half speed they're 56 and 54 RPM: one block at 55
    void rideMergesCloseCadencesWithinASong() {
        std::vector<double> classes(15, std::log2(112 / 60.0));
        classes.insert(classes.end(), 15, std::log2(108 / 60.0));
        const Analysis analysis = analysisOf(classes);
        QCOMPARE(songSegments(analysis).size(), std::size_t(2));
        const CadenceRide ride = buildWorkout({{QStringLiteral("s.wav"), QStringLiteral("S")}},
                                              {{QStringLiteral("s.wav"), analysis}}, 50);
        QCOMPARE(ride.blocks, (QList<std::pair<int, int>>{{66, 55}}));
    }

    // the player: the songs back to back on the device's clock (filled by hand here, no sound device)
    void playerPositionFollowsTheFrames() {
        QList<MusicTrack> tracks = {{paths_["e_172"], QStringLiteral("one"), 0, analyses_["e_172"].duration},
                                    {QStringLiteral("nowhere.wav"), QStringLiteral("gone"), analyses_["e_172"].duration, 30},
                                    {paths_["a_100"], QStringLiteral("two"), analyses_["e_172"].duration + 30, 40}};
        MusicPlayer player(tracks);
        std::vector<qint16> buffer(4410 * MusicPlayer::kChannels);
        player.fill(buffer.data(), 4410);
        QVERIFY(player.position() > 0 && player.position() <= 0.1 + 1e-9);
        QVERIFY(std::any_of(buffer.begin(), buffer.end(), [](qint16 s) { return s != 0; }));
        // paused: silence, the position holds
        player.paused = true;
        player.fill(buffer.data(), 4410);
        QVERIFY(std::all_of(buffer.begin(), buffer.end(), [](qint16 s) { return s == 0; }));
        player.paused = false;
        // the rest of the first song, then past the missing one to the third
        for (int i = 0; i < 210; ++i) {
            player.fill(buffer.data(), 4410);
        }
        QCOMPARE(player.skipped().size(), qsizetype(1));
        QVERIFY(player.position() > analyses_["e_172"].duration + 30);
        // the volume scales the samples
        player.volume = 0.0;
        player.fill(buffer.data(), 4410);
        QVERIFY(std::all_of(buffer.begin(), buffer.end(), [](qint16 s) { return s == 0; }));
    }

    // the real sound device (AWO_SOUND set: it plays, silently, for a second): the music's clock moves with it
    void playsOnTheSoundDevice() {
        if (qEnvironmentVariable("AWO_SOUND").isEmpty()) {
            QSKIP("set AWO_SOUND to play on the sound device");
        }
        MusicPlayer player({{paths_["a_100"], QStringLiteral("one"), 0, analyses_["a_100"].duration}});
        player.volume = 0.0;
        player.start();
        QTest::qWait(1200);
        const double position = player.position();
        player.stop();
        qInfo("played to %.2f s (latency taken off)", position);
        QVERIFY(position > 0.5 && position < 1.3);
    }

    // ---- The cache and the selection ----

    void cacheRoundTrip() {
        Cache cache = loadCache();
        QVERIFY(cache.songs.isEmpty() && cache.version == cacheVersion());
        QVERIFY(!cached(cache, paths_["a_100"]));
        cache.songs.insert(cacheKey(paths_["a_100"]), analyses_["a_100"]);
        saveCache(cache);
        forgetCrcs();
        const Cache loaded = loadCache();
        QCOMPARE(cached(loaded, paths_["a_100"])->toJson(), analyses_["a_100"].toJson());
        QVERIFY(!rememberedCrcs().isEmpty());
        QCOMPARE(rememberedCrcs().front().crc, fileCrc(paths_["a_100"]));
        QVERIFY(!cached(loaded, QStringLiteral("nowhere.wav")));
        // the key: the file name (lowercase), its size and CRC
        QCOMPARE(cacheKey(paths_["a_100"]), QStringLiteral("a_100.wav|%1|%2")
                                                 .arg(QFileInfo(paths_["a_100"]).size())
                                                 .arg(fileCrc(paths_["a_100"]), 8, 16, QLatin1Char('0')));
    }

    void cacheFromAnotherVersionIsDropped() {
        awotest::writeFile(kCacheFile, R"({"version": {"analysis": -1}, "songs": {"x": {}}})");
        QVERIFY(loadCache().songs.isEmpty());
        awotest::writeFile(kCacheFile, "{broken");
        QVERIFY(loadCache().songs.isEmpty());
    }

    void cacheKeyFollowsTheContents() {
        QDir root(dir_->path());
        const QString first = root.filePath("one/song.wav"), second = root.filePath("two/song.wav");
        touch(first, "abc");
        touch(second, "abc");
        // moved to another folder: the same song
        QCOMPARE(cacheKey(first), cacheKey(second));
        awotest::writeFile(second, "abd");
        // (a file's CRC is remembered by its size and modified time: moved on a second so it can't look unchanged)
        QFile file(second);
        file.open(QIODevice::ReadWrite);
        file.setFileTime(QFileInfo(second).lastModified().addSecs(1), QFileDevice::FileModificationTime);
        file.close();
        QVERIFY(cacheKey(first) != cacheKey(second));
    }

    void analyzeSongsUsesTheCache() {
        Cache cache = loadCache();
        Analysis fake;
        fake.duration = 1.0;
        cache.songs.insert(cacheKey(paths_["a_100"]), fake);
        saveCache(cache);
        QList<std::pair<int, int>> progress;
        const auto results = analyzeSongs({paths_["a_100"], paths_["a_100"]}, [&](int d, int t) { progress.append({d, t}); });
        QCOMPARE(results[paths_["a_100"]].duration, 1.0);
        QCOMPARE(progress, (QList<std::pair<int, int>>{{1, 1}}));
    }

    void analyzeSongsOnAWorker() {
        QList<std::pair<int, int>> progress;
        const auto results = analyzeSongs({paths_["b_75"]}, [&](int d, int t) { progress.append({d, t}); }, {}, 1);
        QCOMPARE(results[paths_["b_75"]].classes, analyses_["b_75"].classes);
        QCOMPARE(progress, (QList<std::pair<int, int>>{{0, 1}, {1, 1}}));
        // saved for next time
        QVERIFY(cached(loadCache(), paths_["b_75"]).has_value());
    }

    void selectionRoundTrip() {
        QVERIFY(loadSelection().isEmpty());
        awotest::writeFile(kSelectionFile, R"([["a.mp3", "A", true], ["bad"], ["b.mp3", "B", false]])");
        QCOMPARE(loadSelection(), (QList<SelectedSong>{{"a.mp3", "A", true}, {"b.mp3", "B", false}}));
        saveSelection({{"c.mp3", "C", true}});
        QCOMPARE(loadSelection(), (QList<SelectedSong>{{"c.mp3", "C", true}}));
    }

private:
    QTemporaryDir songsDir_;
    QDir songs_{songsDir_.path()};
    QJsonObject golden_;
    QHash<QString, QString> paths_;
    QHash<QString, QJsonObject> tracks_;
    QHash<QString, Analysis> analyses_;
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_GUILESS_MAIN(TestMusic)
#include "tst_music.moc"
