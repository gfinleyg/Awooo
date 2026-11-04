#pragma once

// A label that, while disabled, has faint diagonal lines over its box, fading out around its text so the text stays
// readable: the workout screen's interval blocks not being ridden. Enabled, it's a plain QLabel. The lines are kept in
// a pixmap, made again only when something they depend on changes (they're drawn on every frame the screen redraws).

#include <QColor>
#include <QLabel>
#include <QPixmap>

namespace awo {

class HatchedLabel : public QLabel {
    Q_OBJECT

public:
    explicit HatchedLabel(const QString& text = {}, QWidget* parent = nullptr);

    // the (unthemed) color the lines have to stand out on: the middle of the box's dimmed background
    void setHatchBackground(const QColor& color);
    // how many times the lines' pixmap has been made
    int hatchBuilds() const { return hatchBuilds_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap buildHatch(const QColor& ink, double scale) const;

    QColor background_;
    QPixmap hatch_;
    QString hatchKey_;
    int hatchBuilds_ = 0;
};

}  // namespace awo
