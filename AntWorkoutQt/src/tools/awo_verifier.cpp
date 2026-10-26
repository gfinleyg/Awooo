// awo-verifier: checks .awo workout files with the app's own reader, reporting every bad line (awo-verifier.py)
//   awo-verifier                       every .awo file in the current folder
//   awo-verifier <folder or file> ...  those .awo files (a folder's .awo files)
// Prints nothing for a file that's fine; exits with 1 if any file isn't.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

#include <windows.h>

#include "AwoParser.hpp"

namespace {

QStringList awoFiles(QStringList arguments) {
    if (arguments.isEmpty()) {
        arguments << QStringLiteral(".");
    }
    QStringList files;
    for (const QString& argument : arguments) {
        const QFileInfo info(argument);
        if (info.isDir()) {
            QStringList names = QDir(argument).entryList(QDir::Files);
            std::sort(names.begin(), names.end());
            for (const QString& name : names) {
                if (name.endsWith(QStringLiteral(".awo"), Qt::CaseInsensitive)) {
                    files << QDir(argument).filePath(name);
                }
            }
        } else {
            files << argument;
        }
    }
    return files;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    SetConsoleOutputCP(CP_UTF8);
    QTextStream out(stdout);

    bool anyInvalid = false;
    for (const QString& path : awoFiles(app.arguments().mid(1))) {
        // (any FTP reads the same blocks as valid or not)
        const awo::AwoResult result = awo::readWorkoutFile(path, {200, awo::kMaxBlockSeconds, true});
        const QString name = QFileInfo(path).fileName();
        for (const awo::AwoError& error : result.errors) {
            anyInvalid = true;
            if (error.line > 0) {
                out << "Invalid workout: file;line\n" << name << ';' << error.line << '\n'
                    << error.reason << ":\n" << error.text << '\n';
            } else {
                out << "Invalid workout: file\n" << name << '\n' << error.reason << '\n';
            }
        }
    }
    return anyInvalid ? 1 : 0;
}
