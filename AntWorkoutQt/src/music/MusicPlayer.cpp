#include "MusicPlayer.hpp"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "miniaudio.h"

namespace awo::music {

namespace {

void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<MusicPlayer*>(device->pUserData)->fill(static_cast<qint16*>(output), frames);
}

}  // namespace

MusicPlayer::MusicPlayer(const QList<MusicTrack>& tracks) : tracks_(tracks) {
    // the first song's file starts being read right away, before the sound device asks for anything
    if (!tracks_.isEmpty()) {
        readAhead(0);
    }
}

MusicPlayer::~MusicPlayer() {
    stop();
    if (decoder_) {
        ma_decoder_uninit(decoder_.get());
    }
}

void MusicPlayer::readAhead(int index) {
    nextReadIndex_ = index;
    const QString path = tracks_[index].path;
    nextRead_ = std::async(std::launch::async, [path] {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return std::make_pair(QByteArray(), QStringLiteral("FileNotFoundError: %1").arg(file.errorString()));
        }
        return std::make_pair(file.readAll(), QString());
    });
}

void MusicPlayer::start() {
    device_ = std::make_unique<ma_device>();
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.sampleRate = kSampleRate;
    config.playback.channels = kChannels;
    config.playback.format = ma_format_s16;
    config.periodSizeInMilliseconds = 100;
    config.dataCallback = dataCallback;
    config.pUserData = this;
    if (ma_device_init(nullptr, &config, device_.get()) != MA_SUCCESS) {
        device_.reset();
        throw std::runtime_error("no sound device");
    }
    if (ma_device_start(device_.get()) != MA_SUCCESS) {
        ma_device_uninit(device_.get());
        device_.reset();
        throw std::runtime_error("the sound device didn't start");
    }
}

void MusicPlayer::stop() {
    if (device_) {
        ma_device_uninit(device_.get());
        device_.reset();
    }
}

double MusicPlayer::latency() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!firstCallback_) {
        return 0.0;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - *firstCallback_).count();
    return std::min(std::max(0.0, static_cast<double>(framesHanded_) / kSampleRate - elapsed), 1.0);
}

double MusicPlayer::position() const {
    double handed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_) {
            double total = 0;
            for (const MusicTrack& track : tracks_) {
                total += track.duration;
            }
            return total + 1;
        }
        if (track_ >= tracks_.size()) {
            return 0.0;
        }
        const MusicTrack& track = tracks_[track_];
        // (no further than the song's analyzed length, so a slightly long file can't run into the next song's time;
        // less than the song's start while the end of the one before is still being heard)
        handed = track.start + std::min(static_cast<double>(trackFrames_) / kSampleRate, track.duration);
    }
    // (silence keeps the device's buffer just as full while paused, so this holds still then)
    return std::max(0.0, handed - latency());
}

QList<std::pair<QString, QString>> MusicPlayer::skipped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return skipped_;
}

bool MusicPlayer::openSong(int index) {
    for (; index < tracks_.size(); ++index) {
        if (nextReadIndex_ != index) {
            readAhead(index);
        }
        auto [bytes, error] = nextRead_.get();
        nextReadIndex_ = -1;
        if (index + 1 < tracks_.size()) {
            readAhead(index + 1);
        }
        if (decoder_) {
            ma_decoder_uninit(decoder_.get());
            decoder_.reset();
        }
        if (error.isEmpty()) {
            data_ = std::move(bytes);
            decoder_ = std::make_unique<ma_decoder>();
            ma_decoder_config config = ma_decoder_config_init(ma_format_s16, kChannels, kSampleRate);
            config.ditherMode = ma_dither_mode_none;
            const ma_result result = ma_decoder_init_memory(data_.constData(), static_cast<size_t>(data_.size()), &config,
                                                            decoder_.get());
            if (result == MA_SUCCESS) {
                std::lock_guard<std::mutex> lock(mutex_);
                track_ = index;
                trackFrames_ = 0;
                return true;
            }
            decoder_.reset();
            error = QStringLiteral("DecodeError: ('failed to init decoder', %1)").arg(static_cast<int>(result));
        }
        // unreadable: on to the next song
        std::lock_guard<std::mutex> lock(mutex_);
        skipped_.append({tracks_[index].path, error});
    }
    return false;
}

void MusicPlayer::fill(qint16* out, quint32 frames) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!firstCallback_) {
            firstCallback_ = Clock::now();
        }
    }
    framesHanded_ += frames;
    quint32 written = 0;
    while (!(paused || finished_) && written < frames) {
        if (!started_) {
            started_ = true;
            if (!openSong(0)) {
                finished_ = true;
                break;
            }
        }
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(decoder_.get(), out + written * kChannels, frames - written, &read);
        if (read == 0) {
            // the song's done: the next one (past any that can't be read)
            int next;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                next = track_ + 1;
            }
            if (!openSong(next)) {
                finished_ = true;
                break;
            }
            continue;
        }
        written += static_cast<quint32>(read);
        std::lock_guard<std::mutex> lock(mutex_);
        trackFrames_ += static_cast<qint64>(read);
    }
    std::memset(out + written * kChannels, 0, (frames - written) * kChannels * sizeof(qint16));
    const double level = std::clamp(volume.load(), 0.0, 1.0);
    if (level != 1.0) {
        // (numpy: int16 times the volume, back to int16 toward zero)
        for (quint32 i = 0; i < frames * kChannels; ++i) {
            out[i] = static_cast<qint16>(std::trunc(out[i] * level));
        }
    }
}

}  // namespace awo::music
