#include "TimedApplication.hpp"

#include <QAbstractEventDispatcher>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QMetaEnum>
#include <QPaintEvent>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <typeinfo>
#include <utility>

namespace awo {

namespace {

// (typeid, not the meta-object: a widget without Q_OBJECT would show as the Qt class it's built on)
QString typeName(const QObject* object) {
    QString name = QString::fromLatin1(typeid(*object).name());
    name.remove(QStringLiteral("class "));
    name.remove(QStringLiteral("awo::"));
    return name;
}

QString describe(const QObject* receiver) {
    // a scroll area's viewport is a plain QWidget: what it's the viewport of says what paints it
    const QObject* parent = receiver->parent();
    if (receiver->objectName() == u"qt_scrollarea_viewport" && parent) {
        return typeName(parent) + QStringLiteral(" viewport");
    }
    // every timer is a QTimer: how often, and whose, say which one
    if (const auto* timer = qobject_cast<const QTimer*>(receiver)) {
        return QStringLiteral("QTimer %1 ms in %2")
            .arg(timer->interval())
            .arg(parent ? typeName(parent) : QStringLiteral("nothing"));
    }
    QString name = typeName(receiver);
    if (!receiver->objectName().isEmpty() && !receiver->objectName().startsWith(u"qt_")) {
        name += QStringLiteral(" (%1)").arg(receiver->objectName());
    } else if (name == u"QWidget" && parent) {
        // (as is a plain widget with no name)
        name += QStringLiteral(" in ") + typeName(parent);
    }
    return name;
}

QString eventName(const QEvent* event) {
    const char* name = QMetaEnum::fromType<QEvent::Type>().valueToKey(event->type());
    return name ? QString::fromLatin1(name) : QString::number(event->type());
}

qint64 paintedArea(const QEvent* event) {
    qint64 area = 0;
    if (event->type() == QEvent::Paint) {
        for (const QRect& rect : static_cast<const QPaintEvent*>(event)->region()) {
            area += qint64(rect.width()) * rect.height();
        }
    }
    return area;
}

}  // namespace

TimedApplication::TimedApplication(int& argc, char** argv) : QApplication(argc, argv) {
    clock_.start();
    if (auto* dispatcher = QAbstractEventDispatcher::instance()) {
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (timing_) {
                blockedAt_ = clock_.nsecsElapsed();
            }
        });
        connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
            if (blockedAt_ >= 0) {
                const qint64 waited = clock_.nsecsElapsed() - blockedAt_;
                idleNs_ += waited;
                innerNs_ += waited;
                blockedAt_ = -1;
            }
        });
    }
    const QString file = qEnvironmentVariable("AWO_EVENT_TIMING");
    if (!file.isEmpty()) {
        // (from where it was started: the app moves to its own folder)
        const QString path = QDir::current().absoluteFilePath(file);
        setTiming(true);
        connect(this, &QCoreApplication::aboutToQuit, this, [this, path] {
            QFile out(path);
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                out.write(report().toUtf8());
            }
        });
    }
}

bool TimedApplication::notify(QObject* receiver, QEvent* event) {
    // (only the UI thread's: other threads' objects' events come through here too)
    if (!timing_ || QThread::currentThread() != thread()) {
        return QApplication::notify(receiver, event);
    }
    const qint64 entered = clock_.nsecsElapsed();
    // (now: handling it can delete both)
    const QString what = describe(receiver) + u' ' + eventName(event);
    const qint64 area = paintedArea(event);
    const qint64 outerInner = std::exchange(innerNs_, 0);
    const qint64 start = clock_.nsecsElapsed();
    const bool handled = QApplication::notify(receiver, event);
    const qint64 self = std::max<qint64>(0, clock_.nsecsElapsed() - start - innerNs_);
    if (timing_) {
        Row& row = rows_[what];
        row.what = what;
        ++row.count;
        row.selfNs += self;
        row.worstNs = std::max(row.worstNs, self);
        row.overFrame += self > kFrameNs ? 1 : 0;
        row.area += area;
    }
    // (this bookkeeping isn't the outer event's own time either)
    innerNs_ = outerInner + clock_.nsecsElapsed() - entered;
    return handled;
}

void TimedApplication::setTiming(bool on) {
    if (on && !timing_) {
        resetTiming();
    }
    timing_ = on;
    blockedAt_ = -1;
}

void TimedApplication::resetTiming() {
    rows_.clear();
    startNs_ = clock_.nsecsElapsed();
    idleNs_ = 0;
}

QList<TimedApplication::Row> TimedApplication::rows() const {
    QList<Row> rows = rows_.values();
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.selfNs > b.selfNs; });
    return rows;
}

double TimedApplication::wallSeconds() const {
    return (clock_.nsecsElapsed() - startNs_) / 1e9;
}

double TimedApplication::busyShare() const {
    const double wall = wallSeconds();
    return wall > 0 ? std::clamp(1 - idleNs_ / 1e9 / wall, 0.0, 1.0) : 0.0;
}

QString TimedApplication::report(int rows) const {
    const QList<Row> all = this->rows();
    const double wall = wallSeconds();
    qint64 count = 0;
    qint64 overFrame = 0;
    for (const Row& row : all) {
        count += row.count;
        overFrame += row.overFrame;
    }
    QString text = QStringLiteral("%1 s timed, UI thread busy %2%, %3 events, %4 over a 60 fps frame (%5 ms)\n\n")
                       .arg(wall, 0, 'f', 1)
                       .arg(busyShare() * 100, 0, 'f', 1)
                       .arg(count)
                       .arg(overFrame)
                       .arg(kFrameNs / 1e6, 0, 'f', 1);
    text += QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8  %9\n")
                .arg(QStringLiteral("self ms"), 9)
                .arg(QStringLiteral("% wall"), 6)
                .arg(QStringLiteral("count"), 7)
                .arg(QStringLiteral("per s"), 6)
                .arg(QStringLiteral("avg ms"), 7)
                .arg(QStringLiteral("worst ms"), 8)
                .arg(QStringLiteral("> frame"), 7)
                .arg(QStringLiteral("avg px"), 8)
                .arg(QStringLiteral("what"));
    for (const Row& row : all.mid(0, rows)) {
        text += QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8  %9\n")
                    .arg(row.selfNs / 1e6, 9, 'f', 1)
                    .arg(wall > 0 ? row.selfNs / 1e7 / wall : 0.0, 6, 'f', 2)
                    .arg(row.count, 7)
                    .arg(wall > 0 ? row.count / wall : 0.0, 6, 'f', 1)
                    .arg(row.selfNs / 1e6 / row.count, 7, 'f', 2)
                    .arg(row.worstNs / 1e6, 8, 'f', 2)
                    .arg(row.overFrame, 7)
                    .arg(row.area / row.count, 8)
                    .arg(row.what);
    }
    if (all.size() > rows) {
        text += QStringLiteral("(and %1 more)\n").arg(all.size() - rows);
    }
    return text;
}

}  // namespace awo
