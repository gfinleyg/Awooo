#pragma once

// The synopsis (ClassSynopsisWindow.py): the session's rides on one graph, one after the other (a divider where each
// after the first starts, their headings along the top with more than one, Musical Cadence Mode's songs under
// them), their target bars and every sensor's readings as recorded, a star and label on each sensor's max, and beside
// it the stats (distance, time, averages, best sustained power), the grade or an FTP test's result, From/To time
// boxes, Block Grades, Apply FTP and Set Max HR. Zooming and panning move only time, within the session; the stats,
// the maxes and the time boxes follow what's in view. Clicking a curve (or its label) highlights it; clicking a
// graded block opens its grade, Left/Right step through them.
//
// The ride just ridden joins the session (session::addCurrentRide) before this is built.

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

#include "Session.hpp"
#include "SynopsisItems.hpp"
#include "ViewBox.hpp"

class QFrame;
class QGraphicsLineItem;
class QGraphicsRectItem;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimeEdit;
class QTimer;
class QVBoxLayout;

namespace awo {

namespace ant {
class AntBridge;
}

namespace plot {
class CurveItem;
class SolidColoredColumns;
}  // namespace plot

class GradeList;
class MaxStar;
class SetupScreen;
class WorkoutGraph;

class SynopsisScreen : public QWidget {
    Q_OBJECT

public:
    // best sustained power durations shown with the stats, in seconds
    static const QList<int> kBestPowerSeconds;
    // the narrowest time span the graph zooms in to, in seconds
    static constexpr double kMinZoomSeconds = 30;
    // the marker and label border of a max that's only the max of the zoomed in selection
    static const QString kSelectionColor;
    // the dashed line where each of the session's rides after the first starts
    static const QColor kDividerColor;

    using Clock = std::function<double()>;

    // bridge: for the low battery warnings; setup: Apply FTP and Set Max HR set it there; clock: the stars' (seconds)
    SynopsisScreen(ant::AntBridge* bridge, SetupScreen* setup, Clock clock = {}, QWidget* parent = nullptr);
    ~SynopsisScreen() override;

    // a sensor's max: its marker, label and star
    struct MaxMarker {
        PlotLabel* label;
        QGraphicsLineItem* line;
        MaxStar* star;
        plot::ViewBox* vbox;
        const std::pair<QList<double>, QList<double>>* vals;
        std::function<QString(double)> format;
        double fullX, fullY;  // the session's max
        double x, y;          // the max shown (the selection's when the session's is out of view)
        bool shown = true;
    };

    // ---- What's on it (tests, LayoutViewer) ----

    WorkoutGraph* graph() const { return graph_; }
    double totalSeconds() const { return totalSeconds_; }
    plot::Range viewRange() const;
    QLabel* statsLabel() const { return stats_; }
    QString statsText(double start, double end) const;
    const QList<MaxMarker>& maxMarkers() const { return maxMarkers_; }
    BandStrip* rideStrip() const { return rideStrip_; }
    BandStrip* songStrip() const { return songStrip_; }
    const QList<GradeRegion*>& gradeRegions() const { return gradeRegions_; }
    QFrame* gradePopup() const;
    GradeRegion* gradePopupRegion() const { return gradePopupRegion_; }
    QLabel* gradeText() const { return workoutGradeText_; }
    QLabel* ftpTestText() const { return ftpTestText_; }
    QPushButton* blockGradesButton() const { return blockGradesButton_; }
    QPushButton* applyFtpButton() const { return applyFtpButton_; }
    QPushButton* setMaxHrButton() const { return setMaxHrButton_; }
    QPushButton* restartButton() const { return restartButton_; }
    QPushButton* exitButton() const { return exitButton_; }
    QTimeEdit* startTimeEdit() const { return startTimeEdit_; }
    QTimeEdit* endTimeEdit() const { return endTimeEdit_; }
    GradeList* blockGradeList() const { return blockGradeList_; }
    plot::CurveItem* selectedCurve() const { return selectedCurve_; }
    const QList<plot::CurveItem*>& sensorCurves() const { return sensorCurves_; }
    std::optional<int> newFtp() const { return newFtp_; }
    const std::pair<QList<double>, QList<double>>& powerValues() const { return processedPowerVals_; }
    const std::pair<QList<double>, QList<double>>& cadenceValues() const { return processedCadenceVals_; }
    const QList<double>& bestPowers() const { return bestPowerRange_; }
    // the session's rides shown, oldest first
    qsizetype rideCount() const { return rides_.size(); }
    // (an FTP effort test among them) the best power keeps its average through dropouts
    bool dropoutsKeepAverage() const { return !keepAverageSpans_.isEmpty(); }

    // the max markers, stats, time boxes and labels for what's in view (the graph's range settled)
    void updateSelection();
    // a ride's or song's stretch, or back out to the whole session if it's showing just that
    void zoomTo(double start, double end);
    void selectCurve(plot::CurveItem* curve);
    void openGradePopup(GradeRegion* region);
    void closeGradePopup();
    // Left/Right: the next/previous graded block's grade
    void stepGrade(int step);
    void showBlockGrades(bool show);
    bool blockGradesShown() const;
    // a block's grade, as its popup details it
    QString gradeDetailsHtml(const grading::Grade& record, const Ride& ride) const;
    // the max labels each in the free spot nearest its star
    void layoutMaxLabels();
    // (LayoutViewer, showing a screen size of its own) the window isn't maximized again a second after it's built
    void keepWindowSize() { keepWindowSize_ = true; }
    // the stars a step on
    void animateStars();
    // the ride a grade is from
    const Ride& rideOf(int rideIndex) const { return rides_[rideIndex]; }

    // Esc: closes the Block Grades sheet, else an open grade; false if neither was open
    Q_INVOKABLE bool escapePressed();

signals:
    // Return to Setup, Exit
    void returnToSetupRequested();
    void exitRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct TargetBar {
        int ride;
        plot::SolidColoredColumns* plot;
        plot::ViewBox* vbox;
        QList<double> xs, ys;
    };
    // what the max labels keep clear of: (columns, view, xs, ys); curves as drawn, dropout zeros included
    struct PlotSeries {
        bool columns;
        plot::ViewBox* vbox;
        QList<double> xs, ys;
    };

    void processSynopsisData();
    void processRide(int index);
    void processSpeedReading(double seconds, int revolutions, int eventTime);
    void processCadenceReading(double seconds, int revolutions, int eventTime);
    void addCadenceValue(double seconds, double cadence);
    std::optional<double> findBestPowerRange(double targetTime, const QList<double>* times = nullptr,
                                             const QList<double>* watts = nullptr) const;

    void setupFtpTestResult();
    QString ftpTestTextNow() const;
    void addMaxMarker(plot::ViewBox* vbox, const std::pair<QList<double>, QList<double>>& vals, qsizetype index,
                      double value, std::function<QString(double)> format, plot::CurveItem* curve);
    void addStrip(BandStrip* strip);
    void layoutStrips();
    void setupGradeRegions();
    void gradeRegionClicked(GradeRegion* region);
    void placeGradePopup();
    void scrollTo(const QRectF& rect);
    void layoutBlockGrades();
    bool isZoomed(double start, double end) const;
    void setTickSpacing(double span);
    void syncTimeEdits();
    void timeEditChanged(QTimeEdit* edited);
    void updateMaxMarkers(double start, double end);
    void fitStarRoom();
    bool rectHitsData(const QRectF& rect) const;
    void plotClicked(Qt::MouseButton button, bool taken);
    void matchTimeLabelWidths();
    void refreshFtpColors();
    void unitsChanged();
    void retheme();
    void applyFtp();
    void setMaxHr();
    void updateBatteryWarning();
    void fixSize();
    bool keepWindowSize_ = false;
    bool cadenceRide(const Ride& ride) const { return ride.kind == RideKind::Cadence; }
    plot::ViewBox* targetVBox(const Ride& ride) const;
    static double graphOffset(const Ride& ride) { return ride.start + ride.rideOffset; }
    static std::optional<QString> songDuring(const Ride& ride, double start, double end);

    ant::AntBridge* bridge_;
    SetupScreen* setup_;
    Clock clock_;
    QList<Ride> rides_;
    QList<double> rideStarts_;
    bool showHr_ = false, showSpeed_ = false, showCadence_ = false;
    QString speedUnit_;
    double endTime_ = 0;
    double totalSeconds_ = 0;
    double powerTop_ = 0, hrCadenceTop_ = 0;
    QList<std::pair<double, double>> keepAverageSpans_;

    // the readings replayed: (session seconds, value) each
    std::pair<QList<double>, QList<double>> processedPowerVals_, processedHrVals_, processedCadenceVals_,
        processedSpeedVals_;
    // (seconds, meters) of each accepted speed reading, for a selection's distance
    std::pair<QList<double>, QList<double>> speedDistance_;
    double totalMeters_ = 0;
    std::optional<std::pair<int, int>> speedLast_, cadenceLast_;
    double wheelCircumference_ = 0;
    double lastNonZeroSpeed_ = 0, lastNonZeroCadence_ = 0;
    int lastNonZeroSpeedIndex_ = 0, lastNonZeroCadenceIndex_ = 0;
    QList<double> bestPowerRange_;

    WorkoutGraph* graph_;
    QFrame* graphFrame_;
    QList<TargetBar> targetBars_;
    plot::CurveItem* powerPlot_ = nullptr;
    plot::CurveItem* hrPlot_ = nullptr;
    plot::CurveItem* cadencePlot_ = nullptr;
    plot::CurveItem* speedPlot_ = nullptr;
    QList<plot::CurveItem*> sensorCurves_;
    QHash<plot::CurveItem*, std::pair<QPen, std::optional<QPen>>> curvePens_;
    plot::CurveItem* selectedCurve_ = nullptr;
    QList<PlotSeries> plotSeries_;
    int speedSeriesIndex_ = -1;
    QList<std::pair<QGraphicsItem*, QPen>> themedPens_;
    QList<BandStrip*> strips_;
    BandStrip* rideStrip_ = nullptr;
    BandStrip* songStrip_ = nullptr;
    QList<MaxMarker> maxMarkers_;

    QLabel* stats_;
    QLabel* batteryWarning_;
    QVBoxLayout* statsLayout_;
    QString ftpTestMode_;
    QList<int> gradedRides_;
    bool grading_ = false;
    QList<std::pair<GradeRegionKey, const grading::Grade*>> blockGrades_;
    QLabel* workoutGradeBadge_ = nullptr;
    QLabel* workoutGradeText_ = nullptr;
    int previousFtp_ = 0;
    std::optional<int> newFtp_;
    QString ftpTestReason_;
    QLabel* ftpTestText_ = nullptr;
    QTimeEdit* startTimeEdit_;
    QTimeEdit* endTimeEdit_;
    QList<QLabel*> timeLabels_;
    QPushButton* blockGradesButton_ = nullptr;
    QPushButton* applyFtpButton_ = nullptr;
    QPushButton* setMaxHrButton_;
    int workoutMaxHr_ = 0;
    QPushButton* restartButton_;
    QPushButton* exitButton_;

    QPointer<QFrame> gradePopup_;
    GradeRegion* gradePopupRegion_ = nullptr;
    QList<GradeRegion*> gradeRegions_;
    QHash<plot::ViewBox*, QGraphicsRectItem*> gradeHighlights_;
    QFrame* blockGradesOverlay_ = nullptr;
    QFrame* blockGradesSheet_ = nullptr;
    GradeList* blockGradeList_ = nullptr;

    QTimer* selectionTimer_;
    QTimer* starTimer_;
};

}  // namespace awo
