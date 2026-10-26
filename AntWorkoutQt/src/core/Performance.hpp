#pragma once

// The performance settings from the title bar's menu (Performance.py), saved in savedSettingsANT.json:
//   "gaugeFps": 60   how many times a second the gauge needles move while easing toward a new reading; 0 turns the
//                    easing off (the needle jumps to each reading, about 4 a second)
// Lower is lighter on the CPU. Gauges connect to changed(), so a new setting applies to the ones on screen too.

#include <QList>
#include <QObject>
#include <QString>

namespace awo {

class Performance : public QObject {
    Q_OBJECT

public:
    // the slider's stops, lightest first
    static const QList<int> kGaugeFpsChoices;
    static constexpr int kDefaultGaugeFps = 60;

    static Performance& current();

    // (read from the saved settings the first time it's asked for)
    int gaugeFps();
    void setGaugeFps(int fps);
    static QString gaugeFpsText(int fps);
    // forget the setting read, so it's read again (tests)
    void reset() { gaugeFps_ = -1; }

signals:
    void changed();

private:
    int gaugeFps_ = -1;
};

}  // namespace awo
