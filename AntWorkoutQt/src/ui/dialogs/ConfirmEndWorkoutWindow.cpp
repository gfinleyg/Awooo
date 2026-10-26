#include "ConfirmEndWorkoutWindow.hpp"

#include <QCloseEvent>
#include <QGridLayout>
#include <QPushButton>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"

namespace awo {

ConfirmEndWorkoutWindow::ConfirmEndWorkoutWindow(HudScreen* hud) : hud_(hud) {
    const bool workoutMode = appState().workoutMode;
    auto* titleBar = framed(this, QStringLiteral("confirmEndWorkoutWindow"),
                            workoutMode ? QStringLiteral("End Awooo Workout?") : QStringLiteral("End Free Ride?"));
    if (workoutMode) {
        restart_ = new QPushButton(QStringLiteral("Keep Existing Data and Restart"));
        connect(restart_, &QPushButton::clicked, this, &ConfirmEndWorkoutWindow::restartWorkout);
    }
    setup_ = new QPushButton(QStringLiteral("Clear Data and Go Back to Setup"));
    synopsis_ = new QPushButton(QStringLiteral("Exit to Synopsis"));
    keepGoing_ = new QPushButton(QStringLiteral("Keep Going!"));
    connect(setup_, &QPushButton::clicked, this, &ConfirmEndWorkoutWindow::backToSetup);
    connect(synopsis_, &QPushButton::clicked, this, &ConfirmEndWorkoutWindow::endWorkout);
    connect(keepGoing_, &QPushButton::clicked, this, &ConfirmEndWorkoutWindow::keepGoing);
    // a running workout waits while it's asked
    if (hud_->engine().unpaused() && workoutMode) {
        hud_->pauseRecording();
        pausedHere_ = true;
    }
    auto* layout = new QGridLayout(this);
    layout->addWidget(titleBar, 0, 0, 1, 2);
    if (restart_) {
        layout->addWidget(restart_, 1, 0, 1, 2);
    }
    layout->addWidget(setup_, 2, 0, 1, 2);
    layout->addWidget(synopsis_, 3, 0);
    layout->addWidget(keepGoing_, 3, 1);
    if (restart_) {
        keys::add(restart_, Qt::Key_R, QStringLiteral("R"));
    }
    keys::add(setup_, Qt::Key_B, QStringLiteral("B"));
    keys::add(synopsis_, Qt::Key_E, QStringLiteral("E"));
    keys::add(keepGoing_, Qt::Key_Space, QStringLiteral("Spacebar"));
    layout::fitDialog(this);
}

void ConfirmEndWorkoutWindow::restartWorkout() {
    pausedHere_ = false;
    hud_->restartWorkout();
    hide();
}

void ConfirmEndWorkoutWindow::backToSetup() {
    pausedHere_ = false;
    hud_->stopTimers();
    hud_->restartWorkout();
    hide();
    emit hud_->setupRequested();
}

void ConfirmEndWorkoutWindow::endWorkout() {
    synopsis_->setEnabled(false);
    pausedHere_ = false;
    hud_->stopTimers();
    if (appState().workoutMode) {
        hud_->endGrading();
    }
    hud_->engine().stop();
    hud_->stopReadings();
    hide();
    emit hud_->synopsisRequested();
}

void ConfirmEndWorkoutWindow::keepGoing() {
    resumeIfPausedHere();
    hide();
}

void ConfirmEndWorkoutWindow::closeEvent(QCloseEvent* event) {
    resumeIfPausedHere();
    QWidget::closeEvent(event);
}

// (once: Keep Going and then a close don't toggle it back to paused)
void ConfirmEndWorkoutWindow::resumeIfPausedHere() {
    if (pausedHere_) {
        pausedHere_ = false;
        hud_->pauseRecording();
    }
}

}  // namespace awo
