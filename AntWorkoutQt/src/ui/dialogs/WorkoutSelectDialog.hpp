#pragma once

// Workout Mode's workout picker (ClassWorkoutSelectWindow.py), laid out like Musical Cadence Mode's song picker: a list
// of .awo and .zwo files (a folder of them, or files) with each one's title and length, and the one picked previewed
// beside it, block by block. Each file is read as it's added (previewWorkout), so one that can't be used says why. A
// .zwo (Zwift) file is ridden as it is, or can be converted to an .awo file beside it. The list is kept for next time
// in workoutPickerANT.json.

#include <QDialog>
#include <QHash>

#include "WorkoutTypes.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;

namespace awo {

class LongVLabels;

// the picker's list from last time: {"files": [path, ...], "last": path}
extern const QString kWorkoutListFile;
// the list to start from with nothing saved: the workouts folder beside the app
extern const QString kWorkoutFolder;
// the picker's style, for #name (the song picker's too)
QString pickerStyle(const QString& name);
constexpr int kMinRandomMinutes = 5;
constexpr int kMaxRandomMinutes = 600;
// a random pick takes any workout within this share of the minutes asked for (else the closest one)
constexpr double kRandomTolerance = 0.2;

// A workout file read as if it were being loaded, without loading it: at the app's FTP, its blocks' text in the
// target unit, or why it can't be used ("Line 3: ..." or the file's problem)
struct WorkoutPreview {
    bool ok = false;
    QString title;
    int seconds = 0;
    QString text;
    QString error;
    Workout workout;
};
WorkoutPreview previewWorkout(const QString& path);
// the same, read again only when the file (or the FTP or unit its blocks are worked out from) has changed
WorkoutPreview cachedPreview(const QString& path);
// the workout files in a folder, sorted by name
QStringList folderWorkouts(const QString& folder);

class WorkoutSelectDialog : public QDialog {
    Q_OBJECT

public:
    explicit WorkoutSelectDialog(QWidget* parent = nullptr);

    QString selectedPath() const;
    QStringList paths() const;
    void addToList(const QStringList& paths);
    void selectPath(const QString& path);
    void pickRandom();
    void convertSelected();
    QListWidget* list() const { return list_; }
    QPushButton* okButton() const { return ok_; }
    QPushButton* convertButton() const { return convert_; }
    QSpinBox* randomMinutes() const { return randomMinutes_; }
    QString summary() const;
    QString previewTitle() const;
    LongVLabels* preview() const { return preview_; }

public slots:
    void accept() override;
    void reject() override;

private:
    void addFolder();
    void addFiles();
    void removeSelected();
    void clearWorkouts();
    void showPreview(QListWidgetItem* item);
    void updateSummary();
    QString itemText(const QString& path) const;
    void save();

    QHash<QString, WorkoutPreview> previews_;
    QListWidget* list_;
    QLabel* previewTitle_;
    LongVLabels* preview_;
    QSpinBox* randomMinutes_;
    QLabel* summary_;
    QPushButton* ok_;
    QPushButton* convert_;
};

}  // namespace awo
