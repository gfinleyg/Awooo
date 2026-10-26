#pragma once

// Musical Cadence Mode's song picker (ClassMusicSelectWindow.py): a folder, songs, or an .m3u playlist; the checked
// songs make up the ride, in list order (drag to reorder). Each song is analyzed as it's added, off the UI thread
// (cached, see music::analyzeSongs). A random pick checks songs adding up to about the minutes asked for, spread over
// cadences. The list is kept for next time (musicSelectionANT.json), the settings in savedSettingsANT.json.

#include <QDialog>
#include <QHash>
#include <QThread>

#include <optional>

#include "TempoAnalysis.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace awo {

// works out the songs' tempos off the UI thread
class AnalysisThread : public QThread {
    Q_OBJECT

public:
    explicit AnalysisThread(const QStringList& paths) : paths_(paths) {}

signals:
    void progressed(int done, int total);
    void analyzed(const QHash<QString, awo::music::Analysis>& results);

protected:
    void run() override;

private:
    QStringList paths_;
};

// what a ride is built from (see music::buildWorkout)
struct MusicSelection {
    QList<std::pair<QString, std::optional<QString>>> songs;
    QHash<QString, music::Analysis> analyses;
    double preferred = 85;
    double minSegment = 8;
};

class MusicSelectDialog : public QDialog {
    Q_OBJECT

public:
    explicit MusicSelectDialog(QWidget* parent = nullptr);
    ~MusicSelectDialog() override;

    // (path, title or none for the file's) songs not already listed, checked unless checked says otherwise
    void addToList(const QList<std::pair<QString, std::optional<QString>>>& songs,
                   const std::optional<QSet<QString>>& checked = std::nullopt);
    void removeSelected();
    void clearSongs();
    void checkAll(bool checked);
    void pickRandom();
    // the checked songs that have been analyzed, in list order: (path, title)
    QList<std::pair<QString, QString>> checkedSongs() const;
    MusicSelection selection() const;
    QStringList paths() const;
    bool analyzing() const { return thread_ != nullptr; }
    // (tests) the analysis thread done, its results in
    void waitForAnalysis();
    // the analyses known (tests and LayoutViewer set them too)
    QHash<QString, music::Analysis> analyses;
    QStringList missing;

    QListWidget* list() const { return list_; }
    QPushButton* okButton() const { return ok_; }
    QSpinBox* randomMinutes() const { return randomMinutes_; }
    QSpinBox* preferredRpm() const { return preferredRpm_; }
    QSpinBox* minSegment() const { return minSegment_; }
    QString summary() const;
    void labelItem(QListWidgetItem* item);
    void updateSummary();

public slots:
    void accept() override;
    void reject() override;

private:
    void addFolder();
    void addSongs();
    void addPlaylist();
    void analyze(const QStringList& paths);
    void analysisProgress(int done, int total);
    void analysisDone(const QHash<QString, music::Analysis>& results);
    void stopAnalysis();
    QList<QListWidgetItem*> items() const;
    bool usable(QListWidgetItem* item) const;
    void save();

    AnalysisThread* thread_ = nullptr;
    QStringList pending_;
    QListWidget* list_;
    QProgressBar* progress_;
    QSpinBox* randomMinutes_;
    QSpinBox* preferredRpm_;
    QSpinBox* minSegment_;
    QLabel* summary_;
    QPushButton* ok_;
};

}  // namespace awo
