#pragma once

// The setup screen (ClassWorkoutSetupWindow.py): the ANT devices (scanned, picked, connected), the rider's settings
// (wheel circumference, FTP, max heart rate, weight), the mode (free ride, the FTP tests, Musical Cadence Mode, a
// workout file) and the workout picked, and START. What still needs doing blinks.
//
// It follows the bridge's signals (a device found, connected or disconnected, a status or error) and checks again every
// 2 s in case one is missed. A slot with just one device found is picked by itself once the scan has listened long
// enough, and once every slot the workout needs has its device, it connects.

#include <QPointer>
#include <QSizePolicy>
#include <QWidget>

#include <functional>
#include <optional>

#include "CadenceWorkout.hpp"
#include "MusicSelectDialog.hpp"
#include "WorkoutTypes.hpp"

class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace awo {

namespace ant {
class AntBridge;
}

class LongVLabels;
struct AwoResult;

// lowest Max Watts the FE-C setting takes (the top of the free mode Watts slider)
constexpr int kMinMaxWatts = 200;
// lowest Power Drive Cutoff the FE-C setting takes, and its default as a share of FTP
constexpr int kMinPowerDriveCutoff = 50;
constexpr double kPowerDriveCutoffFtp = 1.2;

class SetupScreen : public QWidget {
    Q_OBJECT

public:
    explicit SetupScreen(ant::AntBridge* bridge, QWidget* parent = nullptr);

    // the ride's screens up / back from them: the bridge's goings on aren't followed meanwhile
    void stop();
    void resume();
    // back from a ride: scanning again
    void backFromRide();
    void checkEnableStartButton();
    void updateClearSessionButton();
    // an FTP test's new FTP and a workout's max heart rate, from the synopsis: as if typed in and Set
    void applyFtp(int ftp);
    void applyMaxHr(int maxHr);
    // the mode buttons' modes: "free", "20m", "1h", "ramp10", "ramp20", "cadence", "workout"
    void chooseMode(const QString& mode);
    // a workout file loaded (as the picker does it); false when it was rejected
    bool loadWorkoutFile(const QString& path);
    // the workout picker's pick: loaded as the workout to ride, or the reason it can't be
    void pickWorkoutFile(const QString& path);
    void initAntScanConnect();
    // Musical Cadence Mode's songs (as the song picker gives them; none: last time's, if they're still cached), and
    // the ride built from them
    void setMusicSelection(const std::optional<MusicSelection>& selection);
    const std::optional<MusicSelection>& musicSelection() const { return musicSelection_; }

    // ---- LayoutViewer ----

    // the rider's settings still unset as these, shown (not saved)
    void useSettings(double wheelCircumference, int ftp, int maxHr);
    // every blinking control in its normal (not blinked) look
    void settleBlink();
    // Musical Cadence Mode's ride built by this instead of from the songs (none: from the songs; with one, no songs
    // need picking)
    using MusicBuilder = std::function<music::CadenceRide(const MusicSelection&)>;
    void setMusicBuilder(MusicBuilder builder) { musicBuilder_ = std::move(builder); }

    // (tests, LayoutViewer)
    QPushButton* powerButton() const { return powerButton_; }
    QPushButton* hrmButton() const { return hrmButton_; }
    QPushButton* speedCadenceButton() const { return speedCadenceButton_; }
    QPushButton* connectButton() const { return connectButton_; }
    QPushButton* restartScanButton() const { return restartScanButton_; }
    QPushButton* calibratePowerButton() const { return calibratePowerButton_; }
    QPushButton* clearSessionButton() const { return clearSessionButton_; }
    QPushButton* startButton() const { return startButton_; }
    QPushButton* workoutFileButton() const { return workoutFileButton_; }
    QPushButton* modeButton(const QString& mode) const;
    QLineEdit* wheelField() const { return textWheelC_; }
    QLineEdit* ftpField() const { return textFtp_; }
    QLineEdit* maxHrField() const { return textMaxHr_; }
    QLineEdit* weightField() const { return textWeight_; }
    QLineEdit* maxWattsField() const { return textMaxWatts_; }
    QPushButton* setWheelButton() const { return setWheelC_; }
    QPushButton* setFtpButton() const { return setFtp_; }
    QPushButton* setMaxHrButton() const { return setMaxHr_; }
    QPushButton* setWeightButton() const { return setWeight_; }
    QLabel* wheelDisplay() const { return displayWheelC_; }
    QLabel* ftpDisplay() const { return displayFtp_; }
    QLabel* maxHrDisplay() const { return displayMaxHr_; }
    QLabel* weightDisplay() const { return displayWeight_; }
    QLabel* statusDisplay() const { return initStatusDisplay_; }
    QLabel* workoutFileLabel() const { return workoutFileSelected_; }
    QLabel* workoutInfoLabel() const { return workoutInfo_; }
    QLabel* selectedPowerLabel() const { return selectedPowerLabel_; }
    QLabel* selectedHrmLabel() const { return selectedHrmLabel_; }
    QLabel* maxWattsLabel() const { return maxWattsLabel_; }
    LongVLabels* workoutDisplay() const { return workoutDisplayLabel_; }
    QWidget* initializationWidget() const { return initializationWidget_; }
    QWidget* popup() const { return popup_; }
    QStringList initStatusLines() const { return initStatusLines_; }
    bool blinkingStart() const { return startButtonBlink_; }
    bool blinkingConnect() const { return connectButtonBlink_; }
    bool blinkingPower() const { return powerButtonBlink_; }
    bool blinkingWorkoutSelect() const { return workoutSelectBlink_; }
    void setupBlink();
    // the power and heart rate slots have their devices picked
    bool powerSelected() const { return pwrSelected_; }
    bool hrmSelected() const { return hrmSelected_; }
    // Connect Selected Devices: the connect prompt
    void connectSelectedDevices();
    // the 2 s check and the blinking
    QTimer* checkTimer() const { return timer_; }
    QTimer* blinkTimer() const { return blinkTimer_; }

signals:
    // START: the workout screen next (Session::beginRide done)
    void startRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void getFoundDevices();
    void getSelectedDevices();
    QStringList makeTextDevices(const QStringList& devices, bool selected);
    void autoSelectAndConnect();
    void settleCheck();
    void checkForSelectedScanned();
    void setInitStatus(const QString& text);
    void updateBatteryWarnings();
    void renderInitStatus();
    void rejectWorkout(const QString& filename, const QString& reason, int line = 0, const QString& text = {});
    void calibratePower();
    void confirmClearSession();
    void clearSession();
    void selectDevices(int kind);
    void disconnectAndRescan();
    void getFtpRanges();
    void textWheelValidator();
    void textFtpValidator();
    void textMaxHrValidator();
    void textWeightValidator();
    void textMaxWattsValidator();
    void updateMaxWattsMode();
    void defaultPowerDriveCutoff(bool save = false);
    void showMaxWatts(bool show);
    void setWheel();
    void setFtpValue();
    void setMaxHrValue();
    void setWeightValue();
    void showWeight();
    void reloadWorkout();
    void modeClicked();
    bool ftpOptional() const;
    bool workoutNeedsChoosing() const;
    bool workoutReady() const;
    void buildFtpTest();
    void openWorkoutDialog();
    void openMusicDialog();
    void buildMusicWorkout();
    void applyCadenceRanges();
    std::optional<MusicSelection> savedMusicSelection();
    void clearWorkout();
    bool useWorkout(const QString& filename, const AwoResult& result, bool more = false);
    void showWorkoutText(bool more = false);
    void showHudWindow();
    void openPopup(QWidget* popup);

    ant::AntBridge* bridge_;
    // the settings (false: still to be set)
    bool wheelCSet_ = false;
    bool ftpSet_ = false;
    bool maxHrSet_ = false;
    // (autoSelectAndConnect has a check queued for when the scan settles)
    bool settleCheckPending_ = false;
    bool scanning_ = true;
    bool stopped_ = false;
    // the wheel circumference as typed (or as saved)
    QString wheelText_ = QStringLiteral("0");

    QStringList detectedRaw_, selectedRaw_, detectedText_, selectedText_;
    bool pwrSelected_ = false, hrmSelected_ = false, spdCadSelected_ = false, spdSelected_ = false, cadSelected_ = false;
    bool pwrFound_ = false, hrmFound_ = false, spdCadFound_ = false, spdFound_ = false, cadFound_ = false;

    // what blinks, and where its blink is
    bool powerButtonBlink_ = false, powerButtonBlinkOn_ = false;
    bool hrButtonBlink_ = false, hrButtonBlinkOn_ = false;
    bool spdCadButtonBlink_ = false, spdCadButtonBlinkOn_ = false;
    bool connectButtonBlink_ = false, connectButtonBlinkOn_ = false;
    bool wheelCTextBlink_ = false, wheelCTextBlinkOn_ = false;
    bool wheelCButtonBlink_ = false, wheelCButtonBlinkOn_ = false;
    bool maxHrTextBlink_ = false, maxHrTextBlinkOn_ = false;
    bool maxHrButtonBlink_ = false, maxHrButtonBlinkOn_ = false;
    bool ftpTextBlink_ = false, ftpTextBlinkOn_ = false;
    bool ftpButtonBlink_ = false, ftpButtonBlinkOn_ = false;
    bool workoutSelectBlink_ = false, workoutSelectBlinkOn_ = false;
    bool startButtonBlink_ = false, startButtonBlinkOn_ = false;
    bool disconnectButtonBlink_ = false, disconnectButtonBlinkOn_ = false;

    QString btnBlinkOn_, btnBlinkOff_, tinyBtnBlinkOn_, tinyBtnBlinkOff_, txtBlinkOn_, txtBlinkOff_;
    QStringList initStatusLines_;
    QStringList batteryWarnings_;
    // the summary of why the last workout file was rejected
    QStringList workoutFileError_;

    QGridLayout* introductoryLayout_;
    QWidget* initializationWidget_;
    QWidget* centerInitWidget_;
    QPushButton* powerButton_;
    QLabel* selectedPowerLabel_;
    QLabel* maxWattsLabel_;
    QLineEdit* textMaxWatts_;
    QPushButton* hrmButton_;
    QPushButton* speedCadenceButton_;
    QLabel* selectedHrmLabel_;
    QLabel* selectedSpeedCadenceLabel_;
    QLabel* selectedCadenceLabel_;
    QLabel* selectedSpeedLabel_;
    QLabel* initStatusDisplay_;
    QPushButton* connectButton_;
    QPushButton* restartScanButton_;
    QPushButton* calibratePowerButton_;
    QPushButton* clearSessionButton_;
    QLineEdit* textWheelC_;
    QLabel* displayWheelC_;
    QPushButton* setWheelC_;
    QLineEdit* textFtp_;
    QLabel* displayFtp_;
    QPushButton* setFtp_;
    QLineEdit* textMaxHr_;
    QLabel* displayMaxHr_;
    QPushButton* setMaxHr_;
    QLineEdit* textWeight_;
    QLabel* displayWeight_;
    QPushButton* setWeight_;
    QList<std::pair<QPushButton*, QString>> modeButtons_;
    QPushButton* workoutFileButton_;
    QLabel* workoutFileSelected_;
    QLabel* workoutInfo_;
    QPushButton* startButton_;
    LongVLabels* workoutDisplayLabel_;
    QTimer* timer_;
    QTimer* blinkTimer_;
    QPointer<QWidget> popup_;
    std::optional<MusicSelection> musicSelection_;
    MusicBuilder musicBuilder_;
};

}  // namespace awo
