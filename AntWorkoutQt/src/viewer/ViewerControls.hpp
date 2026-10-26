#pragma once

// LayoutViewer's controls window (LayoutViewer.py's ControlWindow): which view the display window shows (and Play),
// the fit report, and the tabs:
//   Screen      a simulated screen size (the app scales itself to fit it, see AppLayout), screenshots
//   Fonts       each font role's family, size and weight
//   Colors      every color the app uses, once each (changing one changes it everywhere, gradients included)
//   Scenario    the mode every view is built in, the workout file, how far through it, battery warnings, a trainer,
//               an example workout message, the key bindings
//   FTP Colors  the power zone colors, shown on their own mockup window while the tab is open
// and the theme file: save, save as the default, load, reset. On the Fonts and Colors tabs a click on the display
// identifies what's clicked instead of clicking it.

#include <QMap>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>

#include "ViewerColors.hpp"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;

namespace awo {

class FtpColorsTab;
class FtpMockupWindow;

namespace viewer {

class DisplayWindow;
class PreviewWindow;

// simulated screens: the effective (after Windows display scaling) size, and what typically has it (none: this one)
struct ScreenSize {
    std::optional<QSize> size;
    QString note;
};
extern const QList<ScreenSize> kScreenSizes;
extern const QString kScreenshotDir;
extern const QString kExampleMessage;

class ControlWindow : public QWidget {
    Q_OBJECT

public:
    // families: the app's own fonts (listed first)
    ControlWindow(DisplayWindow* display, const QStringList& families);
    ~ControlWindow() override;

    // every view built again (in the mode, with the workout file and progress picked)
    void rebuild();
    // this screen size's scale, then the theme on top; place: the display window moved and sized for it
    void applyLayout(bool place = true);
    // the display window was moved to another monitor
    void displayScreenChanged();
    void setHomeScreen(QScreen* screen) { homeScreen_ = screen; }

    // ---- What's on it (tests) ----
    QTabWidget* tabs() const { return tabs_; }
    QComboBox* screenCombo() const { return screenCombo_; }
    QComboBox* modeCombo() const { return modeCombo_; }
    QComboBox* workoutCombo() const { return workoutCombo_; }
    QSlider* progressSlider() const { return progressSlider_; }
    QCheckBox* messageCheck() const { return messageCheck_; }
    QCheckBox* taskbarCheck() const { return taskbarCheck_; }
    QCheckBox* batteryCheck() const { return batteryCheck_; }
    QCheckBox* trainerCheck() const { return trainerCheck_; }
    QLabel* fitLabel() const { return fitLabel_; }
    QLabel* statusLabel() const { return statusLabel_; }
    QPushButton* playButton() const { return playButton_; }
    QWidget* fontsTab() const { return fontsTab_; }
    QWidget* colorsTab() const { return colorsTab_; }
    QWidget* ftpPage() const { return ftpPage_; }
    FtpMockupWindow* ftpMockup() const { return ftpMockup_; }
    // the colors listed (with what uses them), and the gradients
    QStringList listedColors() const { return colorRows_.keys(); }
    QList<Gradient> listedGradients() const { return gradientRows_.keys(); }
    QString usesText(const QString& color) const;
    // a font role's controls: family, size (%) and weight
    QComboBox* fontFamily(const QString& role) const;
    QSpinBox* fontSize(const QString& role) const;
    QComboBox* fontWeight(const QString& role) const;
    // the available area (after the taskbar) of the screen size picked
    QSize availableSize() const;
    // the simulated screen size picked (none: this screen's own)
    std::optional<QSize> screenSize() const;
    // (name, needed, fits) for each view at the size picked
    struct Fit {
        QString name;
        QSize need;
        bool fits;
    };
    QList<Fit> fitResults() const;
    // a color replaced (empty: back to the original), as picking it does
    void setColor(const QString& color, const std::optional<QString>& replacement);
    void identify(QWidget* widget);
    void showUses(const QString& color, const Gradient& gradient = {});
    void screenshot();
    void captureAll();
    void saveThemeTo(const QString& path);
    void loadThemeFrom(const QString& path);
    void savePartTo(const QString& part, const QString& path);
    void loadPartFrom(const QString& part, const QString& path);
    void saveDefaultTheme(bool ask = true);
    void resetTheme();
    // (while busy: true, see busy)
    bool building() const { return busyRunning_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct ColorRow {
        QPushButton* swatch;
        ClickableLabel* name;
        QLabel* uses;
        QPushButton* reset;
    };
    struct FontControls {
        QComboBox* family;
        QSpinBox* size;
        QComboBox* weight;
    };

    QWidget* buildScreenTab();
    QWidget* buildFontsTab(const QStringList& families);
    QWidget* buildColorsTab();
    QHBoxLayout* partButtons(const QString& part, const QString& name);
    QWidget* buildScenarioTab();
    void tabChanged(int index);
    void playingChanged(bool playing);
    void viewChanged(const QString& name);
    void modeChanged();
    void rebuildWorkout();
    void applyExampleMessage();
    void keysToggled(bool show);
    void updateKeyLabels();
    void trainerChanged(bool trainer);
    // one build at a time: a rebuild asked for during one waits, and only the latest waiting one runs, once this is
    // done; a waiting full rebuild (every view) isn't replaced by a Workout view one
    void busy(std::function<void()> work, bool full = false);
    void monitorChanged();
    void placeDisplay();
    QScreen* homeScreen();
    bool isOversize() const;
    void afterLayout();
    void updateFitReport();
    QString screenTag() const;
    void syncFontControls();
    void fontChanged(const QString& role, const QString& key, const QVariant& value);
    // ({color: views}, {gradient: views}) across the app stylesheet, the window and each view; "All" for what every
    // view shares
    std::pair<QMap<QString, QSet<QString>>, QMap<Gradient, QSet<QString>>> usedColors() const;
    void refreshColorList();
    static QString usesText(const QSet<QString>& where);
    void updateColorRow(const QString& color);
    void pickColor(const QString& color);
    QString fontRoleOf(QWidget* widget) const;
    void applyTheme();
    void saveTheme();
    void savePart(const QString& part);
    void loadPart(const QString& part);
    void loadTheme();

    DisplayWindow* display_;
    PreviewWindow* preview_;
    QPointer<QScreen> homeScreen_;
    QTimer* monitorTimer_;
    QTimer* restyleTimer_;
    QMap<QString, ColorRow> colorRows_;
    QMap<Gradient, GradientPreview*> gradientRows_;
    // every color (and gradient) seen in any view, in any mode, this session: where it's used
    QMap<QString, QSet<QString>> knownColors_;
    QMap<Gradient, QSet<QString>> knownGradients_;
    QButtonGroup* viewButtons_;
    QPushButton* playButton_;
    QLabel* fitLabel_;
    QTabWidget* tabs_;
    QWidget* fontsTab_;
    QWidget* colorsTab_;
    QWidget* ftpPage_;
    FtpColorsTab* ftpTab_;
    FtpMockupWindow* ftpMockup_ = nullptr;
    QLabel* statusLabel_;
    QComboBox* screenCombo_;
    QSpinBox* customW_;
    QSpinBox* customH_;
    QCheckBox* taskbarCheck_;
    QLabel* scaleNote_;
    QMap<QString, FontControls> fontControls_;
    QMap<QString, QLabel*> fontRoleLabels_;
    QWidget* colorListWidget_;
    QGridLayout* colorList_;
    QScrollArea* colorScroll_;
    QComboBox* modeCombo_;
    QComboBox* workoutCombo_;
    QSlider* progressSlider_;
    QLabel* progressLabel_;
    QCheckBox* batteryCheck_;
    QCheckBox* trainerCheck_;
    QCheckBox* messageCheck_;
    QCheckBox* keysCheck_;
    QTimer* keyLabelTimer_;
    bool busyRunning_ = false;
    std::function<void()> busyNext_;
    bool busyNextFull_ = false;
};

}  // namespace viewer
}  // namespace awo
