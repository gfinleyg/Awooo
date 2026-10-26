#include "AwoParser.hpp"

#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <array>
#include <limits>
#include <optional>

#include "PyCompat.hpp"
#include "ZwoConvert.hpp"

namespace awo {

namespace {

// A block's power: an FTP multiplier, or explicit watts (written with a W after them), which don't follow FTP
struct Power {
    double value = 0;
    bool explicitWatts = false;

    bool operator==(const Power&) const = default;
};

// Digits as a number, saturating rather than overflowing: a huge one fails its range check like any too-big one
// A block as read, and its ramp steps if it's a ramp
struct ReadBlock {
    Block block;
    QList<WorkoutEntry> steps;
};

long long digitsValue(const QString& digits) {
    bool ok = false;
    const long long value = digits.toLongLong(&ok);
    return ok ? value : std::numeric_limits<long long>::max() / 4;
}

class Parser {
public:
    explicit Parser(const AwoOptions& options) : options_(options) {
        for (int i = -10; i <= 10; ++i) {
            ftpIncrement_[static_cast<std::size_t>(i + 10)] = static_cast<int>((options_.ftp * i) * 0.02);
        }
    }

    AwoResult result;

    void parse(const QStringList& lines) {
        for (qsizetype index = 0; index < lines.size(); ++index) {
            if (!parseLine(lines[index], static_cast<int>(index) + 1) && !options_.allErrors) {
                return;
            }
        }
    }

private:
    const AwoOptions& options_;
    std::array<int, kDifficulties> ftpIncrement_{};
    // workout time at the start of the block being read, to place its messages
    int blockStart_ = 0;

    bool reject(const QString& reason, int lineNumber, const QString& text) {
        result.errors.append({reason, lineNumber, text});
        return false;
    }

    // the block text without its |message fields, and those fields
    static std::pair<QString, QStringList> splitMessageFields(const QString& text) {
        QStringList fields = text.split(u'|');
        const QString block = fields.takeFirst();
        return {block, fields};
    }

    bool parseLine(const QString& line, int lineNumber) {
        Workout& workout = result.workout;
        const auto [blockText, messageFields] = splitMessageFields(line);
        if (line.isEmpty()) {
            return true;
        }
        const QChar type = line[0];
        if (type == u'T') {
            if (!line.startsWith(QStringLiteral("T:")) || line.mid(2).trimmed().isEmpty()) {
                return reject(QStringLiteral("Title line must be T:<title>"), lineNumber, line);
            }
            if (!workout.title.isEmpty()) {
                return reject(QStringLiteral("Only one title (T:) line is allowed"), lineNumber, line);
            }
            workout.title = line.mid(2).trimmed();
            return true;
        }
        if (type == u'0' || type == u'1' || type == u'2') {
            const std::optional<ReadBlock> read = type == u'0'   ? freeRide(blockText, lineNumber)
                                                  : type == u'1' ? steady(blockText, lineNumber)
                                                                 : ramp(blockText, lineNumber);
            if (!read) {
                return false;
            }
            const std::optional<QList<Message>> messages = blockMessages(messageFields, read->block.seconds, line,
                                                                         lineNumber);
            if (!messages) {
                return false;
            }
            for (const Message& message : *messages) {
                workout.messages.append({blockStart_ + message.start, blockStart_ + message.end, message.text});
            }
            blockStart_ += read->block.seconds;
            // (a ramp's steps go after its block)
            workout.entries.append(WorkoutEntry::ofBlock(read->block));
            workout.entries.append(read->steps);
            return true;
        }
        if (type == u'3') {
            return interval(line, lineNumber);
        }
        return reject(QStringLiteral("Invalid block type: %1").arg(type), lineNumber, line);
    }

    // ---- The blocks ----

    // <freeride = 0>:<seconds>,<position>
    std::optional<ReadBlock> freeRide(const QString& text, int lineNumber, int repeats = 1) {
        const QStringList blockData = text.split(u':');
        if (blockData.size() != 2) {
            return fail(QStringLiteral("Need exactly one colon after block type"), lineNumber, text);
        }
        if (blockData[0] != u"0") {
            return fail(QStringLiteral("Block type must be 0 to 3"), lineNumber, text);
        }
        const QStringList values = blockData[1].split(u',');
        if (values.size() != 1 && values.size() != 2) {
            return fail(QStringLiteral("Free ride needs 1 or 2 values"), lineNumber, text);
        }
        Block block;
        block.type = BlockType::FreeRide;
        if (!duration(values[0], lineNumber, text, repeats, block.seconds)) {
            return std::nullopt;
        }
        if (values.size() == 2 && !position(values[1], lineNumber, text, block.position)) {
            return std::nullopt;
        }
        return ReadBlock{block, {}};
    }

    // <steady = 1>:<seconds>,<power>,<position>
    std::optional<ReadBlock> steady(const QString& text, int lineNumber, int repeats = 1) {
        const QStringList blockData = text.split(u':');
        if (blockData.size() != 2) {
            return fail(QStringLiteral("Need exactly one colon after block type"), lineNumber, text);
        }
        if (blockData[0] != u"1") {
            return fail(QStringLiteral("Block type must be 0 to 3"), lineNumber, text);
        }
        const QStringList values = blockData[1].split(u',');
        if (values.size() != 2 && values.size() != 3) {
            return fail(QStringLiteral("Steady state needs 2 or 3 values"), lineNumber, text);
        }
        Block block;
        block.type = BlockType::Steady;
        if (!duration(values[0], lineNumber, text, repeats, block.seconds)) {
            return std::nullopt;
        }
        const std::optional<Power> power = powerValue(values[1], lineNumber, text);
        if (!power) {
            return std::nullopt;
        }
        block.power = targets(*power);
        if (values.size() == 3 && !position(values[2], lineNumber, text, block.position)) {
            return std::nullopt;
        }
        return ReadBlock{block, {}};
    }

    // <ramp = 2>:<seconds>,<start power>,<end power>,<position>; one with the same start and end is the steady
    // block it amounts to
    std::optional<ReadBlock> ramp(const QString& text, int lineNumber, int repeats = 1) {
        const QStringList blockData = text.split(u':');
        if (blockData.size() != 2) {
            return fail(QStringLiteral("Need exactly one colon after block type"), lineNumber, text);
        }
        if (blockData[0] != u"2") {
            return fail(QStringLiteral("Block type must be 0 to 3"), lineNumber, text);
        }
        const QStringList values = blockData[1].split(u',');
        if (values.size() != 3 && values.size() != 4) {
            return fail(QStringLiteral("Ramp needs 3 or 4 values"), lineNumber, text);
        }
        Block block;
        if (!duration(values[0], lineNumber, text, repeats, block.seconds)) {
            return std::nullopt;
        }
        const std::optional<Power> start = powerValue(values[1], lineNumber, text);
        if (!start) {
            return std::nullopt;
        }
        const std::optional<Power> end = powerValue(values[2], lineNumber, text);
        if (!end) {
            return std::nullopt;
        }
        if (values.size() == 4 && !position(values[3], lineNumber, text, block.position)) {
            return std::nullopt;
        }
        if (*start == *end) {
            block.type = BlockType::Steady;
            block.power = targets(*start);
            return ReadBlock{block, {}};
        }
        block.type = BlockType::Ramp;
        block.power = targets(*start);
        block.powerEnd = targets(*end);
        return ReadBlock{block, rampSteps(block.seconds, *start, *end)};
    }

    // <interval = 3>:<repeats>;<block>;<block>;<block>, each block's messages shown on every repeat
    bool interval(const QString& line, int lineNumber) {
        Workout& workout = result.workout;
        QStringList blockData = line.split(u';');
        if (blockData.size() < 2 || blockData.size() > 4) {
            return reject(QStringLiteral("Interval needs 1 to 3 blocks after 3:<repeats>"), lineNumber, line);
        }
        if (blockData[0].contains(u'|')) {
            return reject(QStringLiteral("Put messages on interval blocks, not on 3:<repeats>"), lineNumber, line);
        }
        const QStringList first = blockData[0].split(u':');
        if (first.size() != 2) {
            return reject(QStringLiteral("Interval must start with 3:<repeats>"), lineNumber, line);
        }
        if (first[0] != u"3") {
            return reject(QStringLiteral("Block type must be 0 to 3"), lineNumber, line);
        }
        if (!py::isDigits(first[1]) || digitsValue(first[1]) < 1) {
            return reject(QStringLiteral("Interval repeats must be a whole number, 1+"), lineNumber, line);
        }
        if (digitsValue(first[1]) > kMaxIntervalRepeats) {
            return reject(QStringLiteral("Interval repeats can be at most %1").arg(kMaxIntervalRepeats), lineNumber,
                          line);
        }
        const int repeats = static_cast<int>(digitsValue(first[1]));
        blockData.removeFirst();

        QList<Block> blocks;
        QList<QList<WorkoutEntry>> steps;  // each block's ramp steps (none unless it's a ramp)
        QList<QList<Message>> messages;    // each block's, from its start
        for (const QString& raw : blockData) {
            const auto [text, messageFields] = splitMessageFields(raw);
            if (text.isEmpty()) {
                return reject(QStringLiteral("Empty interval block (extra semicolon?)"), lineNumber, line);
            }
            std::optional<ReadBlock> read;
            if (text[0] == u'0') {
                read = freeRide(text, lineNumber, repeats);
            } else if (text[0] == u'1') {
                read = steady(text, lineNumber, repeats);
            } else if (text[0] == u'2') {
                read = ramp(text, lineNumber, repeats);
            } else {
                return reject(QStringLiteral("Invalid block type in interval: %1").arg(text[0]), lineNumber, line);
            }
            if (!read) {
                return false;
            }
            blocks.append(read->block);
            steps.append(read->steps);
            const std::optional<QList<Message>> blockMessageList =
                blockMessages(messageFields, read->block.seconds, line, lineNumber);
            if (!blockMessageList) {
                return false;
            }
            messages.append(*blockMessageList);
        }

        if (blocks.size() == 1) {
            // one block repeated has to be one free ride or steady block, ridden as that block for the whole time
            if (blocks[0].type == BlockType::Ramp) {
                return reject(QStringLiteral("A one block interval can't be a ramp: write the ramps out, or add a "
                                             "second block"),
                              lineNumber, line);
            }
            if (static_cast<long long>(blocks[0].seconds) * repeats > options_.maxBlockSeconds) {
                return reject(QStringLiteral("A one block interval is one block, at most %1s in all")
                                  .arg(options_.maxBlockSeconds),
                              lineNumber, line);
            }
            Block whole = blocks[0];
            whole.seconds = blocks[0].seconds * repeats;
            workout.entries.append(WorkoutEntry::ofBlock(whole));
            for (int rep = 0; rep < repeats; ++rep) {
                for (const Message& message : messages[0]) {
                    workout.messages.append({blockStart_ + message.start, blockStart_ + message.end, message.text});
                }
                blockStart_ += blocks[0].seconds;
            }
            return true;
        }

        for (int rep = 0; rep < repeats; ++rep) {
            for (qsizetype i = 0; i < blocks.size(); ++i) {
                for (const Message& message : messages[i]) {
                    workout.messages.append({blockStart_ + message.start, blockStart_ + message.end, message.text});
                }
                blockStart_ += blocks[i].seconds;
            }
        }
        workout.entries.append(WorkoutEntry::interval(repeats, blocks));
        // each repeat's blocks, the repeats left counting down, each ramp followed by its steps
        for (int left = repeats; left > 0; --left) {
            for (qsizetype i = 0; i < blocks.size(); ++i) {
                workout.entries.append(WorkoutEntry::intervalBlock(left, static_cast<int>(i), blocks[i]));
                workout.entries.append(steps[i]);
            }
        }
        return true;
    }

    // ---- Values ----

    template <typename... Args>
    std::nullopt_t fail(Args&&... args) {
        reject(std::forward<Args>(args)...);
        return std::nullopt;
    }

    bool duration(const QString& text, int lineNumber, const QString& line, int repeats, int& seconds) {
        if (!py::isDigits(text)) {
            return reject(QStringLiteral("Block durations must be integers"), lineNumber, line);
        }
        const long long value = digitsValue(text);
        if (value < 1 || value > options_.maxBlockSeconds) {
            return reject(QStringLiteral("Block durations must be between 1 and %1s").arg(options_.maxBlockSeconds),
                          lineNumber, line);
        }
        seconds = static_cast<int>(value);
        result.workout.seconds += seconds * repeats;
        return true;
    }

    bool position(const QString& text, int lineNumber, const QString& line, int& position) {
        const std::optional<long long> value = py::toInt(text);
        if (!value || *value < 0 || *value > 5) {
            return reject(QStringLiteral("Position must be 0 to 5"), lineNumber, line);
        }
        position = static_cast<int>(*value);
        return true;
    }

    std::optional<Power> powerValue(QString text, int lineNumber, const QString& line) {
        text = text.trimmed();
        if (text.endsWith(u'W') || text.endsWith(u'w')) {
            const QString number = text.chopped(1);
            if (py::isDigits(number.trimmed())) {
                const long long watts = digitsValue(number.trimmed());
                if (watts >= kMinExplicitWatts && watts <= kMaxExplicitWatts) {
                    return Power{static_cast<double>(watts), true};
                }
            }
            return fail(QStringLiteral("Watts must be a whole number, %1 to %2W").arg(kMinExplicitWatts)
                            .arg(kMaxExplicitWatts),
                        lineNumber, line);
        }
        const std::optional<double> multiplier = py::toFloat(text);
        if (!multiplier || !(*multiplier >= kMinFtpMultiplier && *multiplier <= kMaxFtpMultiplier)) {
            return fail(QStringLiteral("Power must be 0.05 to 3 (x FTP) or watts (e.g. 250W)"), lineNumber, line);
        }
        return Power{*multiplier, false};
    }

    // A power at one of the difficulties: an FTP multiplier times that difficulty's FTP, or explicit watts moved the
    // same fraction a step as FTP is
    double wattsAt(const Power& power, int difficulty) const {
        if (power.explicitWatts) {
            return power.value * (1 + (difficulty - 10) * kDifficultyStep);
        }
        return power.value * (options_.ftp + ftpIncrement_[static_cast<std::size_t>(difficulty)]);
    }

    Targets targets(const Power& power) const {
        Targets values{};
        for (int d = 0; d < kDifficulties; ++d) {
            values[static_cast<std::size_t>(d)] = static_cast<int>(wattsAt(power, d));
        }
        return values;
    }

    // A ramp's steps: a new target for each whole watt it moves (at the written difficulty), at most one a second,
    // covering the whole ramp: the workout screen takes the entry after a ramp block as its first step, and moves on
    // once the last runs out (a ramp too shallow to move a whole watt still gets one)
    QList<WorkoutEntry> rampSteps(int seconds, const Power& start, const Power& end) const {
        const int adjustedStart = static_cast<int>(wattsAt(start, kBaseDifficulty));
        const int adjustedEnd = static_cast<int>(wattsAt(end, kBaseDifficulty));
        const double slope = static_cast<double>(adjustedEnd - adjustedStart) / seconds;
        int lastPower = adjustedStart;
        int lastSecond = 0;
        QList<std::pair<int, int>> stepAt;  // (second, how long since the last)
        for (int i = 0; i <= seconds; ++i) {
            const int power = static_cast<int>(slope * i + adjustedStart);
            if (power != lastPower && i - lastSecond >= 1) {
                lastPower = power;
                stepAt.append({i, i - lastSecond});
                lastSecond = i;
            }
        }
        if (lastSecond < seconds) {
            stepAt.append({seconds, seconds - lastSecond});
        }
        QList<WorkoutEntry> steps;
        for (const auto& [second, held] : stepAt) {
            Targets power{};
            for (int d = 0; d < kDifficulties; ++d) {
                const double endPower = wattsAt(end, d);
                const double startPower = wattsAt(start, d);
                power[static_cast<std::size_t>(d)] =
                    static_cast<int>(startPower + (((endPower - startPower) * second) / seconds));
            }
            steps.append(WorkoutEntry::rampStep(held, power, seconds));
        }
        return steps;
    }

    // A block's messages, from its start; empty (rejected) if they're not right
    std::optional<QList<Message>> blockMessages(const QStringList& fields, int blockSeconds, const QString& line,
                                                int lineNumber) {
        QList<Message> messages;
        if (fields.isEmpty()) {
            return messages;
        }
        QList<QStringList> groups;
        if (fields.size() <= 3) {
            groups.append(fields);
        } else if (fields.size() % 3 == 0) {
            for (qsizetype i = 0; i < fields.size(); i += 3) {
                groups.append(fields.mid(i, 3));
            }
        } else {
            return fail(QStringLiteral("Multiple messages need |message|seconds|start each"), lineNumber, line);
        }
        for (const QStringList& group : groups) {
            const QString text = group[0].trimmed();
            const QString secondsText = group.size() > 1 ? group[1].trimmed() : QString();
            const QString startText = group.size() > 2 ? group[2].trimmed() : QString();
            if (text.isEmpty()) {
                return fail(QStringLiteral("Message text can't be empty"), lineNumber, line);
            }
            long long start = 0;
            if (!startText.isEmpty()) {
                if (!py::isDigits(startText) || digitsValue(startText) >= blockSeconds) {
                    return fail(QStringLiteral("Message start must be before the block ends"), lineNumber, line);
                }
                start = digitsValue(startText);
            }
            long long seconds = blockSeconds - start;
            if (!secondsText.isEmpty()) {
                if (!py::isDigits(secondsText) || digitsValue(secondsText) < 1) {
                    return fail(QStringLiteral("Message seconds must be a whole number, 1+"), lineNumber, line);
                }
                seconds = digitsValue(secondsText);
            }
            if (start + seconds > blockSeconds) {
                return fail(QStringLiteral("Message must end by the end of its block"), lineNumber, line);
            }
            messages.append({static_cast<int>(start), static_cast<int>(start + seconds), text});
        }
        return messages;
    }
};

}  // namespace

AwoResult parseAwoLines(const QStringList& lines, const AwoOptions& options) {
    Parser parser(options);
    parser.parse(lines);
    return parser.result;
}

QStringList splitLines(const QString& contents) {
    QString text = contents;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    QStringList lines = text.split(u'\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

AwoResult readWorkoutFile(const QString& path, const AwoOptions& options) {
    AwoResult result;
    const QString extension = QFileInfo(path).suffix().toLower();
    if (extension == u"awo") {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.errors.append({QStringLiteral("Can't open the file (%1)").arg(file.errorString())});
            return result;
        }
        QByteArray data = file.readAll();
        if (data.startsWith("\xEF\xBB\xBF")) {
            data.remove(0, 3);
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString contents = decoder.decode(data);
        if (decoder.hasError()) {
            result.errors.append({QStringLiteral("File must be saved as UTF-8 text")});
            return result;
        }
        return parseAwoLines(splitLines(contents), options);
    }
    if (extension == u"zwo") {
        // a Zwift workout, read as the .awo it converts to
        const ZwoConversion conversion = convertZwo(path);
        if (!conversion.ok()) {
            result.errors.append({conversion.error});
            return result;
        }
        result = parseAwoLines(conversion.lines, options);
        result.warnings = conversion.warnings;
        return result;
    }
    result.errors.append({QStringLiteral("File must be of type .awo or .zwo")});
    return result;
}

}  // namespace awo
