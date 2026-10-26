#include "Performance.hpp"

#include <QJsonObject>

#include "SavedSettings.hpp"

namespace awo {

const QList<int> Performance::kGaugeFpsChoices = {0, 10, 15, 20, 30, 45, 60};

Performance& Performance::current() {
    static Performance performance;
    return performance;
}

int Performance::gaugeFps() {
    if (gaugeFps_ < 0) {
        const QJsonValue saved = settings::load().value(QStringLiteral("gaugeFps"));
        const int fps = saved.isDouble() && saved.toDouble() == saved.toInt() ? saved.toInt() : -1;
        gaugeFps_ = kGaugeFpsChoices.contains(fps) ? fps : kDefaultGaugeFps;
    }
    return gaugeFps_;
}

void Performance::setGaugeFps(int fps) {
    if (!kGaugeFpsChoices.contains(fps) || fps == gaugeFps()) {
        return;
    }
    gaugeFps_ = fps;
    settings::update({{QStringLiteral("gaugeFps"), fps}});
    emit changed();
}

QString Performance::gaugeFpsText(int fps) {
    if (fps == 0) {
        return QStringLiteral("Off - needles jump to each reading");
    }
    if (fps == kGaugeFpsChoices.last()) {
        return QStringLiteral("%1 fps - smoothest").arg(fps);
    }
    return QStringLiteral("%1 fps").arg(fps);
}

}  // namespace awo
