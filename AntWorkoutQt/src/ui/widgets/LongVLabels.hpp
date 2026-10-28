#pragma once

// A column of colored block labels that fits the widget's height (ClassLongVerticalLabelSeries.py), scrollable by
// wheel or drag, with a "more..." block at the top or bottom while blocks are scrolled out of view that way: the
// workout screen's coming up list and the setup screen's workout preview.

#include <QWidget>

#include <optional>

#include "FtpColors.hpp"

class QLabel;
class QTimer;
class QVBoxLayout;

namespace awo {

class LongVLabels : public QWidget {
    Q_OBJECT

public:
    static constexpr int kWidgetCount = 30;
    // a block's width (design px): room for the longest block text there can be, "1000x 1 Leg ⇄ 2400-2400W 29m 59s",
    // in the labels' font (the default theme's; a wider font is made smaller to fit)
    static constexpr int kLabelWidth = 330;

    explicit LongVLabels(const ftpcolors::ZoneRanges& ftpRanges = {}, QWidget* parent = nullptr);

    // the blocks, a line each (an empty list shows none); "DONE" holds the list's size
    void setFullText(const QString& text);
    QString fullText() const { return fullText_; }
    // the zones the blocks are colored in
    ftpcolors::ZoneRanges ftpRanges;
    const QList<QLabel*>& labels() const { return labels_; }
    int scrollOffset() const { return scrollOffset_; }
    void scrollTo(int offset);
    // the lines shown now (the queued layout done)
    void renderLines();

    QSize minimumSizeHint() const override;

signals:
    void textChanged(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    double rowHeight() const;
    QString labelColor(const QString& text) const;

    QString fullText_;
    QList<QLabel*> labels_;
    QVBoxLayout* labelLayout_;
    int scrollOffset_ = 0;
    int wheelRemainder_ = 0;
    std::optional<double> dragStartY_;
    int dragStartOffset_ = 0;
    // the lines are laid out once the widget has its size for the new text (and again on each resize): a render
    // queued for the event loop, however many changes come in before it runs
    QTimer* renderTimer_;
};

}  // namespace awo
