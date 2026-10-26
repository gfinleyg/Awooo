#pragma once

// The small popup windows: confirm exit, confirm clear session, the log viewer, and the settings menu's
// measurements and performance windows (ClassConfirmExitWindow.py, ClassConfirmClearSessionWindow.py,
// ClassShowLogsWindow.py, ClassMeasurementsWindow.py, ClassPerformanceWindow.py). Each is frameless with its own
// title bar, scaled for the screen it's on with its text fitted (AppLayout::fitDialog), and has a key for each button.

#include <QDialog>
#include <QWidget>

#include <functional>

class QButtonGroup;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;

namespace awo {

namespace ant {
class AntBridge;
}

class CustomTitleBar;

// Exit the app? Yes closes the ANT stick cleanly before leaving
class ConfirmExitWindow : public QWidget {
    Q_OBJECT

public:
    explicit ConfirmExitWindow(ant::AntBridge* bridge);
    QPushButton* yesButton() const { return yes_; }
    QPushButton* noButton() const { return no_; }

private:
    void exitProgram();

    ant::AntBridge* bridge_;
    QPushButton* yes_;
    QPushButton* no_;
};

// The setup screen's Clear Session, which can't be undone
class ConfirmClearSessionWindow : public QWidget {
    Q_OBJECT

public:
    explicit ConfirmClearSessionWindow(std::function<void()> clearSession);
    QPushButton* yesButton() const { return yes_; }

private:
    QPushButton* yes_;
};

// The event log (antWorkoutLog.txt), refreshed while it shows
class ShowLogsWindow : public QWidget {
    Q_OBJECT

public:
    ShowLogsWindow();
    void readLog();
    QPlainTextEdit* text() const { return text_; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPlainTextEdit* text_;
    QTimer* timer_;
};

// Weight in lbs or kg, power to weight in W/kg or W/lb, distance in miles or km; each choice applies (and is saved)
// as it's clicked
class MeasurementsWindow : public QDialog {
    Q_OBJECT

public:
    explicit MeasurementsWindow(QWidget* parent = nullptr);
    // a row's next choice, wrapping (a click, so it applies as a mouse click does)
    static void nextChoice(QButtonGroup* group);
};

// How smoothly the gauge needles move, against how much CPU they use; applies (and is saved) as the slider moves
class PerformanceWindow : public QDialog {
    Q_OBJECT

public:
    explicit PerformanceWindow(QWidget* parent = nullptr);
    QSlider* slider() const { return slider_; }

private:
    void showValue();

    QSlider* slider_;
    QLabel* value_;
};

}  // namespace awo
