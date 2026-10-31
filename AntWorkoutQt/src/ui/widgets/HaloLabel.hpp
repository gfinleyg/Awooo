#pragma once

// A label over the window's gradient (the workout screen's "Up Next:" and "Coming up..." headers) whose text gets a
// halo where it doesn't stand out from the gradient behind it, as the zone legend's numbers do (ZoneLegend): light
// under dark text and dark under light, only while the contrast is under ZoneLegend::kMinContrast.

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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // the rect QLabel draws the text in
    QRect layoutRect() const;
};

}  // namespace awo
