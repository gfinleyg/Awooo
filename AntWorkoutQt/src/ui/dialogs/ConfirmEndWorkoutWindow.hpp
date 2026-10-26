#pragma once

// END on the workout screen (ClassConfirmEndWorkoutWindow.py): restart (keeping what's been ridden), back to setup
// (clearing it), on to the synopsis, or keep going. A running workout is paused while it's up, and goes on again if
// the rider keeps going.

#include <QWidget>

class QPushButton;

namespace awo {

class HudScreen;

class ConfirmEndWorkoutWindow : public QWidget {
    Q_OBJECT

public:
    explicit ConfirmEndWorkoutWindow(HudScreen* hud);

    QPushButton* restartButton() const { return restart_; }
    QPushButton* setupButton() const { return setup_; }
    QPushButton* synopsisButton() const { return synopsis_; }
    QPushButton* keepGoingButton() const { return keepGoing_; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void restartWorkout();
    void backToSetup();
    void endWorkout();
    void keepGoing();
    void resumeIfPausedHere();

    HudScreen* hud_;
    QPushButton* restart_ = nullptr;
    QPushButton* setup_;
    QPushButton* synopsis_;
    QPushButton* keepGoing_;
    // the prompt paused the workout (and resumes it on Keep Going)
    bool pausedHere_ = false;
};

}  // namespace awo
