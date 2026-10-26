#pragma once

// The songs' analyses, cached (MusicCadence.py's cache): musicCadenceCacheANT.json, keyed by file name, size and CRC32
// under cacheVersion(), so a song is only ever analyzed once (by either app: the analysis is the Python app's, see
// TempoAnalysis); the files' CRCs are kept in it too, so later runs only look at the files' sizes and times. And the
// song picker's list from last time, musicSelectionANT.json.

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

#include "TempoAnalysis.hpp"

namespace awo::music {

extern const QString kCacheFile;
extern const QString kSelectionFile;

// what the cached analyses were made with: kAnalysisVersion and the settings the analysis uses
QJsonObject cacheVersion();

struct Cache {
    QJsonObject version;
    QHash<QString, Analysis> songs;  // by cacheKey
};
Cache loadCache();
void saveCache(const Cache& cache);
// saves the cache if a song's CRC had to be worked out from its file since it was last saved
void saveNewCrcs(const Cache& cache);
// <file name>|<size>|<CRC32>: a song renamed, replaced or changed is a different song; one moved is the same. Throws
// std::runtime_error when the file can't be read
QString cacheKey(const QString& path);
std::optional<Analysis> cached(const Cache& cache, const QString& path);
// (tests) the CRCs remembered, as the cache file keeps them: [path, size, modified ns, crc]
struct CrcEntry {
    QString path;
    qint64 size;
    qint64 modifiedNs;
    quint32 crc;
};
QList<CrcEntry> rememberedCrcs();
void forgetCrcs();

// {path: analysis} for every song, from the cache where it can be and a thread pool otherwise (workers: how many at a
// time, none: the cores less one). progress(done, total) after each song; cancelled() returning true stops early
// (what's done is kept)
QHash<QString, Analysis> analyzeSongs(const QStringList& paths, const std::function<void(int, int)>& progress = {},
                                      const std::function<bool()>& cancelled = {}, std::optional<int> workers = {});

// the song picker's list: [path, title, checked] each (malformed entries left out)
struct SelectedSong {
    QString path;
    QString title;
    bool checked = true;
    bool operator==(const SelectedSong&) const = default;
};
QList<SelectedSong> loadSelection();
void saveSelection(const QList<SelectedSong>& songs);

}  // namespace awo::music
