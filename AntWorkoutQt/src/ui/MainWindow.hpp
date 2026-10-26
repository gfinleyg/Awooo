#pragma once

// The app's one window (ClassMainWindow.py): frameless and maximized, the title bar above whichever screen is up
// (setup, workout, synopsis), and the ANT connection, which lives as long as it does.

#include <QMainWindow>
#include <QPointer>

#include <memory>

#include "AntBridge.hpp"

class QVBoxLayout;

namespace awo {

class ConfirmExitWindow;
class CustomTitleBar;
class HudScreen;
class SetupScreen;
class SynopsisScreen;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // the title bar's text; the workout and synopsis screens add the workout's name
    static const QString kAppTitle;

    // bridge: the ANT connection (a real one, started, by default)
    explicit MainWindow(std::unique_ptr<ant::AntBridge> bridge = nullptr);
    ~MainWindow() override;

    ant::AntBridge* bridge() const { return bridge_.get(); }
    CustomTitleBar* titleBar() const { return titleBar_; }
    SetupScreen* setupScreen() const { return setup_; }
    HudScreen* hudScreen() const { return hud_; }
    SynopsisScreen* synopsisScreen() const { return synopsis_; }
    // START on the setup screen: the workout screen
    void generateHud();
    // the END prompt's Clear Data and Go Back to Setup
    void goBackToSetup();
    // the END prompt's Exit to Synopsis: the ride joins the session, and the session's synopsis
    void generateSynopsis();
    // the synopsis' Return to Setup
    void returnFromSynopsis();
    // just the app's name, or (on the workout and synopsis screens, riding a workout file or an FTP test) with the
    // workout's name
    void setAppTitle(bool showWorkoutName = false);
    // a screen below the title bar, under its view name, laid out for the screen
    void addScreen(QWidget* screen, const QString& name);
    // removes and destroys a screen built before (so one built twice doesn't leave two stacked up)
    void discardScreen(QWidget* screen);
    // Esc: the screen's own (the workout's End Workout prompt, the synopsis' overlays), else the exit prompt
    void escapePressed();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    std::unique_ptr<ant::AntBridge> bridge_;
    CustomTitleBar* titleBar_;
    SetupScreen* setup_;
    HudScreen* hud_ = nullptr;
    SynopsisScreen* synopsis_ = nullptr;
    QVBoxLayout* layout_;
    QPointer<ConfirmExitWindow> exitPrompt_;
};

}  // namespace awo
