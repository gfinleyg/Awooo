// The files the C++ app shares with the Python app, both ways (PLAN.md §6.5): golden/files.json holds the settings,
// the song picker's selection and a log line as the Python app writes them. The C++ app reads each as the Python
// app meant it and writes the same, and the workout picker reads and writes the Python app's list. (The music cache
// is tst_music's, the theme files tst_layout_viewer's.)

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QtTest>

#include "AnalysisCache.hpp"
#include "AppState.hpp"
#include "Logger.hpp"
#include "SavedSettings.hpp"
#include "TestSupport.hpp"
#include "WorkoutSelectDialog.hpp"

using namespace awo;

namespace {

QJsonValue readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
}

QJsonObject python() {
    return awotest::golden(QStringLiteral("files.json")).toObject();
}

}  // namespace

class TestSharedFiles : public QObject {
    Q_OBJECT

private slots:
    void init() { dir_ = std::make_unique<awotest::TempWorkingDir>(); }
    void cleanup() { dir_.reset(); }

    void settings() {
        const QJsonObject saved = python().value(QStringLiteral("settings")).toObject();
        // the Python app's file read as it wrote it
        awotest::writeFile(QStringLiteral("savedSettingsANT.json"), QJsonDocument(saved).toJson());
        QCOMPARE(settings::load(), saved);
        // and the same settings written here are the same file (Qt's key order and indents, see CHANGES.md)
        QFile::remove(QStringLiteral("savedSettingsANT.json"));
        QVERIFY(settings::update(saved));
        QCOMPARE(readJson(QStringLiteral("savedSettingsANT.json")).toObject(), saved);
    }

    void musicSelection() {
        const QJsonArray saved = python().value(QStringLiteral("selection")).toArray();
        awotest::writeFile(QStringLiteral("musicSelectionANT.json"), QJsonDocument(saved).toJson());
        const QList<music::SelectedSong> songs = music::loadSelection();
        QCOMPARE(songs.size(), 2);
        QCOMPARE(songs[0].path, QStringLiteral("C:/Music/Ünïcode Band - A.mp3"));
        QCOMPARE(songs[0].title, QStringLiteral("Ünïcode Band - A"));
        QVERIFY(songs[0].checked);
        QVERIFY(!songs[1].checked);
        QFile::remove(QStringLiteral("musicSelectionANT.json"));
        music::saveSelection(songs);
        QCOMPARE(readJson(QStringLiteral("musicSelectionANT.json")).toArray(), saved);
    }

    void logLines() {
        // the same line as the Python app's, timestamp aside: the same bytes (UTF-8, CRLF)
        QByteArray tail;
        for (const QJsonValue& byte : python().value(QStringLiteral("logTail")).toArray()) {
            tail.append(static_cast<char>(byte.toInt()));
        }
        log::event(QStringLiteral("TEST"), QStringLiteral("a line, with ünïcode"));
        QFile file(QStringLiteral("antWorkoutLog.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray written = file.readAll();
        file.close();
        QCOMPARE(written.mid(23), tail);
        QVERIFY(QDateTime::fromString(QString::fromLatin1(written.left(23)), QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).isValid());
        // a log the Python app wrote, read here
        QFile::remove(QStringLiteral("antWorkoutLog.txt"));
        awotest::writeFile(QStringLiteral("antWorkoutLog.txt"), QByteArray("2026-10-26 15:34:00.123") + tail);
        QVERIFY(log::read().contains(QStringLiteral("2026-10-26 15:34:00.123 [TEST] a line, with ünïcode")));
    }

    void workoutPickerList() {
        // the list as the Python app's workout picker saves it: {"files": [...], "last": path}
        QDir().mkpath(QStringLiteral("awoWorkouts"));
        QStringList paths;
        for (const QString& name : {QStringLiteral("0_Little_bit_of_everything.awo"), QStringLiteral("04_Z2_3.awo")}) {
            QFile::copy(awotest::pythonAppDir() + QStringLiteral("/awoWorkouts/") + name, QStringLiteral("awoWorkouts/") + name);
            paths.append(QDir::current().absoluteFilePath(QStringLiteral("awoWorkouts/") + name));
        }
        const QJsonObject saved{{QStringLiteral("files"), QJsonArray::fromStringList(paths)}, {QStringLiteral("last"), paths[1]}};
        awotest::writeFile(QStringLiteral("workoutPickerANT.json"), QJsonDocument(saved).toJson(QJsonDocument::Compact));
        appState().workoutFilename.clear();
        WorkoutSelectDialog dialog;
        QCOMPARE(dialog.paths(), paths);
        QCOMPARE(dialog.selectedPath(), paths[1]);
        QFile::remove(QStringLiteral("workoutPickerANT.json"));
        dialog.reject();
        QCOMPARE(readJson(QStringLiteral("workoutPickerANT.json")).toObject(), saved);
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_MAIN(TestSharedFiles)
#include "tst_shared_files.moc"
