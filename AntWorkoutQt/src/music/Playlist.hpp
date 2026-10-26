#pragma once

// Picking songs for Musical Cadence Mode (MusicCadence.py): a folder's songs, an .m3u/.m3u8 playlist's, a song's title
// from its file name, and a random pick that comes as close to a ride's length as it can, spread over cadences.

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

#include "PyRandom.hpp"

namespace awo::music {

extern const QStringList kAudioExtensions;     // .mp3 .wav .flac
extern const QStringList kPlaylistExtensions;  // .m3u .m3u8
// a random pick spreads its songs over cadence bands this wide (RPM)
constexpr int kCadenceBandRpm = 10;

bool isAudio(const QString& path);
bool isPlaylist(const QString& path);
// os.path.normpath on Windows: backslashes, no "." or "x\.."
QString normPath(const QString& path);
// every song in a folder and its subfolders, in name order (a folder's own songs before its subfolders')
QStringList folderSongs(const QString& folder);

struct PlaylistSongs {
    QList<std::pair<QString, std::optional<QString>>> songs;  // (path, the #EXTINF title)
    QStringList missing;                                      // the entries that aren't songs there
};
// a playlist's songs that exist and the entries that don't; relative entries are from the playlist's folder. UTF-8,
// else the Windows codepage
PlaylistSongs readPlaylist(const QString& path);
QString songTitle(const QString& path);

// songs picked at random (each once at most) to come as close to targetSeconds as they can: added while they fit,
// then the leftover filled by whichever unused song comes closest. cadences ({song: rpm, or none for no clear beat})
// spreads the pick: each song comes from whichever kCadenceBandRpm wide band (that still has one that fits) has been
// picked from least, ties at random. Python's random.Random, so a seed picks as the Python app does
QStringList randomSelection(const QStringList& songs, double targetSeconds, const QHash<QString, double>& durations,
                            py::Random& rng, const QHash<QString, std::optional<double>>& cadences = {});

}  // namespace awo::music
