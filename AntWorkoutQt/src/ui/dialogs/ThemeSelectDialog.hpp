#pragma once

// The theme picker (ClassThemeSelectWindow.py), laid out like the song and workout pickers and filling the screen: the
// theme's colors (a file from themes/colors) and its power/cadence gradients (themes/gradients) picked from two lists,
// each tried on the app right away, with the workout screen beside them, part way through a ride, in the look picked
// (a RidePreview). Use This Look keeps it for next time; Cancel goes back to the look from before.

#include <QDialog>
#include <QMap>
#include <QPixmap>

#include <memory>

#include "Theme.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace awo {

class RidePreview;

namespace themepicker {

// "05-night-city.json" -> "Night City"
QString displayName(const QString& path);
// the colors a colors file's swatch shows: the window, panels, highlights and text
extern const QStringList kSwatchColors;
// after a gradient's name when its colors were moved to stand out on the theme's background
extern const QString kAdjustedMark;
// a choice is tried this long after the list stops moving (arrowing down a list doesn't load every theme on the way)
constexpr int kApplyDelayMs = 150;

}  // namespace themepicker

class ThemeSelectDialog : public QDialog {
    Q_OBJECT

public:
    // preview: draw the workout screen beside the lists (off for tests that don't look at it)
    explicit ThemeSelectDialog(QWidget* parent = nullptr, bool preview = true);
    ~ThemeSelectDialog() override;

    QListWidget* list(const QString& part) const { return lists_.value(part); }
    QPushButton* okButton() const { return ok_; }
    QString summary() const;
    QLabel* previewLabel() const { return previewLabel_; }
    // the lists' choices on the app (and the preview), those that changed
    void applyChoice();
    void pickBase();
    void loadPath(const QString& path);
    void buildPreview();

public slots:
    void accept() override;
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void fillList(const QString& part);
    QListWidgetItem* addItem(const QString& part, const QString& path);
    void refreshGradientSwatches();
    QListWidgetItem* itemFor(const QString& part, const QString& path) const;
    void select(const QString& part, const QString& path);
    void switchList();
    void loadFile();
    void updateSummary();
    void redrawPreview();
    void showPicture();
    void closePreview();
    void appliedFromTheme();

    theme::Snapshot original_;
    // the colors/gradients files tried on the app now (empty: the base theme's)
    QMap<QString, QString> applied_;
    bool wantPreview_;
    std::unique_ptr<RidePreview> preview_;
    QPixmap previewPicture_;
    QMap<QString, QListWidget*> lists_;
    QListWidget* activeList_ = nullptr;
    QLabel* previewLabel_;
    QLabel* summary_;
    QPushButton* ok_;
    QTimer* applyTimer_;
};

}  // namespace awo
