#include "Logger.hpp"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QStringList>

#include <exception>

#include <windows.h>
#include <dbghelp.h>

namespace awo::log {

const QString kLogFile = QStringLiteral("antWorkoutLog.txt");

namespace {

QMutex& mutex() {
    static QMutex lock;
    return lock;
}

qint64 maxLogBytes = kMaxLogBytes;

QString timestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

// The file's lines, each with its newline; \r\n read as \n, anything not UTF-8 as replacement characters
QStringList readLines() {
    QFile file(kLogFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList lines = text.split(u'\n');
    // (the text after the last newline is the last line, without one)
    for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
        lines[i] += u'\n';
    }
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

// Windows line endings, as the Python app writes the file (both apps can share it)
QByteArray encode(const QString& text) {
    return QString(text).replace(QStringLiteral("\n"), QStringLiteral("\r\n")).toUtf8();
}

QString formatLine(const QString& category, const QString& message) {
    return QStringLiteral("%1 [%2] %3\n").arg(timestamp(), category, message);
}

// (the caller holds the lock, or is the crash handler, which mustn't wait for it)
void append(const QString& line) {
    QFile file(kLogFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(encode(line));
    }
}

// (the caller holds the lock)
void trimIfNeeded() {
    if (QFile(kLogFile).size() <= maxLogBytes) {
        return;
    }
    const QStringList lines = readLines();
    QFile file(kLogFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(encode(lines.mid(kTrimLines).join(QString())));
    }
}

QtMessageHandler previousHandler = nullptr;
bool handlerInstalled = false;

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const char* level = type == QtWarningMsg    ? "Qt warning"
                        : type == QtCriticalMsg ? "Qt critical"
                        : type == QtFatalMsg    ? "Qt fatal"
                                                : nullptr;
    if (level) {
        const QString location =
            context.file ? QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line) : QString();
        event(QStringLiteral("ERROR"), QStringLiteral("%1%2: %3").arg(QLatin1String(level), location, message));
    }
    if (previousHandler) {
        previousHandler(type, context, message);
    }
}

// A crash: the code and where, and a minidump beside the log
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    append(formatLine(QStringLiteral("ERROR"),
                      QStringLiteral("Crash: exception 0x%1 at 0x%2 (minidump: AntWorkout_crash.dmp)")
                          .arg(static_cast<quint32>(record->ExceptionCode), 8, 16, QLatin1Char('0'))
                          .arg(reinterpret_cast<quintptr>(record->ExceptionAddress), 0, 16)));
    const HANDLE file = CreateFileW(L"AntWorkout_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr,
                          nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler() {
    QString what = QStringLiteral("unknown exception");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = QString::fromLocal8Bit(e.what());
        } catch (...) {
        }
    }
    event(QStringLiteral("ERROR"), QStringLiteral("Unhandled exception: %1").arg(what));
    std::abort();
}

}  // namespace

void event(const QString& category, const QString& message) {
    const QString line = formatLine(category, message);
    QMutexLocker locker(&mutex());
    append(line);
    trimIfNeeded();
}

void clear() {
    QMutexLocker locker(&mutex());
    QFile file(kLogFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(encode(QStringLiteral("%1 [LOG] Log cleared\n").arg(timestamp())));
    }
}

QString read(int maxLines) {
    QMutexLocker locker(&mutex());
    const QStringList lines = readLines();
    return lines.mid(std::max<qsizetype>(0, lines.size() - maxLines)).join(QString());
}

void installErrorLogging() {
    if (!handlerInstalled) {
        previousHandler = qInstallMessageHandler(messageHandler);
        handlerInstalled = true;
    }
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);
}

void uninstallErrorLogging() {
    if (handlerInstalled) {
        qInstallMessageHandler(previousHandler);
        handlerInstalled = false;
    }
}

void setMaxLogBytesForTesting(qint64 bytes) {
    maxLogBytes = bytes;
}

}  // namespace awo::log
