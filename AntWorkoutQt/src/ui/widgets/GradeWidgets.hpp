#pragma once

// What shows the block grades (ClassGradeWidgets.py, see WorkoutGrading):
//
//   GradeOverlay  "Grade" and the letter in the middle of the workout screen's spiral when a block ends, fading out;
//                 or, during an FTP test's effort, its average power (showValue)
//   GradeList     each ridden block's grade beside its usual colored block label (Musical Cadence Mode: each song's,
//                 its blocks beneath it), with the workout's average grade pinned at the bottom: the workout screen's
//                 left column once the workout is complete, and the synopsis' Block Grades view (there, every ride of
//                 the session under its own heading, see Session)
//
// Both draw themselves at the layout scale (theme::uiScale), in the theme's fonts and colors.

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPainterPath>
#include <QWidget>

#include <map>
#include <optional>
#include <tuple>

#include "FtpColors.hpp"
#include "Session.hpp"
#include "WorkoutGrading.hpp"

class QTimer;

namespace awo {

class PowerSpiral;

// font, shrunk if needed so text fits width x height
QFont fittedFont(const QFont& font, const QString& text, double width, double height);

class GradeOverlay : public QWidget {
    Q_OBJECT

public:
    static constexpr double kMaxSeconds = 5.0;
    static constexpr double kMaxFadeSeconds = 1.0;

    // covers the spiral, drawing in its hole
    explicit GradeOverlay(PowerSpiral* spiral);

    // shows the grade for kMaxSeconds, or the next block's length if that's shorter, fading out over the last of it;
    // a grade already showing is replaced
    void showGrade(const QString& grade, std::optional<double> nextBlockSeconds = std::nullopt);
    // shows title over value until cleared or replaced (no fade); the same again just redraws
    void showValue(const QString& title, const QString& value);
    void clear();
    QString grade() const { return grade_; }
    QString title() const { return title_; }
    double opacity() const { return opacity_; }
    // (center, radius) of the space inside the spiral's innermost ring
    std::pair<QPointF, double> innerCircle() const;
    // (tests) a fade step at elapsed seconds since it was shown
    void stepAt(double elapsed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void step();
    void drawOutlined(QPainter& painter, const QString& text, const QRectF& rect, const QColor& fill, double points);
    static std::pair<QPainterPath, double> textShape(const QString& text, const QRectF& rect, double points,
                                                     const QString& family);

    PowerSpiral* spiral_;
    QString grade_;
    QString title_ = QStringLiteral("Grade");
    // a value's color (showValue); a grade is colored by its letter
    std::optional<QColor> valueColor_;
    QElapsedTimer shownAt_;
    double showSeconds_ = 0.0;
    double opacity_ = 0.0;
    // the drawn text's (path, outline width), by what it depends on
    std::map<std::tuple<QString, double, double, double, double, double, QString>, std::pair<QPainterPath, double>>
        shapes_;
    QTimer* timer_;
};

class GradeList : public QWidget {
    Q_OBJECT

public:
    static constexpr double kRowHeight = 42;
    static constexpr double kRowGap = 4;
    static constexpr double kBadgeWidth = 78;
    static constexpr double kIndent = 18;
    static constexpr double kAverageHeight = 64;
    static constexpr double kLabelPoints = 14;

    // a row as shown: a grade row, a ride's heading (the synopsis' session) or "more..."
    struct Row {
        enum class Kind { Block, Interval, Sub, Song, Ride, More };
        Kind kind = Kind::Block;
        QString label;
        std::optional<double> pct;
        QString grade;
        QString note;
        bool indent = false;
        bool graded = true;
        // the zones a ride's blocks were ridden in (none: the app's)
        std::optional<ftpcolors::ZoneRanges> ranges;
    };

    explicit GradeList(QWidget* parent = nullptr);

    // these grades (AppState's blockGrades by default), in the app's workout order
    void refresh();
    void refresh(const grading::Grades& grades);
    // the session's rides: each under a heading with its own average, the graded ones' blocks in the zones they were
    // ridden in, and the average of all of them at the bottom
    void refresh(const QList<const Ride*>& rides);

    // rows of its own (LayoutViewer, tests)
    void setRows(const QList<Row>& rows, std::optional<double> average,
                 const QString& averageText = QStringLiteral("Workout Average"));
    const QList<Row>& rows() const { return rows_; }
    std::optional<double> average() const { return average_; }
    QString averageText() const { return averageText_; }
    // the rows in view (with "more..." in the top/bottom slot when rows are out of view that way)
    QList<Row> shownRows();
    int scrollOffset() const { return scrollOffset_; }
    void scrollTo(int offset);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    static double scaled(double value);
    double rowStep() const;
    double listHeight() const;
    void drawLabel(QPainter& painter, const QRectF& rect, const QString& text, const QFont& font, const QColor& start,
                   const QColor& end, const QString& note = {}, const std::optional<QColor>& textColor = {});
    void drawAverage(QPainter& painter);

    QList<Row> rows_;
    std::optional<double> average_;
    QString averageText_ = QStringLiteral("Workout Average");
    int scrollOffset_ = 0;
    int wheelRemainder_ = 0;
    std::optional<double> dragStartY_;
    int dragStartOffset_ = 0;
};

}  // namespace awo
