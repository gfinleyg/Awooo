#pragma once

// The songs' analyses, cached (MusicCadence.py's cache): musicCadenceCacheANT.json, keyed by file name, size and CRC32,
// each song with the cacheVersion() it was made with, so a song is only analyzed once (by either app: the analysis is
// the Python app's, see TempoAnalysis), or again when the song picker reprocesses one from an older version (one this
// code can't use, see compatibleVersion, is analyzed again by itself); the files' CRCs are kept in it too, so later runs
// only look at the files' sizes and times. And the song picker's list from last time, musicSelectionANT.json.

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

#include "TempoAnalysis.hpp"

namespace awo::music {

extern const QString kCacheFile;
extern const QString kSelectionFile;

// what a cached analysis was made with: kAnalysisMajor.kAnalysisMinor and the settings the analysis uses, so changing
// either one marks the analyses made before as from an older version
QJsonObject cacheVersion();
// whether an analysis made with this cacheVersion() can be used by this code: the same major version and read by the
// same settings (the beats are envelope frames, the classes a window every step); if not, it's analyzed again
bool compatibleVersion(const QJsonObject& version);
// whether a (usable) analysis is from an older version than this code makes: the song picker can reprocess it
bool isOutdated(const Analysis& analysis);

struct Cache {
    QJsonObject version;
    QHash<QString, Analysis> songs;  // by cacheKey
};
// the songs this code can use, each with the version it was made with (one saved without: the file's, as every song
// was before they each had theirs)
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
// time, none: the cores less one; redo: every one analyzed again, the cache only written). progress(done, total) after
// each song; cancelled() returning true stops early (what's done is kept)
QHash<QString, Analysis> analyzeSongs(const QStringList& paths, const std::function<void(int, int)>& progress = {},
                                      const std::function<bool()>& cancelled = {}, std::optional<int> workers = {},
                                      bool redo = false);

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
