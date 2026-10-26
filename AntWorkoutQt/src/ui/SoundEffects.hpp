#pragma once

// The UI's sound effects (SoundEffects.py): a click for every button, a sound on changing screens, and one when a
// workout is complete. Played by miniaudio on its own audio thread, so a busy workout screen can't starve them (the
// Python app moved its players to a thread of their own for the same reason). A missing file or no sound device is
// logged once and that sound is skipped: the app runs fine without them.

#include <QString>

class QApplication;

namespace awo::sound {

// Loads the sounds (sounds/ beside the app) and starts watching buttons app wide; once, after the QApplication
void init(QApplication* app);
// "click", "window" or "complete"; safe from any thread
void play(const QString& name);
void shutdown();

}  // namespace awo::sound
