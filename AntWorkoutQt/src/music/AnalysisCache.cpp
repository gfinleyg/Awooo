#include "AnalysisCache.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

#include <windows.h>

namespace awo::music {

const QString kCacheFile = QStringLiteral("musicCadenceCacheANT.json");
const QString kSelectionFile = QStringLiteral("musicSelectionANT.json");

namespace {

// {(normcase'd absolute path, size, modified ns): crc}, so a song's file is only read for its CRC once (kept in the
// cache file too); whether one was worked out since the cache was last saved
std::mutex crcMutex;
std::map<std::tuple<QString, qint64, qint64>, quint32> crcs;
bool crcsAdded = false;

// zlib's crc32
quint32 crc32(quint32 crc, const char* data, qint64 length) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (qint64 i = 0; i < length; ++i) {
        crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Python's os.path.normcase(os.path.abspath(path)) on Windows
QString normcaseAbsolute(const QString& path) {
    return QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath())).toLower();
}

// os.stat's st_size and st_mtime_ns; false if the file isn't there
bool statFile(const QString& path, qint64& size, qint64& modifiedNs) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GetFileExInfoStandard,
                              &data)) {
        return false;
    }
    size = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const qint64 ticks = (static_cast<qint64>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    modifiedNs = (ticks - 116444736000000000LL) * 100;
    return true;
}

quint32 fileCrc(const QString& path, qint64 size, qint64 modifiedNs) {
    const auto known = std::make_tuple(normcaseAbsolute(path), size, modifiedNs);
    {
        std::lock_guard<std::mutex> lock(crcMutex);
        if (const auto found = crcs.find(known); found != crcs.end()) {
            return found->second;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    quint32 crc = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 20);
        crc = crc32(crc, chunk.constData(), chunk.size());
    }
    std::lock_guard<std::mutex> lock(crcMutex);
    crcs[known] = crc;
    crcsAdded = true;
    return crc;
}

void rememberCrcs(const QJsonValue& saved) {
    if (!saved.isArray()) {
        return;
    }
    std::lock_guard<std::mutex> lock(crcMutex);
    for (const QJsonValue& value : saved.toArray()) {
        const QJsonArray entry = value.toArray();
        if (entry.size() != 4 || !entry.at(0).isString()) {
            continue;
        }
        bool integers = true;
        for (int i = 1; i < 4; ++i) {
            integers = integers && entry.at(i).isDouble() && entry.at(i).toDouble() == std::floor(entry.at(i).toDouble());
        }
        if (integers) {
            crcs.emplace(std::make_tuple(entry.at(0).toString(), entry.at(1).toInteger(), entry.at(2).toInteger()),
                         static_cast<quint32>(entry.at(3).toInteger()));
        }
    }
}

// the CRCs for the cache file: only the files still there and unchanged
QJsonArray currentCrcs() {
    QJsonArray current;
    std::lock_guard<std::mutex> lock(crcMutex);
    for (const auto& [known, crc] : crcs) {
        const auto& [path, size, modified] = known;
        qint64 nowSize = 0, nowModified = 0;
        if (statFile(path, nowSize, nowModified) && nowSize == size && nowModified == modified) {
            current.append(QJsonArray{path, size, modified, static_cast<qint64>(crc)});
        }
    }
    return current;
}

// two JSON values the same as Python compares them (8 and 8.0 are equal)
bool sameJson(const QJsonValue& a, const QJsonValue& b) {
    if (a.isDouble() && b.isDouble()) {
        return a.toDouble() == b.toDouble();
    }
    if (a.isArray() && b.isArray()) {
        const QJsonArray x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        for (qsizetype i = 0; i < x.size(); ++i) {
            if (!sameJson(x.at(i), y.at(i))) {
                return false;
            }
        }
        return true;
    }
    if (a.isObject() && b.isObject()) {
        const QJsonObject x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys()) {
            return false;
        }
        for (auto it = x.begin(); it != x.end(); ++it) {
            if (!sameJson(it.value(), y.value(it.key()))) {
                return false;
            }
        }
        return true;
    }
    return a == b;
}

}  // namespace

QJsonObject cacheVersion() {
    return {{"analysis", kAnalysisVersion},
            {"sampleRate", kSampleRate},
            {"fft", kNfft},
            {"hop", kHop},
            {"window", kWindowSeconds},
            {"step", kStepSeconds},
            {"classSteps", kClassSteps},
            {"classOctaves", QJsonArray{kClassOctaves[0], kClassOctaves[1], kClassOctaves[2], kClassOctaves[3]}},
            {"classOctaveWeights", QJsonArray{kClassOctaveWeights[0], kClassOctaveWeights[1], kClassOctaveWeights[2],
                                              kClassOctaveWeights[3]}},
            {"medianWindows", kMedianWindows},
            {"beatTightness", kBeatTightness},
            {"silenceStep", kSilenceStepSeconds},
            {"silenceDb", kSilenceDb},
            {"silenceBelowLoud", kSilenceBelowLoud},
            {"silenceMin", kSilenceMinSeconds}};
}

Cache loadCache() {
    Cache cache{cacheVersion(), {}};
    QFile file(kCacheFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return cache;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return cache;
    }
    const QJsonObject data = document.object();
    // (the files' CRCs are kept whatever version the analyses were made with: they're just the files')
    rememberCrcs(data.value(QStringLiteral("crcs")));
    if (sameJson(data.value(QStringLiteral("version")), QJsonValue(cacheVersion())) &&
        data.value(QStringLiteral("songs")).isObject()) {
        const QJsonObject songs = data.value(QStringLiteral("songs")).toObject();
        for (auto it = songs.begin(); it != songs.end(); ++it) {
            cache.songs.insert(it.key(), Analysis::fromJson(it.value().toObject()));
        }
    }
    return cache;
}

void saveCache(const Cache& cache) {
    QJsonObject songs;
    for (auto it = cache.songs.cbegin(); it != cache.songs.cend(); ++it) {
        songs.insert(it.key(), it.value().toJson());
    }
    const QJsonObject data{{"version", cache.version}, {"songs", songs}, {"crcs", currentCrcs()}};
    QSaveFile file(kCacheFile);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(data).toJson(QJsonDocument::Compact));
        file.commit();
    }
    std::lock_guard<std::mutex> lock(crcMutex);
    crcsAdded = false;
}

void saveNewCrcs(const Cache& cache) {
    bool added;
    {
        std::lock_guard<std::mutex> lock(crcMutex);
        added = crcsAdded;
    }
    if (added) {
        saveCache(cache);
    }
}

QString cacheKey(const QString& path) {
    qint64 size = 0, modified = 0;
    if (!statFile(path, size, modified)) {
        throw std::runtime_error("FileNotFoundError");
    }
    const QString name = QFileInfo(QDir::fromNativeSeparators(path)).fileName().toLower();
    return QStringLiteral("%1|%2|%3").arg(name).arg(size).arg(fileCrc(path, size, modified), 8, 16, QLatin1Char('0'));
}

std::optional<Analysis> cached(const Cache& cache, const QString& path) {
    try {
        const auto found = cache.songs.find(cacheKey(path));
        if (found != cache.songs.end()) {
            return *found;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

QList<CrcEntry> rememberedCrcs() {
    QList<CrcEntry> entries;
    std::lock_guard<std::mutex> lock(crcMutex);
    for (const auto& [known, crc] : crcs) {
        entries.append({std::get<0>(known), std::get<1>(known), std::get<2>(known), crc});
    }
    return entries;
}

void forgetCrcs() {
    std::lock_guard<std::mutex> lock(crcMutex);
    crcs.clear();
    crcsAdded = false;
}

QHash<QString, Analysis> analyzeSongs(const QStringList& paths, const std::function<void(int, int)>& progress,
                                      const std::function<bool()>& cancelled, std::optional<int> workers) {
    Cache cache = loadCache();
    QHash<QString, Analysis> results;
    QStringList todo;
    QStringList unique;
    for (const QString& path : paths) {
        if (!unique.contains(path)) {
            unique.append(path);
        }
    }
    for (const QString& path : unique) {
        if (const std::optional<Analysis> hit = cached(cache, path)) {
            results.insert(path, *hit);
        } else {
            todo.append(path);
        }
    }
    const int total = static_cast<int>(results.size() + todo.size());
    int done = static_cast<int>(results.size());
    if (progress) {
        progress(done, total);
    }
    if (todo.isEmpty()) {
        return results;
    }
    const int threads = workers.value_or(std::max(1, std::min(static_cast<int>(todo.size()),
                                                              static_cast<int>(std::thread::hardware_concurrency()) - 1)));
    // the songs handed to the workers one at a time; each result back to this thread
    std::mutex mutex;
    std::condition_variable finished;
    std::deque<QString> queue(todo.begin(), todo.end());
    std::deque<std::pair<QString, Analysis>> ready;
    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&] {
            while (true) {
                QString path;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop || queue.empty()) {
                        return;
                    }
                    path = queue.front();
                    queue.pop_front();
                }
                Analysis analysis = analyzeFile(path);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ready.push_back({path, std::move(analysis)});
                }
                finished.notify_one();
            }
        });
    }
    int collected = 0;
    while (collected < todo.size()) {
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [&] { return !ready.empty(); });
        auto [path, analysis] = std::move(ready.front());
        ready.pop_front();
        lock.unlock();
        ++collected;
        if (!analysis.error) {
            try {
                cache.songs.insert(cacheKey(path), analysis);
            } catch (const std::exception&) {
            }
        }
        results.insert(path, std::move(analysis));
        ++done;
        if (progress) {
            progress(done, total);
        }
        if (cancelled && cancelled()) {
            std::lock_guard<std::mutex> stopLock(mutex);
            stop = true;
            queue.clear();
            // (the ones already being analyzed finish, unwaited for below)
            break;
        }
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    saveCache(cache);
    return results;
}

QList<SelectedSong> loadSelection() {
    QList<SelectedSong> songs;
    QFile file(kSelectionFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return songs;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    for (const QJsonValue& value : document.array()) {
        const QJsonArray entry = value.toArray();
        if (value.isArray() && entry.size() == 3) {
            songs.append({entry.at(0).toString(), entry.at(1).toString(), entry.at(2).toBool()});
        }
    }
    return songs;
}

void saveSelection(const QList<SelectedSong>& songs) {
    QJsonArray data;
    for (const SelectedSong& song : songs) {
        data.append(QJsonArray{song.path, song.title, song.checked});
    }
    QSaveFile file(kSelectionFile);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(data).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

}  // namespace awo::music
