#pragma once

// The title bar of the frameless main window and popup windows (ClassCustomTitleBar.py): the title, the main
// window's settings menu (logs, themes, measurements, performance), minimize/maximize/close, dragging, and snapping
// to maximized when dropped at the top of a screen.

#include <QPoint>
#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QToolButton;

namespace awo {

class CustomTitleBar : public QWidget {
    Q_OBJECT

public:
    // letting go of a drag with the cursor this close to the top of a screen maximizes the window there
    static constexpr int kSnapDistance = 6;

    // mainWindow: the settings menu, and snapping; popup windows just move, and Esc is their X
    explicit CustomTitleBar(QWidget* parent, bool mainWindow = true);

    void setTitle(const QString& text);
    QToolButton* settingsButton() const { return settingsButton_; }
    QToolButton* closeButton() const { return closeButton_; }

    // Pick Theme's window (the theme picker; set by the app once it's built)
    static std::function<void(QWidget* parent)>& themePicker();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void showLogs();
    void pickTheme();
    void clearTheme();
    void randomTheme(bool on);
    void snapToTop(const QPoint& cursor);

    QLabel* title_ = nullptr;
    QToolButton* settingsButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    std::optional<QPoint> initialPos_;
    bool snaps_ = true;
};

}  // namespace awo
