// The event log (src/core/Logger): tests/test_logger.py

#include <QThread>

#include "Logger.hpp"
#include "TestSupport.hpp"

using namespace awo;

class TestLogger : public QObject {
    Q_OBJECT

private slots:
    void init() { dir_ = std::make_unique<awotest::TempWorkingDir>(); }
    void cleanup() {
        log::setMaxLogBytesForTesting(log::kMaxLogBytes);
        dir_.reset();
    }

    void logAndRead() {
        log::event(QStringLiteral("ANT"), QStringLiteral("hello"));
        log::event(QStringLiteral("SCAN"), QStringLiteral("Ünïcödé song – title"));
        const QStringList lines = log::read().split(u'\n', Qt::SkipEmptyParts);
        QVERIFY(lines[0].endsWith(QStringLiteral("[ANT] hello")));
        QVERIFY(lines[1].endsWith(QStringLiteral("[SCAN] Ünïcödé song – title")));
        QCOMPARE(log::read(1), lines[1] + u'\n');
        // Windows line endings, as the Python app writes it
        QFile file(log::kLogFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("hello\r\n"));
    }

    void readWithoutALog() { QVERIFY(log::read().isEmpty()); }

    void clearLeavesAMarker() {
        log::event(QStringLiteral("ANT"), QStringLiteral("x"));
        log::clear();
        QVERIFY(log::read().trimmed().endsWith(QStringLiteral("[LOG] Log cleared")));
        QCOMPARE(log::read().count(u'\n'), 1);
    }

    void oldCodepageTextStillReads() {
        awotest::writeFile(log::kLogFile, "caf\xe9\n");
        QVERIFY(log::read().startsWith(QStringLiteral("caf")));
    }

    void rotationTrimsTheOldestLines() {
        log::setMaxLogBytesForTesting(2000);
        for (int i = 0; i < 100; ++i) {
            log::event(QStringLiteral("T"), QStringLiteral("line %1").arg(i, 3, 10, QLatin1Char('0')));
        }
        const QStringList lines = log::read(1000).split(u'\n', Qt::SkipEmptyParts);
        QVERIFY(QFile(log::kLogFile).size() <= 2000 + 50 * 40);
        QVERIFY(lines.last().endsWith(QStringLiteral("line 099")));
        QVERIFY(!lines.first().endsWith(QStringLiteral("line 000")));
    }

    void loggingNeverFails() {
        // (a folder where the log would go: nothing can be written, nothing breaks)
        QVERIFY(QDir().mkdir(log::kLogFile));
        log::event(QStringLiteral("ANT"), QStringLiteral("lost"));
        log::clear();
        QVERIFY(log::read().isEmpty());
    }

    void threadSafe() {
        QList<QThread*> threads;
        for (int n = 0; n < 8; ++n) {
            threads << QThread::create([n] {
                for (int i = 0; i < 50; ++i) {
                    log::event(QStringLiteral("T"), QStringLiteral("%1-%2").arg(n).arg(i));
                }
            });
        }
        for (QThread* thread : threads) {
            thread->start();
        }
        for (QThread* thread : threads) {
            thread->wait();
            delete thread;
        }
        const QStringList lines = log::read(1000).split(u'\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 400);
        for (const QString& line : lines) {
            QVERIFY(line.contains(QStringLiteral("[T]")));
        }
    }

    void qtMessagesAreLogged() {
        log::installErrorLogging();
        qWarning("careful");
        qDebug("chatter");
        log::uninstallErrorLogging();
        const QString text = log::read();
        QVERIFY2(text.contains(QStringLiteral("Qt warning")) && text.contains(QStringLiteral(": careful")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("chatter")));
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_GUILESS_MAIN(TestLogger)
#include "tst_logger.moc"
