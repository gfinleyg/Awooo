#pragma once

// A column of colored block labels that fits the widget's height (ClassLongVerticalLabelSeries.py), scrollable by
// wheel or drag, with a "more..." block at the top or bottom while blocks are scrolled out of view that way: the
// workout screen's coming up list and the setup screen's workout preview.
//
// An interval ("3x <block>" and its other blocks' lines indented under it) shows as its blocks' rows with a cap down
// their left side holding the repeat count, colored top to bottom through the blocks' colors; the list scrolls by
// whole intervals (and blocks), so one is never cut in two by the edge.

#include <QWidget>

#include <optional>

#include "FtpColors.hpp"
#include "SpiralSlide.hpp"

class QLabel;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;

namespace awo {

class LongVLabels : public QWidget {
    Q_OBJECT

public:
    static constexpr int kWidgetCount = 30;
    // a block's width (design px): room for the longest block text there can be, "1000x 1 Leg ⇄ 2400-2400W 29m 59s",
    // in the labels' font (the default theme's; a wider font is made smaller to fit)
    static constexpr int kLabelWidth = 330;
    // an interval's cap (design px): its width, and the gap between it and its rows (which make room for both)
    static constexpr int kCapWidth = 46;
    static constexpr int kCapGap = 3;
    // its left corners, rounded as the blocks' are (the app's sheet)
    static constexpr int kCapRadius = 15;
    // a cap's sheet, for its blocks' colors top to bottom (the Up Next box's too)
    static QString capSheet(const QList<std::pair<QString, QString>>& blockColors);
    // what an interval's rows add to their sheets: room for its cap, and their left corners square facing it
    static QString capRoomCss();

    explicit LongVLabels(const ftpcolors::ZoneRanges& ftpRanges = {}, QWidget* parent = nullptr);

    // the workout screen's coming up list and the setup screen's preview: blocks labelWidth wide (design px) from the
    // top down, so the rest stay where they are as one finishes, each one's text centered, and scrolled across it when
    // it's too long (ScrollingLabel)
    void setPinnedStyle(int labelWidth = kLabelWidth);

    // the blocks, a line each (an empty list shows none); "DONE" holds the list's size
    void setFullText(const QString& text);
    QString fullText() const { return fullText_; }
    // the zones the blocks are colored in
    ftpcolors::ZoneRanges ftpRanges;
    const QList<QLabel*>& labels() const { return labels_; }
    // the space between rows, at the layout scale
    int rowGap() const;
    // the intervals' caps (the ones in use shown)
    const QList<QLabel*>& caps() const { return caps_; }
    // how many blocks and intervals are scrolled out of view above
    int scrollOffset() const { return scrollOffset_; }
    void scrollTo(int offset);
    // the lines shown now (the queued layout done)
    void renderLines();

    // the workout screen's coming up list: when a block finishes, the rest each trace a small spiral and then slide up
    // to their new places (spiralslide::kBlockDone), the one that's left the list going on up into the Up Next box
    // (HudScreen); when the list scrolls they glide there (spiralslide::kScroll). Off by default: the setup screen's
    // preview jumps
    void setSliding(bool on) { sliding_ = on; }
    bool sliding() const { return sliding_; }
    // a slide under way (or held), and the rows put straight where they go
    bool slideRunning() const;
    void finishSlide();
    // the slide timed from outside (HudScreen: by the workout clock, so it waits while the ride's paused): held at
    // time ms into it, a slide that starts later starting there too; none: it runs on by itself from where it is
    void holdSlide(std::optional<int> time);

    QSize minimumSizeHint() const override;

signals:
    void textChanged(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // a block's line, or an interval's lines (its repeat count taken off the first, the indent off the others)
    struct Item {
        QStringList lines;
        int repeats = 0;
    };
    // a cap shown: over which rows, for which interval
    struct Cap {
        int first = 0;
        int last = 0;
        int item = 0;
    };

    double rowHeight() const;
    QString labelColor(const QString& text) const;
    QList<Item> items() const;
    // the caps over their rows, wherever the layout has put them
    void placeCaps();
    // how many rows the new ones moved by from the old (positive: up), going the way direction says (0: none found)
    static int rowShift(const QStringList& before, const QStringList& after, int direction);
    // the rows shown from rows rows lower (negative: higher), step px apart, to where they go, from time ms in
    void startSlide(int rows, double step, const spiralslide::Motion& motion, int time = 0);
    void placeSlide(int time);

    QString fullText_;
    QList<QLabel*> labels_;
    QList<QLabel*> caps_;
    QList<Cap> shownCaps_;
    QVBoxLayout* labelLayout_;
    int scrollOffset_ = 0;
    int wheelRemainder_ = 0;
    std::optional<double> dragStartY_;
    int dragStartOffset_ = 0;
    // the lines are laid out once the widget has its size for the new text (and again on each resize): a render
    // queued for the event loop, however many changes come in before it runs
    QTimer* renderTimer_;
    // sliding: the rows' text as last shown, the way the next render's rows are expected to move (1: up, a block done
    // or scrolled down; -1: down) and how, and the rows' places as laid out, which a slide starts away from and ends at
    bool sliding_ = false;
    QStringList shownRows_;
    int slideDirection_ = 0;
    spiralslide::Motion slideMotion_;
    QVariantAnimation* slide_;
    QList<QPoint> slideHomes_;
    int slideRows_ = 0;
    double slideStep_ = 0.0;
    std::optional<int> slideHold_;
};

}  // namespace awo
