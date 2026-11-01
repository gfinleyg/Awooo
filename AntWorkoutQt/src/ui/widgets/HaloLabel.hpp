#pragma once

// A label over the window's gradient (the workout screen's "Up Next:" and "Coming up..." headers) whose text gets a
// one pixel outline where it doesn't stand out from the gradient behind it (contrast under ZoneLegend::kMinContrast,
// as the zone legend's numbers get their halo). The outline is the gradient behind the text taken toward the lightest
// of the theme's background colors, only as far as it needs to go to be seen against the text.

#include <QLabel>

namespace awo {

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
