#pragma once

// LayoutViewer's colors (LayoutViewer.py's colors half): which colors and gradients each widget's styling uses
// (a gradient's colors kept together), the gradient rows' previews, and the outlines and blinks that show what was
// clicked.

#include <QLabel>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <utility>

class QTimer;

namespace awo::viewer {

// a gradient: its colors in order, "#rrggbb"
using Gradient = QStringList;

struct WidgetColors {
    QSet<QString> singles;
    QSet<Gradient> gradients;
};

// (the stylesheet without its gradients, the gradients in it); a gradient from one color to the same one is just
// that color
std::pair<QString, QSet<Gradient>> splitGradients(const QString& css);
// the single colors and gradients the widget's own styling uses (the zone colors left to the FTP Colors tab)
WidgetColors widgetColors(const QWidget* widget);

// The gradient as it looks with the theme's colors; clicking it shows where it's used
class GradientPreview : public QWidget {
    Q_OBJECT

public:
    GradientPreview(const Gradient& colors, std::function<void()> onClick);
    void setHighlight(bool on);
    bool highlighted() const { return highlight_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    Gradient colors_;
    std::function<void()> onClick_;
    bool highlight_ = false;
};

class ClickableLabel : public QLabel {
    Q_OBJECT

public:
    ClickableLabel(const QString& text, std::function<void()> onClick);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::function<void()> onClick_;
};

// A blinking outline over part of the display window, to show what was picked (only parts of it: nothing else
// can be outlined)
class OutlineFlash : public QWidget {
    Q_OBJECT

public:
    static constexpr int kBlinkMs = 160;
    static constexpr int kBlinks = 8;

    // (deletes itself once done; nullptr when widget isn't part of display)
    static OutlineFlash* flash(QWidget* display, QWidget* widget, const QString& color = QStringLiteral("#ff1744"));

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    OutlineFlash(QWidget* display, QWidget* widget, const QString& color);
    void blink();

    QColor color_;
    int blinks_ = 0;
    QTimer* timer_;
};

// Flashes a yellow highlight on control rows (the controls for what was clicked)
void blinkWidgets(const QList<QWidget*>& widgets, int times = 6, int interval = 160);

}  // namespace awo::viewer
