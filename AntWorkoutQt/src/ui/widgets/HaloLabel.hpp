#pragma once

// A label over the window's gradient (the workout screen's "Up Next:" and "Coming up..." headers, the setup screen's
// device headers) whose text gets a one pixel outline where it doesn't stand out from the gradient behind it (contrast
// under halo::kMinContrast). The outline is the gradient behind the text taken toward the lightest of the theme's
// background colors, only as far as it needs to go to be seen against the text.
//
// The halo namespace is that outline for text painted some other way: the zone legend's numbers, a gauge's reading.

#include <QLabel>

class QPainter;

namespace awo {

namespace halo {

// WCAG AA for text: text that contrasts less than this with what's behind it gets an outline
constexpr double kMinContrast = 4.5;
// how far out the outline is, in device pixels: as thin as an outline can be
constexpr double kDevicePx = 1;

bool needed(const QColor& text, const QColor& behind);
// the outline's color: behind taken toward lightest only as far as it needs to go to stand out from the text (all the
// way, if even that doesn't). Without lightest, the lightest of the colors the theme gives the backgrounds
QColor color(const QColor& text, const QColor& behind);
QColor color(const QColor& text, const QColor& behind, const QColor& lightest);
// the outline of text as drawText would draw it in rect with these flags: the text in color all around where it goes
// (the painter's font; the text itself is the caller's to draw over it)
void draw(QPainter& painter, const QRectF& rect, int flags, const QString& text, const QColor& color);

}  // namespace halo

class HaloLabel : public QLabel {
    Q_OBJECT

public:
    explicit HaloLabel(const QString& text = {}, QWidget* parent = nullptr);

    // where the text is drawn, in the label
    QRectF textRect() const;
    // whether the text is drawn over a halo (with the theme as it is and where this is in its window)
    bool haloed() const;
    // the outline's color
    QColor haloColor() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // the rect QLabel draws the text in
    QRect layoutRect() const;
};

}  // namespace awo
