#pragma once

// The ride's songs played back to back on the default sound device, on miniaudio's own audio thread
// (MusicCadence.MusicPlayer). position() is where the music is in the ride (each song's start plus the frames of it
// played so far, less what's been handed to the device but not heard yet), the workout's clock, so the targets can't
// drift from the music. A song that can't be read is skipped: position() jumps to the next song's start with it. Each
// song's file is read on a thread of its own while the one before plays, so the device's callback never waits on it.

#include <QList>
#include <QString>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "AppState.hpp"

struct ma_device;
struct ma_decoder;

namespace awo::music {

class MusicPlayer {
public:
    static constexpr int kSampleRate = 44100;
    static constexpr int kChannels = 2;

    explicit MusicPlayer(const QList<MusicTrack>& tracks);
    ~MusicPlayer();

    // starts playing from the first song; throws std::runtime_error with no sound device
    void start();
    void stop();
    // seconds into the ride being heard now (past the end once the last song's done)
    double position() const;
    // seconds of audio handed to the device that haven't been heard yet
    double latency() const;

    std::atomic<bool> paused{false};
    // 0 to 1
    std::atomic<double> volume{1.0};
    bool finished() const { return finished_; }
    // (path, why) of the songs that couldn't be played
    QList<std::pair<QString, QString>> skipped() const;

    // (tests) the device's callback, filling out with frames' worth (silence while paused)
    void fill(qint16* out, quint32 frames);

private:
    using Clock = std::chrono::steady_clock;
    void readAhead(int index);
    // the next song that opens, from index on (false: none left)
    bool openSong(int index);

    QList<MusicTrack> tracks_;
    std::unique_ptr<ma_device> device_;
    std::unique_ptr<ma_decoder> decoder_;
    QByteArray data_;  // the open song's file, which its decoder reads from
    std::future<std::pair<QByteArray, QString>> nextRead_;
    int nextReadIndex_ = -1;
    bool started_ = false;  // a song asked for at least once

    mutable std::mutex mutex_;
    int track_ = 0;
    qint64 trackFrames_ = 0;
    std::atomic<bool> finished_{false};
    // every frame handed to the device (silence too) and when the first was
    std::atomic<qint64> framesHanded_{0};
    std::optional<Clock::time_point> firstCallback_;
    QList<std::pair<QString, QString>> skipped_;
};

}  // namespace awo::music
