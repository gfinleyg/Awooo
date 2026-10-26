// zwo2awo: makes .awo workout files from Zwift .zwo files, each beside its .zwo (zwo2awo.py)
//   zwo2awo                       every .zwo in the current folder
//   zwo2awo <folder or file> ...  those .zwo files (a folder's .zwo files)
// The conversion itself is ZwoConvert, which the app also uses to ride a .zwo file directly.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

#include <windows.h>

#include "ZwoConvert.hpp"

namespace {

QStringList zwoFiles(QStringList arguments) {
    if (arguments.isEmpty()) {
        arguments << QStringLiteral(".");
    }
    QStringList files;
    for (const QString& argument : arguments) {
        if (QFileInfo(argument).isDir()) {
            QStringList names = QDir(argument).entryList(QDir::Files);
            std::sort(names.begin(), names.end());
            for (const QString& name : names) {
                if (name.endsWith(QStringLiteral(".zwo"), Qt::CaseInsensitive)) {
                    files << QDir::toNativeSeparators(QDir(argument).filePath(name));
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

    int failed = 0;
    for (const QString& path : zwoFiles(app.arguments().mid(1))) {
        QString awoPath;
        const awo::ZwoConversion conversion = awo::writeAwo(path, {}, &awoPath);
        if (!conversion.ok()) {
            ++failed;
            out << path << ": not converted, " << conversion.error << '\n';
            continue;
        }
        out << QDir::toNativeSeparators(awoPath) << '\n';
        for (const QString& warning : conversion.warnings) {
            out << "    " << warning << '\n';
        }
    }
    return failed ? 1 : 0;
}
