#include "Playlist.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>

namespace awo::music {

const QStringList kAudioExtensions = {QStringLiteral(".mp3"), QStringLiteral(".wav"), QStringLiteral(".flac")};
const QStringList kPlaylistExtensions = {QStringLiteral(".m3u"), QStringLiteral(".m3u8")};

namespace {

bool endsWithAny(const QString& path, const QStringList& endings) {
    const QString lower = path.toLower();
    return std::any_of(endings.begin(), endings.end(), [&](const QString& e) { return lower.endsWith(e); });
}

// Python's str.lower() sort key
bool lowerLess(const QString& a, const QString& b) {
    return a.toLower() < b.toLower();
}

void walk(const QString& folder, QStringList& songs) {
    QDir dir(folder);
    QStringList files = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);
    QStringList dirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    std::stable_sort(files.begin(), files.end(), lowerLess);
    std::stable_sort(dirs.begin(), dirs.end(), lowerLess);
    for (const QString& file : files) {
        if (isAudio(file)) {
            songs.append(normPath(dir.filePath(file)));
        }
    }
    for (const QString& sub : dirs) {
        walk(dir.filePath(sub), songs);
    }
}

}  // namespace

bool isAudio(const QString& path) {
    return endsWithAny(path, kAudioExtensions);
}

bool isPlaylist(const QString& path) {
    return endsWithAny(path, kPlaylistExtensions);
}

QString normPath(const QString& path) {
    if (path.isEmpty()) {
        return QStringLiteral(".");
    }
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

QStringList folderSongs(const QString& folder) {
    QStringList songs;
    walk(folder, songs);
    return songs;
}

PlaylistSongs readPlaylist(const QString& path) {
    PlaylistSongs result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.missing.append(path);
        return result;
    }
    QByteArray raw = file.readAll();
    // .m3u8 is UTF-8; plain .m3u is usually UTF-8 too, else the Windows codepage
    if (raw.startsWith("\xEF\xBB\xBF")) {
        raw.remove(0, 3);
    }
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8(raw);
    if (utf8.hasError()) {
        QStringDecoder codepage("windows-1252");
        text = codepage.isValid() ? codepage(raw) : QString::fromLatin1(raw);
    }
    const QString base = QFileInfo(path).absolutePath();
    std::optional<QString> title;
    for (QString line : text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")))) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(u'#')) {
            if (line.toUpper().startsWith(QStringLiteral("#EXTINF:")) && line.contains(u',')) {
                const QString named = line.section(u',', 1).trimmed();
                title = named.isEmpty() ? std::nullopt : std::optional<QString>(named);
            }
            continue;
        }
        if (line.toLower().startsWith(QStringLiteral("file:///"))) {
            line = line.mid(8);
        }
        const QString songPath = normPath(QDir::isAbsolutePath(line) ? line : QDir(base).filePath(line));
        if (QFileInfo(songPath).isFile() && isAudio(songPath)) {
            result.songs.append({songPath, title});
        } else {
            result.missing.append(line);
        }
        title.reset();
    }
    return result;
}

QString songTitle(const QString& path) {
    const QString name = QFileInfo(QDir::fromNativeSeparators(path)).fileName();
    // (os.path.splitext: the last dot, unless the name only starts with one)
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot > 0 ? name.left(dot) : name;
}

QStringList randomSelection(const QStringList& songs, double targetSeconds, const QHash<QString, double>& durations,
                            py::Random& rng, const QHash<QString, std::optional<double>>& cadences,
                            const QSet<QString>& already) {
    // a band each: its key (none for no clear beat) and members, in the order the bands were first met
    struct Band {
        std::optional<int> key;
        QStringList members;
        int used = 0;
    };
    QList<Band> bands;
    for (const QString& song : songs) {
        if (!durations.value(song)) {
            continue;
        }
        std::optional<int> key;
        if (const auto found = cadences.find(song); found != cadences.end() && found.value()) {
            key = static_cast<int>(std::floor(*found.value() / kCadenceBandRpm));
        }
        auto band = std::find_if(bands.begin(), bands.end(), [&](const Band& b) { return b.key == key; });
        if (band == bands.end()) {
            bands.append({key, {}, 0});
            band = bands.end() - 1;
        }
        band->members.append(song);
    }
    const auto fresh = [&](const QString& song) { return !already.contains(song); };
    for (Band& band : bands) {
        rng.shuffle(band.members);
        // (the songs in already at the back, so a band offers them only once its others don't fit)
        std::stable_partition(band.members.begin(), band.members.end(), fresh);
    }
    QStringList picked;
    double total = 0.0;
    while (true) {
        // each band's first song that still fits
        QList<std::pair<int, QString>> fitting;
        for (int b = 0; b < bands.size(); ++b) {
            for (const QString& song : bands[b].members) {
                if (total + durations.value(song) <= targetSeconds) {
                    fitting.append({b, song});
                    break;
                }
            }
        }
        if (fitting.isEmpty()) {
            break;
        }
        // songs in already only once no other song fits, whichever band it's in
        if (std::any_of(fitting.cbegin(), fitting.cend(), [&](const auto& entry) { return fresh(entry.second); })) {
            fitting.removeIf([&](const auto& entry) { return !fresh(entry.second); });
        }
        int fewest = bands[fitting.front().first].used;
        for (const auto& [b, song] : fitting) {
            fewest = std::min(fewest, bands[b].used);
        }
        QList<std::pair<int, QString>> least;
        for (const auto& entry : fitting) {
            if (bands[entry.first].used == fewest) {
                least.append(entry);
            }
        }
        const auto& [chosen, song] = least[static_cast<qsizetype>(rng.choiceIndex(static_cast<std::uint32_t>(least.size())))];
        bands[chosen].members.removeOne(song);
        ++bands[chosen].used;
        picked.append(song);
        total += durations.value(song);
    }
    // one more song if it lands closer to the target than stopping short does (one in already only if none of the
    // others does)
    QStringList pools[2];
    for (const Band& band : bands) {
        for (const QString& song : band.members) {
            pools[fresh(song) ? 0 : 1].append(song);
        }
    }
    for (const QStringList& pool : pools) {
        if (pool.isEmpty() || total >= targetSeconds) {
            continue;
        }
        const QString* best = &pool.front();
        for (const QString& song : pool) {
            if (std::abs(total + durations.value(song) - targetSeconds) < std::abs(total + durations.value(*best) - targetSeconds)) {
                best = &song;
            }
        }
        if (std::abs(total + durations.value(*best) - targetSeconds) < targetSeconds - total) {
            picked.append(*best);
            break;
        }
    }
    return picked;
}

}  // namespace awo::music
