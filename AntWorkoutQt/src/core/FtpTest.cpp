#include "FtpTest.hpp"

#include "PyCompat.hpp"

namespace awo::ftptest {

const QStringList kWarmupLines = {QStringLiteral("1:180,0.5"), QStringLiteral("1:60,1.0"), QStringLiteral("1:120,0.6"),
                                  QStringLiteral("1:60,1.1"), QStringLiteral("2:480,0.6,0.8")};

const QString kNotPedalingMessage = QStringLiteral("Done yet? Keep pedaling to continue...");

namespace {

QList<Test>& allTests() {
    static QList<Test> all = [] {
        QList<Test> list;
        Test twenty{QStringLiteral("20m"), Test::Kind::Effort, QStringLiteral("20m FTP Test"), 0.95,
                    QStringLiteral("20m Avg:")};
        twenty.effortSeconds = 1200;
        twenty.extraMessages = {{60, 30, QStringLiteral("Average shown above will be 95% of the true average to match "
                                                        "your new FTP over the 20m")}};
        list << twenty;
        Test hour{QStringLiteral("1h"), Test::Kind::Effort, QStringLiteral("1h FTP Test"), 1.0, QStringLiteral("Avg:")};
        hour.effortSeconds = 3600;
        list << hour;
        Test ramp10{QStringLiteral("ramp10"), Test::Kind::Ramp, QStringLiteral("10W delta FTP Ramp"), 0.75,
                    QStringLiteral("Ramp FTP:")};
        ramp10.startWatts = 50;
        ramp10.stepWatts = 10;
        ramp10.unsetFtpWatts = 50;
        list << ramp10;
        Test ramp20{QStringLiteral("ramp20"), Test::Kind::Ramp, QStringLiteral("20W delta FTP Ramp"), 0.75,
                    QStringLiteral("Ramp FTP:")};
        ramp20.startWatts = 100;
        ramp20.stepWatts = 20;
        ramp20.unsetFtpWatts = 100;
        list << ramp20;
        return list;
    }();
    return all;
}

std::optional<QStringList>& warmupOverride() {
    static std::optional<QStringList> lines;
    return lines;
}

}  // namespace

const QList<Test>& tests() {
    return allTests();
}

void shortenForTests(const QString& key, const QStringList& warmupLines, int effortSeconds) {
    warmupOverride() = warmupLines;
    for (Test& test : allTests()) {
        if (test.key == key) {
            test.effortSeconds = effortSeconds;
        }
    }
}

const Test* find(const QString& key) {
    for (const Test& test : tests()) {
        if (test.key == key) {
            return &test;
        }
    }
    return nullptr;
}

bool isRamp(const QString& key) {
    const Test* test = find(key);
    return test && test->kind == Test::Kind::Ramp;
}

QStringList workoutLines(const QString& key, bool ftpSet) {
    const Test& test = *find(key);
    QStringList lines{QStringLiteral("T:") + test.name};
    if (ftpSet) {
        lines += warmupOverride().value_or(kWarmupLines);
    } else {
        lines << QStringLiteral("0:%1").arg(kWarmupSeconds);
    }
    if (test.kind == Test::Kind::Effort) {
        lines << QStringLiteral("0:%1").arg(test.effortSeconds);
    }
    return lines;
}

WorkoutEntry rampBlock(const QString& key, int index) {
    const Test& test = *find(key);
    Block block;
    block.type = BlockType::Steady;
    block.seconds = kRampBlockSeconds;
    block.power.fill(test.startWatts + index * test.stepWatts);
    return WorkoutEntry::ofBlock(block);
}

QList<Message> messages(const QString& key, int effortStart, int ftp) {
    const Test& test = *find(key);
    if (test.kind == Test::Kind::Ramp) {
        return {{effortStart - 30, effortStart - 15,
                 QStringLiteral("Get ready for your %1W delta ramp test...").arg(test.stepWatts)},
                {effortStart - 15, effortStart,
                 QStringLiteral("Stop pedaling for 10s to end the test and start your cooldown.")}};
    }
    const QString effortName = test.name.split(u' ').first();
    const int effortEnd = effortStart + test.effortSeconds;
    QList<Message> timed;
    for (const ExtraMessage& extra : test.extraMessages) {
        timed.append({effortStart - extra.before, effortStart - extra.before + extra.seconds, extra.text});
    }
    timed.append({effortStart - 30, effortStart - 20, QStringLiteral("Get ready for your %1 effort").arg(effortName)});
    timed.append({effortStart - 20, effortStart - 10,
                  QStringLiteral("Your set FTP is currently: %1, shoot for slightly higher than that").arg(ftp)});
    timed.append({effortStart - 10, effortStart, QStringLiteral("Get Ready...")});
    timed.append({effortStart, effortStart + kGoSeconds, QStringLiteral("GO!")});
    timed.append({effortEnd - 120, effortEnd - 120 + kTwoMinutesLeftSeconds,
                  QStringLiteral("Two minutes left! If you kept the previous power, now is the chance to push it!")});
    return timed;
}

int ftpFromAverage(const QString& key, double average) {
    return static_cast<int>(py::round(average * find(key)->ftpFactor));
}

void EffortAverage::add(double seconds, std::optional<double> power) {
    if (!power || seconds <= 0) {
        return;
    }
    wattSeconds_ += *power * seconds;
    seconds_ += seconds;
}

std::optional<double> EffortAverage::average() const {
    if (seconds_ <= 0) {
        return std::nullopt;
    }
    return wattSeconds_ / seconds_;
}

void BestRollingAverage::add(double seconds, std::optional<double> power) {
    if (seconds <= 0) {
        return;
    }
    const double watts = power.value_or(0);
    samples_.emplace_back(seconds, watts);
    wattSeconds_ += watts * seconds;
    seconds_ += seconds;
    // drop the oldest while the rest still cover the window
    while (!samples_.empty() && seconds_ - samples_.front().first >= window_) {
        const auto [oldSeconds, oldPower] = samples_.front();
        samples_.pop_front();
        seconds_ -= oldSeconds;
        wattSeconds_ -= oldSeconds * oldPower;
    }
    if (seconds_ >= window_) {
        const double average = wattSeconds_ / seconds_;
        best_ = best_ ? std::max(*best_, average) : average;
    }
}

void BestRollingAverage::startNewWindow() {
    samples_.clear();
    wattSeconds_ = 0;
    seconds_ = 0;
}

}  // namespace awo::ftptest
