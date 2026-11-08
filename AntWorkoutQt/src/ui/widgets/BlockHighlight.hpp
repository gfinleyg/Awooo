#pragma once

#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QWidget>

class QLabel;

namespace awo {

// The workout screen's lit block, drawn over its block labels while it moves (C++ only, see CHANGES.md):
// - when an interval's next block starts, the lit box slides from the row lit before to the new one's
//   (kSlideSecondsPerRow a row, easing in and out), passing over the rows between in their own colors;
// - through a block's last kSwellWindowSeconds the lit box (an interval's row, or a plain block's box) swells and
//   shrinks back once every kSwellPeriodSeconds, kSwellScale wider and taller at the most, and less either way where
//   that would take it up to the widgets around it (the interval's other rows, its count, the block bar, the rest of
//   the screen): it always stays kSwellClearance px clear of them, so an interval's row, flush with the rows above and
//   below it, only widens.
// Still, it draws nothing: the labels show themselves, exactly as they would without it. What it draws is the labels'
// own look (each one grabbed lit and dimmed), so their colors, text and blink come with it.
class BlockHighlight : public QWidget {
    Q_OBJECT

public:
    static constexpr double kSlideSecondsPerRow = 0.35;
    static constexpr double kSwellWindowSeconds = 10;
    static constexpr double kSwellPeriodSeconds = 1;
    static constexpr double kSwellScale = 0.025;
    // (design px)
    static constexpr double kSwellClearance = 2;

    // over area (the block's labels' box), a child of parent; it reaches a little past area for the swell
    BlockHighlight(QWidget* area, QWidget* parent);

    // the block's labels: an interval's rows top to bottom, or the plain block's one, the lit one at index (none: off).
    // A new block's labels don't slide.
    void setRows(const QList<QLabel*>& rows, int index = 0);
    // the interval's next block lit: it slides there from the one lit now
    void setActive(int index);
    // where the block being ridden is (its seconds in and seconds left): the slide's progress, and the swell; swell
    // false: the block's too short to blink, so it doesn't swell either
    void setBlockTime(double elapsed, double remaining, bool swell);

    int activeRow() const { return index_; }
    bool sliding() const { return sliding_; }
    // the lit box's row (between two while sliding) and how far it's swollen (0 to 1)
    double slidePosition() const { return position_; }
    double swell() const { return swell_; }
    // how much wider and taller the lit box is drawn now (1: its own size), and the most it grows each way (a fraction
    // of its size) at the top of a swell where it is
    QSizeF swellScale() const { return {1 + swellRoom_.width() * swell_, 1 + swellRoom_.height() * swell_}; }
    QSizeF swellRoom() const { return swellRoom_; }
    // whether it draws anything now
    bool drawing() const { return sliding_ || swell_ > 0; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void place();
    void grabRows();
    QRectF rowRect(int row) const;
    QRectF litRect() const;
    double radius(const QRectF& box) const;
    void updateDrawn(bool wasDrawing);
    QSizeF roomToSwell() const;

    QPointer<QWidget> area_;
    QList<QPointer<QLabel>> rows_;
    QList<QPixmap> lit_, dim_;
    int index_ = 0;
    double fromPosition_ = 0;
    double slideSeconds_ = 0;
    bool sliding_ = false;
    double position_ = 0;
    double swell_ = 0;
    QSizeF swellRoom_{kSwellScale, kSwellScale};
    bool stale_ = true;
    bool grabbing_ = false;
};

}  // namespace awo
