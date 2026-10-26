#pragma once

// The power zones' colors, edited (FtpColorsEditor.py): LayoutViewer's FTP Colors tab (each zone's color and free
// ride's, as it looks fitted to the theme) and its mockup window (the power gauge, the workout block labels and the
// workout graph's bars, colored by zone for any FTP and power). FtpColorsEditor.exe shows the two on their own.

#include <QList>
#include <QPushButton>
#include <QScrollArea>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QLabel;
class QSlider;
class QSpinBox;

namespace awo {

class Gauge;

namespace ftpeditor {

// the zones' limits as % of FTP (the setup screen sets the app's the same way)
constexpr int kZoneLimits[4] = {36, 87, 119, 187};
constexpr int kDefaultFtp = 220;
// "0" - "4" and "free"
QString zoneName(const QString& zone);

}  // namespace ftpeditor

// a color button: click to pick a new color (applied live while picking)
class Swatch : public QPushButton {
    Q_OBJECT

public:
    Swatch(const QString& tip, std::function<void(const QString&)> onPick);
    void setColor(const QString& color);
    QString color() const { return color_; }

private:
    void pick();

    std::function<void(const QString&)> onPick_;
    QString color_ = QStringLiteral("#000000");
};

// two colors blended, as the part shows them
class GradientBar : public QWidget {
public:
    GradientBar();
    void setColors(const QList<QString>& colors);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<QString> colors_ = {QStringLiteral("#ffffff"), QStringLiteral("#ffffff")};
};

// each power zone's color, and free ride's
class FtpColorsTab : public QScrollArea {
    Q_OBJECT

public:
    FtpColorsTab();
    void refresh();
    void resetAll();

private:
    QPushButton* resetButton(int zone);

    QList<std::function<void()>> refreshers_;
};

// the workout graph's bars, every zone in each state
class GraphMock : public QWidget {
public:
    GraphMock();

protected:
    void paintEvent(QPaintEvent* event) override;
};

// the four parts colored by power zone, for any FTP and power
class FtpMockupWindow : public QWidget {
    Q_OBJECT

public:
    FtpMockupWindow();
    void refresh();
    QSpinBox* ftpSpin() const { return ftpSpin_; }
    QSpinBox* powerSpin() const { return powerSpin_; }
    QLabel* powerNote() const { return powerNote_; }
    const QList<QLabel*>& blockLabels() const { return blockLabels_; }
    Gauge* gauge() const { return gauge_; }

private:
    QWidget* titled(const QString& title, QWidget* widget);

    QSpinBox* ftpSpin_;
    QSlider* ftpSlider_;
    QSpinBox* powerSpin_;
    QSlider* powerSlider_;
    QLabel* powerNote_;
    QHBoxLayout* stopButtons_;
    Gauge* gauge_;
    QList<QLabel*> blockLabels_;
    GraphMock* graph_;
};

}  // namespace awo
