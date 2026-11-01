#pragma once

// A label whose lines too long for it (a song's title, mostly) slowly scroll across it and back, pausing at each end,
// rather than being made smaller to fit: the workout screen's Up Next box and coming up blocks. A line that fits is
// drawn as a QLabel draws it. Until setScrolling is on it's a plain QLabel.

#include <QElapsedTimer>
#include <QLabel>

class QTimer;

namespace awo {

class ScrollingLabel : public QLabel {
    Q_OBJECT

public:
    // design px a second, and how long each end shows
    static constexpr double kSpeed = 30;
    static constexpr double kPauseSeconds = 1.5;
    static constexpr int kStepMs = 33;

    explicit ScrollingLabel(const QString& text = {}, QWidget* parent = nullptr);

    // long lines scroll (and the text is no longer made smaller to fit, see layout::setOwnFit)
    void setScrolling(bool on);
    bool scrolling() const { return scrolling_; }
    // whether any line is wider than the label has room for
    bool overflows() const;
    // how far a line overflow px too wide is scrolled, seconds after its text was set: at its start for
    // kPauseSeconds, along to its end at speed px a second, kPauseSeconds there, and back
    static double offsetAt(double seconds, double overflow, double speed);
    // how far the line is scrolled now (0 for one that fits)
    double lineOffset(int line) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    // where QLabel draws the text, in the label
    QRect textArea() const;
    void step();
    void updateTimer();

    bool scrolling_ = false;
    QTimer* timer_;
    // since the text shown now was set (each new text starts at its beginning)
    QElapsedTimer since_;
    QString scrolledText_;
};

}  // namespace awo
