#pragma once

// Calibrating the connected power meter (ClassCalibratePowerWindow.py), step by step:
//   1. ANT+ manual zero offset: the meter's own calibration, sent over ANT+
//   2. At-rest check: the meter's reading with no load becomes an offset
//   3. Accuracy check (optional): a steady ride compared with a reference wattage gives a scale factor
//   4. Review and save: kept per meter (PowerCalibration), applied to every power reading in workouts

#include <QElapsedTimer>
#include <QWidget>

#include <optional>

#include "PowerCalibration.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace awo {

namespace ant {
class AntBridge;
}

class CalibratePowerWindow : public QWidget {
    Q_OBJECT

public:
    static constexpr int kRestMeasureSeconds = 5;
    static constexpr int kRideMeasureSeconds = 30;

    explicit CalibratePowerWindow(ant::AntBridge* bridge);

    void goToPage(int index);
    int page() const;
    QPushButton* nextButton() const { return next_; }
    QPushButton* backButton() const { return back_; }
    QPushButton* sendZeroButton() const { return sendZero_; }
    QString liveText() const;
    QString zeroStatus() const;
    QString reviewText() const;
    double offsetWatts() const { return offsetWatts_; }
    double scale() const { return scale_; }
    // (tests) a measurement over these readings, as if its time had run out
    void finishMeasureWith(const QString& kind, const QList<int>& samples);
    void setReference(const QString& text);
    void applyScale();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* pageWith(const QString& text, QLabel** label = nullptr);
    QLabel* statusLabel(QWidget* page);
    QWidget* buildIntroPage();
    QWidget* buildZeroOffsetPage();
    QWidget* buildRestPage();
    QWidget* buildRidePage();
    QWidget* buildReviewPage();
    void nextPage();
    void sendZeroOffset();
    void onCalibrationUpdate(int deviceNumber, const QString& result, const QVariant& offset, int attempts);
    void pollPower();
    void startMeasure(const QString& kind);
    void stopMeasure();
    void finishMeasure();
    void skipRide();
    void refreshReview();
    void save();
    void clearSaved();

    ant::AntBridge* bridge_;
    std::optional<int> deviceType_, deviceNumber_;
    std::optional<calibration::Calibration> saved_;
    // what the steps found, applied on Save
    double offsetWatts_ = 0.0;
    double scale_ = 1.0;
    std::optional<int> antZeroOffset_;
    // a measurement under way: "rest", "ride" or none
    QList<int> samples_;
    QString measuring_;
    QElapsedTimer measureClock_;
    double measureSeconds_ = 0;
    std::optional<quint64> lastVersion_;
    std::optional<double> rideMeasuredWatts_;

    QLabel* live_;
    QStackedWidget* pages_;
    QPushButton* cancel_;
    QPushButton* back_;
    QPushButton* next_;
    QPushButton* sendZero_ = nullptr;
    QLabel* zeroStatus_ = nullptr;
    QPushButton* measureRest_ = nullptr;
    QLabel* restStatus_ = nullptr;
    QLineEdit* reference_ = nullptr;
    QPushButton* measureRide_ = nullptr;
    QPushButton* applyScale_ = nullptr;
    QPushButton* skipRide_ = nullptr;
    QLabel* rideStatus_ = nullptr;
    QLabel* review_ = nullptr;
    QPushButton* clear_ = nullptr;
    QLabel* reviewStatus_ = nullptr;
    QTimer* timer_;
};

}  // namespace awo
