#include "ZwoConvert.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

#include <cmath>
#include <optional>
#include <tuple>

#include "AwoParser.hpp"
#include "PyCompat.hpp"

namespace awo {

namespace {

constexpr double kMinMultiplier = kMinFtpMultiplier;
constexpr double kMaxMultiplier = kMaxFtpMultiplier;
// how long a text event shows (Zwift's own is about this), unless it says otherwise
constexpr int kMessageSeconds = 10;
// a block's Cadence at or above / below these is seated high / low cadence
constexpr double kHighCadenceRpm = 100;
constexpr double kLowCadenceRpm = 70;

// What can't be converted, thrown from wherever it's found and reported as the conversion's error
struct Failure {
    QString message;
};

// The parsed file, as much of it as the conversion looks at
struct Element {
    QString tag;
    QList<std::pair<QString, QString>> attributes;  // in the file's order
    QString text;                                   // before its first child, as ElementTree's .text
    QList<Element> children;
};

std::optional<Element> parseXml(QXmlStreamReader& reader, QString* error) {
    QList<Element> open;
    std::optional<Element> root;
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::StartElement: {
            Element element;
            element.tag = reader.name().toString();
            for (const QXmlStreamAttribute& attribute : reader.attributes()) {
                element.attributes.append({attribute.name().toString(), attribute.value().toString()});
            }
            open.append(element);
            break;
        }
        case QXmlStreamReader::Characters:
            if (!open.isEmpty() && open.last().children.isEmpty()) {
                open.last().text += reader.text();
            }
            break;
        case QXmlStreamReader::EndElement: {
            Element element = open.takeLast();
            if (open.isEmpty()) {
                root = element;
            } else {
                open.last().children.append(element);
            }
            break;
        }
        default:
            break;
        }
    }
    if (reader.hasError() || !root) {
        *error = QStringLiteral("%1: line %2, column %3")
                     .arg(reader.hasError() ? reader.errorString() : QStringLiteral("no element found"))
                     .arg(reader.lineNumber())
                     .arg(reader.columnNumber());
        return std::nullopt;
    }
    return root;
}

// The text without closing tags that don't close anything (hand edited files have them, and Zwift doesn't mind),
// and the line of each one dropped
std::pair<QString, QList<int>> dropStrayClosingTags(QString text) {
    static const QRegularExpression tag(QStringLiteral(R"(<(/?)([A-Za-z_][\w.-]*)[^>]*?(/?)>)"),
                                        QRegularExpression::UseUnicodePropertiesOption);
    QStringList open;
    QList<QRegularExpressionMatch> drop;
    QRegularExpressionMatchIterator it = tag.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const bool closing = !match.captured(1).isEmpty();
        const QString name = match.captured(2);
        if (!match.captured(3).isEmpty()) {
            continue;
        }
        if (!closing) {
            open.append(name);
        } else if (!open.isEmpty() && open.last() == name) {
            open.removeLast();
        } else if (open.contains(name)) {
            // closes an outer one: what it skips over are left open, close them here too
            while (open.takeLast() != name) {
            }
        } else {
            drop.append(match);
        }
    }
    for (qsizetype i = drop.size() - 1; i >= 0; --i) {
        text.remove(drop[i].capturedStart(), drop[i].capturedLength());
    }
    // (counted in the text as it is now, at each tag's place in the original, as the Python app did)
    QList<int> lines;
    for (const QRegularExpressionMatch& match : drop) {
        lines.append(static_cast<int>(text.left(match.capturedStart()).count(u'\n')) + 1);
    }
    return {text, lines};
}

// The first of the attributes (any capitalization) the element has, as a number; empty if none
std::optional<double> number(const Element& element, std::initializer_list<const char*> names) {
    QHash<QString, QString> attributes;
    for (const auto& [key, value] : element.attributes) {
        attributes.insert(key.toLower(), value);
    }
    for (const char* name : names) {
        const auto found = attributes.constFind(QString::fromLatin1(name).toLower());
        if (found == attributes.constEnd()) {
            continue;
        }
        const std::optional<double> value = py::toFloat(*found);
        // (not a finite one either: the Python app took inf or nan here and then failed working with it)
        if (!value || !std::isfinite(*value)) {
            throw Failure{QStringLiteral("<%1> %2=\"%3\" isn't a number").arg(element.tag, QLatin1String(name), *found)};
        }
        return value;
    }
    return std::nullopt;
}

int whole(double value) {
    return static_cast<int>(py::round(value));
}

QString cleanMessage(QString text) {
    return text.replace(u'|', u'/').replace(u';', u',').simplified();
}

// (block second, text, seconds shown)
using TimedMessage = std::tuple<int, QString, int>;

class Writer {
public:
    explicit Writer(std::optional<double> ftpOverride) : ftpOverride_(ftpOverride) {}

    QStringList lines;
    QStringList warnings;
    bool clamped = false;

    // A Zwift power (x FTP) as an .awo power: a multiplier, or watts with a ftpOverride
    QString power(double multiplier) {
        if (ftpOverride_) {
            int watts = whole(multiplier * *ftpOverride_);
            if (watts < kMinExplicitWatts || watts > kMaxExplicitWatts) {
                clamped = true;
                watts = std::min(std::max(watts, kMinExplicitWatts), kMaxExplicitWatts);
            }
            return QStringLiteral("%1W").arg(watts);
        }
        if (!(multiplier >= kMinMultiplier && multiplier <= kMaxMultiplier)) {
            clamped = true;
            multiplier = std::min(std::max(multiplier, kMinMultiplier), kMaxMultiplier);
        }
        return py::formatG(py::round(multiplier, 3));
    }

    // |message|seconds|start fields for the messages inside [start, start + duration)
    static QString messages(const QList<TimedMessage>& messages, double start, int duration) {
        QString fields;
        for (const auto& [second, text, seconds] : messages) {
            if (start <= second && second < start + duration) {
                const int offset = static_cast<int>(second - start);
                fields += QStringLiteral("|%1|%2|%3").arg(text).arg(std::max(1, std::min(seconds, duration - offset)))
                              .arg(offset);
            }
        }
        return fields;
    }

    // A duration split into whole-second pieces of at most kMaxBlockSeconds
    static QList<int> pieces(int duration) {
        const int count = (duration + kMaxBlockSeconds - 1) / kMaxBlockSeconds;
        QList<int> result;
        for (int i = 0; i < count; ++i) {
            result.append(duration / count + (i < duration % count ? 1 : 0));
        }
        return result;
    }

    // A free ride (kind 0), steady (1) or ramp (2) block, split into pieces if it's too long; powers are x FTP
    void block(int kind, int duration, QList<double> powers = {}, int position = 0,
               const QList<TimedMessage>& blockMessages = {}) {
        if (kind == 2 && power(powers[0]) == power(powers[1])) {
            kind = 1;
            powers = {powers[0]};
        }
        int start = 0;
        for (int piece : pieces(duration)) {
            QString text;
            if (kind == 2) {
                const double low = powers[0];
                const double high = powers[1];
                const double from = low + (high - low) * start / duration;
                const double to = low + (high - low) * (start + piece) / duration;
                text = QStringLiteral("2:%1,%2,%3").arg(piece).arg(power(from), power(to));
                if (power(from) == power(to)) {
                    text = QStringLiteral("1:%1,%2").arg(piece).arg(power(from));
                }
            } else if (kind == 1) {
                text = QStringLiteral("1:%1,%2").arg(piece).arg(power(powers[0]));
            } else {
                text = QStringLiteral("0:%1").arg(piece);
            }
            if (position) {
                text += QStringLiteral(",%1").arg(position);
            }
            lines.append(text + messages(blockMessages, start, piece));
            start += piece;
        }
    }

    // One block of an interval line (no splitting: an interval's blocks are each at most kMaxBlockSeconds)
    QString subBlock(int duration, double powerValue, int position, const QList<TimedMessage>& blockMessages) {
        QString text = QStringLiteral("1:%1,%2").arg(duration).arg(power(powerValue));
        if (position) {
            text += QStringLiteral(",%1").arg(position);
        }
        return text + messages(blockMessages, 0, duration);
    }

private:
    std::optional<double> ftpOverride_;
};

int duration(const Element& element, const char* name = "Duration") {
    const std::optional<double> value = number(element, {name});
    return value ? whole(*value) : 0;
}

// A block's Cadence: the position for a fast or slow one, and an "<rpm> RPM" message
std::pair<int, QString> cadence(const Element& element, const char* name = "Cadence") {
    const std::optional<double> rpm = number(element, {name});
    if (!rpm || *rpm <= 0) {
        return {0, QString()};
    }
    const int position = *rpm >= kHighCadenceRpm ? 1 : *rpm < kLowCadenceRpm ? 2 : 0;
    return {position, QStringLiteral("%1 RPM").arg(whole(*rpm))};
}

// The block's <textevent>s
QList<TimedMessage> textEvents(const Element& element, int blockDuration) {
    QList<TimedMessage> events;
    for (const Element& event : element.children) {
        if (event.tag.toLower() != u"textevent") {
            continue;
        }
        QString raw;
        for (const auto& [key, value] : event.attributes) {
            if (key == u"message") {
                raw = value;
            }
        }
        const QString text = cleanMessage(raw);
        if (text.isEmpty()) {
            continue;
        }
        // (a 0 offset or duration counts as none given, as the Python app read them)
        const std::optional<double> offsetValue = number(event, {"timeoffset"});
        const std::optional<double> secondsValue = number(event, {"duration"});
        const int offset = whole(offsetValue && *offsetValue != 0 ? *offsetValue : 0);
        const int seconds = whole(secondsValue && *secondsValue != 0 ? *secondsValue : kMessageSeconds);
        if (offset >= 0 && offset < blockDuration) {
            events.append({offset, text, std::max(1, seconds)});
        }
    }
    return events;
}

QList<TimedMessage> shifted(const QList<TimedMessage>& events, int by) {
    QList<TimedMessage> result;
    for (const auto& [second, text, seconds] : events) {
        result.append({second - by, text, seconds});
    }
    return result;
}

ZwoConversion convert(const Element& root, const QStringList& repairs) {
    if (root.tag.toLower() != u"workout_file") {
        throw Failure{QStringLiteral("Not a Zwift workout file (no <workout_file>)")};
    }
    QHash<QString, const Element*> children;
    for (const Element& child : root.children) {
        children.insert(child.tag.toLower(), &child);
    }
    const Element* sportElement = children.value(QStringLiteral("sporttype"));
    const QString sport = sportElement ? sportElement->text.trimmed().toLower() : QString();
    const Element* workout = children.value(QStringLiteral("workout"));
    if (!workout) {
        throw Failure{QStringLiteral("No <workout> in the file")};
    }
    std::optional<double> ftpOverride;
    if (const Element* overrideElement = children.value(QStringLiteral("ftpoverride"))) {
        ftpOverride = py::toFloat(overrideElement->text);
        if (!ftpOverride || !std::isfinite(*ftpOverride)) {
            throw Failure{QStringLiteral("<ftpOverride> isn't a number")};
        }
        if (*ftpOverride <= 0) {
            ftpOverride.reset();
        }
    }
    const Element* nameElement = children.value(QStringLiteral("name"));
    const QString title = nameElement ? nameElement->text.simplified() : QString();

    Writer writer(ftpOverride);
    writer.warnings += repairs;
    if (!sport.isEmpty() && sport != u"bike") {
        writer.warnings.append(QStringLiteral("Written for %1, not bike").arg(sport));
    }
    if (ftpOverride) {
        writer.warnings.append(QStringLiteral("Powers are watts, for an FTP of %1 (its <ftpOverride>)")
                                   .arg(py::formatG(*ftpOverride)));
    }
    if (!title.isEmpty()) {
        writer.lines.append(QStringLiteral("T:") + title);
    }
    for (const Element& element : workout->children) {
        const QString tag = element.tag.toLower();
        if (tag == u"textevent") {
            continue;
        }
        const int blockDuration = duration(element);
        const auto [position, cadenceMessage] = cadence(element);
        const QList<TimedMessage> cadenceMessages =
            cadenceMessage.isEmpty() ? QList<TimedMessage>{} : QList<TimedMessage>{{0, cadenceMessage, blockDuration}};
        if (tag == u"freeride" || tag == u"maxeffort") {
            if (blockDuration <= 0) {
                continue;
            }
            QList<TimedMessage> messages = textEvents(element, blockDuration);
            if (tag == u"maxeffort") {
                messages.prepend({0, QStringLiteral("Max effort!"), std::min(kMessageSeconds, blockDuration)});
            }
            messages = cadenceMessages + messages;
            writer.block(0, blockDuration, {}, position, messages);
        } else if (tag == u"steadystate") {
            if (blockDuration <= 0) {
                continue;
            }
            const std::optional<double> low = number(element, {"PowerLow"});
            const std::optional<double> high = number(element, {"PowerHigh"});
            const std::optional<double> power = number(element, {"Power"});
            const QList<TimedMessage> messages = cadenceMessages + textEvents(element, blockDuration);
            if (!power && low && high) {
                writer.block(2, blockDuration, {*low, *high}, position, messages);
            } else if (power) {
                writer.block(1, blockDuration, {*power}, position, messages);
            } else {
                throw Failure{QStringLiteral("<SteadyState> without a Power")};
            }
        } else if (tag == u"warmup" || tag == u"cooldown" || tag == u"ramp") {
            if (blockDuration <= 0) {
                continue;
            }
            const std::optional<double> low = number(element, {"PowerLow"});
            const std::optional<double> high = number(element, {"PowerHigh"});
            if (!low || !high) {
                throw Failure{QStringLiteral("<%1> needs PowerLow and PowerHigh").arg(element.tag)};
            }
            writer.block(2, blockDuration, {*low, *high}, position, cadenceMessages + textEvents(element, blockDuration));
        } else if (tag == u"intervalst") {
            // (a 0 Repeat counts as none given, as the Python app read it)
            const std::optional<double> repeatValue = number(element, {"Repeat"});
            const int repeat = whole(repeatValue && *repeatValue != 0 ? *repeatValue : 1);
            const int onDuration = duration(element, "OnDuration");
            const int offDuration = duration(element, "OffDuration");
            const std::optional<double> onPower = number(element, {"OnPower", "PowerOnHigh", "PowerOnLow"});
            const std::optional<double> offPower = number(element, {"OffPower", "PowerOffLow", "PowerOffHigh"});
            if (!onPower || !offPower) {
                throw Failure{QStringLiteral("<IntervalsT> needs OnPower and OffPower")};
            }
            if (repeat <= 0 || onDuration <= 0) {
                continue;
            }
            const auto [onPosition, onCadence] = cadence(element, "Cadence");
            const auto [offPosition, offCadence] = cadence(element, "CadenceResting");
            const QList<TimedMessage> events = textEvents(element, repeat * (onDuration + offDuration));
            QList<TimedMessage> onMessages;
            QList<TimedMessage> offMessages;
            if (!onCadence.isEmpty()) {
                onMessages.append({0, onCadence, onDuration});
            }
            if (!offCadence.isEmpty()) {
                offMessages.append({0, offCadence, offDuration});
            }
            bool allInFirstRep = true;
            for (const auto& [second, text, seconds] : events) {
                allInFirstRep = allInFirstRep && second < onDuration + offDuration;
            }
            if (offDuration <= 0) {
                // no rest: just the efforts back to back
                writer.block(1, repeat * onDuration, {*onPower}, onPosition, onMessages + events);
            } else if (!events.isEmpty() && allInFirstRep) {
                // messages in the first rep stay on its blocks (shown again each rep): the interval stays one line
                for (const auto& [second, text, seconds] : events) {
                    if (second < onDuration) {
                        onMessages.append({second, text, seconds});
                    } else {
                        offMessages.append({second - onDuration, text, seconds});
                    }
                }
                writer.lines.append(QStringLiteral("3:%1;").arg(repeat) +
                                    writer.subBlock(onDuration, *onPower, onPosition, onMessages) + u';' +
                                    writer.subBlock(offDuration, *offPower, offPosition, offMessages));
            } else if (!events.isEmpty()) {
                // messages later on show once, where they are in Zwift: the interval rep by rep
                for (int rep = 0; rep < repeat; ++rep) {
                    const int repStart = rep * (onDuration + offDuration);
                    writer.block(1, onDuration, {*onPower}, onPosition, onMessages + shifted(events, repStart));
                    writer.block(1, offDuration, {*offPower}, offPosition,
                                 offMessages + shifted(events, repStart + onDuration));
                }
            } else if (onDuration > kMaxBlockSeconds || offDuration > kMaxBlockSeconds) {
                for (int rep = 0; rep < repeat; ++rep) {
                    writer.block(1, onDuration, {*onPower}, onPosition, onMessages);
                    writer.block(1, offDuration, {*offPower}, offPosition, offMessages);
                }
            } else {
                writer.lines.append(QStringLiteral("3:%1;").arg(repeat) +
                                    writer.subBlock(onDuration, *onPower, onPosition, onMessages) + u';' +
                                    writer.subBlock(offDuration, *offPower, offPosition, offMessages));
            }
        } else if (blockDuration > 0) {
            writer.warnings.append(QStringLiteral("<%1> isn't supported, ridden as a free ride").arg(element.tag));
            writer.block(0, blockDuration, {}, 0, {{0, element.tag, std::min(kMessageSeconds, blockDuration)}});
        } else {
            writer.warnings.append(QStringLiteral("<%1> isn't supported, left out").arg(element.tag));
        }
    }
    if (writer.clamped) {
        writer.warnings.append(QStringLiteral("Some powers were outside what a workout file allows, they're limited to it"));
    }
    bool hasBlock = false;
    for (const QString& line : writer.lines) {
        hasBlock = hasBlock || (!line.isEmpty() && QStringLiteral("0123").contains(line[0]));
    }
    if (!hasBlock) {
        throw Failure{QStringLiteral("The workout has no blocks")};
    }
    return {writer.lines, title, writer.warnings, {}};
}

}  // namespace

ZwoConversion convertZwoData(const QByteArray& data) {
    try {
        QString error;
        QXmlStreamReader reader(data);
        std::optional<Element> root = parseXml(reader, &error);
        QStringList repairs;
        if (!root) {
            const auto [text, lines] = dropStrayClosingTags(QString::fromUtf8(data));
            if (lines.isEmpty()) {
                throw Failure{QStringLiteral("Not a readable workout file (%1)").arg(error)};
            }
            QString retryError;
            QXmlStreamReader retry(text);
            root = parseXml(retry, &retryError);
            if (!root) {
                throw Failure{QStringLiteral("Not a readable workout file (%1)").arg(error)};
            }
            for (int line : lines) {
                repairs.append(QStringLiteral("Left out a closing tag that doesn't close anything (line %1)").arg(line));
            }
        }
        return convert(*root, repairs);
    } catch (const Failure& failure) {
        ZwoConversion conversion;
        conversion.error = failure.message;
        return conversion;
    }
}

ZwoConversion convertZwo(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ZwoConversion conversion;
        conversion.error = QStringLiteral("Can't open the file (%1)").arg(file.errorString());
        return conversion;
    }
    return convertZwoData(file.readAll());
}

QString awoPathFor(const QString& zwoPath) {
    const QFileInfo info(zwoPath);
    const QString suffix = info.suffix();
    return suffix.isEmpty() ? zwoPath + QStringLiteral(".awo") : zwoPath.chopped(suffix.size()) + QStringLiteral("awo");
}

ZwoConversion writeAwo(const QString& zwoPath, const QString& awoPath, QString* writtenPath) {
    ZwoConversion conversion = convertZwo(zwoPath);
    if (!conversion.ok()) {
        return conversion;
    }
    const QString target = awoPath.isEmpty() ? awoPathFor(zwoPath) : awoPath;
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        conversion.error = QStringLiteral("Can't write %1 (%2)").arg(target, file.errorString());
        return conversion;
    }
    file.write((conversion.lines.join(u'\n') + u'\n').toUtf8());
    if (!file.commit()) {
        conversion.error = QStringLiteral("Can't write %1 (%2)").arg(target, file.errorString());
        return conversion;
    }
    if (writtenPath) {
        *writtenPath = target;
    }
    return conversion;
}

}  // namespace awo
