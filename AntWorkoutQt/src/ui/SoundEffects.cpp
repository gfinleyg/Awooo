#include "SoundEffects.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QMap>
#include <QMutex>
#include <QTimer>
#include <QToolButton>

#include <atomic>
#include <memory>

#include "Logger.hpp"
#include "miniaudio.h"

namespace awo::sound {

namespace {

struct SoundFile {
    const char* name;
    const char* file;
    float volume;
};

constexpr SoundFile kSounds[] = {
    {"click", "click.wav", 0.9f},
    {"window", "window_change.wav", 0.9f},
    {"complete", "workout_complete.wav", 0.9f},
};

QMutex mutex;
std::unique_ptr<ma_engine> engine;
QMap<QString, ma_sound*> sounds;
// window sounds asked for so far: lets a click tell if it opened a new screen
std::atomic<int> windowSounds{0};

// a click once the button's own clicked handlers are done, only if none of them changed screens (a window sound
// since the press), so it doesn't pile onto the window sound
void playClick(QAbstractButton* button) {
    const int atPress = button->property("awoWindowSoundsAtPress").toInt();
    QTimer::singleShot(0, button, [atPress] {
        if (windowSounds == atPress) {
            play(QStringLiteral("click"));
        }
    });
}

// every button in the app gets the click, with no wiring per window, through its own clicked (which covers keys
// too, and not a press dragged off the button before release)
class ButtonSoundFilter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Polish) {
            if (auto* button = qobject_cast<QAbstractButton*>(object); button && !button->property("awoClickSound").toBool()) {
                button->setProperty("awoClickSound", true);
                auto* tool = qobject_cast<QToolButton*>(button);
                if (tool && tool->popupMode() == QToolButton::InstantPopup) {
                    // (the settings gear opens its menu on press and never clicks: a menu, never a screen change)
                    connect(button, &QAbstractButton::pressed, [] { play(QStringLiteral("click")); });
                } else {
                    connect(button, &QAbstractButton::pressed, button,
                            [button] { button->setProperty("awoWindowSoundsAtPress", windowSounds.load()); });
                    connect(button, &QAbstractButton::clicked, button, [button] { playClick(button); });
                }
            }
        }
        return false;
    }
};

}  // namespace

void init(QApplication* app) {
    QMutexLocker locker(&mutex);
    if (engine) {
        return;
    }
    engine = std::make_unique<ma_engine>();
    if (ma_engine_init(nullptr, engine.get()) != MA_SUCCESS) {
        log::event(QStringLiteral("SOUND"), QStringLiteral("No sound device, sound effects disabled"));
        engine.reset();
    } else {
        const QString folder = QCoreApplication::applicationDirPath() + QStringLiteral("/sounds/");
        for (const SoundFile& sound : kSounds) {
            const QString path = folder + QLatin1String(sound.file);
            auto* loaded = new ma_sound;
            if (!QFile::exists(path) ||
                ma_sound_init_from_file_w(engine.get(), reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
                                          MA_SOUND_FLAG_DECODE, nullptr, nullptr, loaded) != MA_SUCCESS) {
                log::event(QStringLiteral("SOUND"), QStringLiteral("Missing sound file sounds/%1, '%2' sound disabled")
                                                        .arg(QLatin1String(sound.file), QLatin1String(sound.name)));
                delete loaded;
                continue;
            }
            ma_sound_set_volume(loaded, sound.volume);
            sounds.insert(QLatin1String(sound.name), loaded);
        }
    }
    app->installEventFilter(new ButtonSoundFilter(app));
    QObject::connect(app, &QCoreApplication::aboutToQuit, [] { shutdown(); });
}

void play(const QString& name) {
    if (name == u"window") {
        ++windowSounds;
    }
    QMutexLocker locker(&mutex);
    ma_sound* sound = sounds.value(name);
    if (!sound) {
        return;
    }
    ma_sound_seek_to_pcm_frame(sound, 0);
    ma_sound_start(sound);
}

void shutdown() {
    QMutexLocker locker(&mutex);
    for (ma_sound* sound : sounds) {
        ma_sound_uninit(sound);
        delete sound;
    }
    sounds.clear();
    if (engine) {
        ma_engine_uninit(engine.get());
        engine.reset();
    }
}

}  // namespace awo::sound
