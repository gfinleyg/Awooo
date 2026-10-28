#pragma once

// A progress bar that paints its own fill so it can sit between pixels, letting the bar glide instead of stepping a
// pixel at a time (ClassSmoothProgressBar.py): setFraction() drives the smooth position; setValue()/setRange() still
// work as usual, and a hard set overrides the smooth position until the next setFraction().

#include <QColor>
#include <QProgressBar>

#include <optional>

namespace awo {

class SmoothProgressBar : public QProgressBar {
    Q_OBJECT

public:
    explicit SmoothProgressBar(QWidget* parent = nullptr);

    // the fill and the rest, in the theme's colors for them from now on
    void setBarColors(const QColor& chunkColor, const QColor& barColor);
    QColor chunkColor() const;
    QColor barColor() const;
    // the colors setBarColors was given, before the theme (none: it never was)
    std::optional<std::pair<QColor, QColor>> originalBarColors() const;
    // the smooth position, 0 - 1
    void setFraction(double fraction);
    std::optional<double> fraction() const { return fraction_; }
    // (a block change, reset, workout end)
    void setValue(int value);
    void setRange(int minimum, int maximum);
    // the bar color's border around the fill, and the bar's rounding, in pixels
    double frameWidth() const;
    double cornerRadius() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor chunkColor_{QStringLiteral("#1f15e1")};
    QColor barColor_{QStringLiteral("#bdd6de")};
    // (the constructor's colors are drawn as they are; those set through setBarColors in the theme's)
    bool themed_ = false;
    std::optional<double> fraction_;
};

}  // namespace awo
