#include "MainWindow.hpp"

#include <QCloseEvent>
#include <QKeySequence>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "CustomTitleBar.hpp"
#include "Dialogs.hpp"
#include "Logger.hpp"
#include "SetupScreen.hpp"
#include "Theme.hpp"

namespace awo {

const QString MainWindow::kAppTitle = QStringLiteral("Awooo Workout");

MainWindow::MainWindow(std::unique_ptr<ant::AntBridge> bridge) : bridge_(std::move(bridge)) {
    // the ANT stick opened and the live device connection started, for the life of the app
    if (!bridge_) {
        bridge_ = std::make_unique<ant::AntBridge>();
        bridge_->start();
    }
    setObjectName(QStringLiteral("mainWindow"));
    resize(800, 600);  // (this fixes some widgets' sizes before maximizing)
    // keeps the keyboard focus (no text box focused to start with: the first Tab goes to the first one)
    setFocusPolicy(Qt::StrongFocus);
    setWindowFlags(Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("   ") + kAppTitle);
    theme::setSheet(this, QStringLiteral("#mainWindow{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, "
                                         "stop:0 #28275e stop:1 #9fc5d1);}"));

    titleBar_ = new CustomTitleBar(this);
    titleBar_->setFixedHeight(36);
    auto* central = new QWidget;
    layout_ = new QVBoxLayout(central);
    layout_->addWidget(titleBar_);
    setCentralWidget(central);
    // the setup screen first, for the life of the app (the ride's screens come and go)
    setup_ = new SetupScreen(bridge_.get());
    connect(setup_, &SetupScreen::startRequested, this, &MainWindow::generateHud);
    layout::markScreen(setup_, QStringLiteral("Init"));
    layout_->addWidget(setup_);

    // Esc: the End Workout prompt during a ride (which pauses it), otherwise the same as the X (the exit prompt)
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, &MainWindow::escapePressed);
    layout::layoutWindow(this);
    showMaximized();
    setFocus();
}

MainWindow::~MainWindow() = default;

// (the workout screen comes in Phase 6 of PLAN.md: until then START comes straight back to the setup screen)
void MainWindow::generateHud() {
    setAppTitle(true);
    log::event(QStringLiteral("SETUP"), QStringLiteral("START pressed: the workout screen isn't built yet"));
    setup_->show();
    setup_->resume();
    setAppTitle();
}

void MainWindow::setAppTitle(bool showWorkoutName) {
    const AppState& state = appState();
    const QString name = state.workoutDisplayName();
    const QString title =
        showWorkoutName && state.workoutMode && !name.isEmpty() ? kAppTitle + QStringLiteral(": ") + name : kAppTitle;
    setWindowTitle(QStringLiteral("   ") + title);
    titleBar_->setTitle(QStringLiteral("   ") + title);
}

void MainWindow::addScreen(QWidget* screen, const QString& name) {
    layout::markScreen(screen, name);
    layout_->addWidget(screen);
    layout::layoutWindow(this);
    // (showMaximized() straight after swapping screens doesn't always take on this frameless window, a Qt-on-Windows
    // quirk: a tick later, once the new layout has settled, it does)
    QTimer::singleShot(0, this, &QWidget::showMaximized);
}

void MainWindow::discardScreen(QWidget* screen) {
    if (!screen) {
        return;
    }
    layout_->removeWidget(screen);
    screen->hide();
    screen->deleteLater();
}

void MainWindow::escapePressed() {
    // a screen that takes Esc itself answers escapePressed() with true
    for (QWidget* screen : centralWidget()->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        if (screen->isVisible() && screen->metaObject()->indexOfMethod("escapePressed()") >= 0) {
            bool handled = false;
            QMetaObject::invokeMethod(screen, "escapePressed", Qt::DirectConnection, Q_RETURN_ARG(bool, handled));
            if (handled) {
                return;
            }
        }
    }
    if (exitPrompt_ && exitPrompt_->isVisible()) {
        exitPrompt_->raise();
        exitPrompt_->activateWindow();
    } else {
        close();
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // the X asks first
    event->ignore();
    if (!exitPrompt_) {
        exitPrompt_ = new ConfirmExitWindow(bridge_.get());
        exitPrompt_->setAttribute(Qt::WA_DeleteOnClose);
    }
    exitPrompt_->show();
    exitPrompt_->raise();
    exitPrompt_->activateWindow();
}

}  // namespace awo
