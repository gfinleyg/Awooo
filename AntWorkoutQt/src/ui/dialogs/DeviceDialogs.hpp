#pragma once

// The setup screen's device popups (ClassSelectDevicesWindow.py, ClassConnectDevicesWindow.py):
//
//   SelectDevicesWindow   a button for each device of a kind (power, heart rate, or speed/cadence) found or picked
//                         before; clicking one picks it (or unpicks it), and the others it would clash with are
//                         greyed out. A choice still to make blinks
//   ConnectDevicesWindow  what's picked, what's been found, and anything usable left unpicked, before connecting

#include <QHash>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QLabel;
class QPushButton;
class QTimer;

namespace awo {

namespace ant {
class AntBridge;
}

class SelectDevicesWindow : public QWidget {
    Q_OBJECT

public:
    enum class Kind { Power, HeartRate, SpeedCadence };

    // devices: "<type> <number>" lines of this kind (found and picked); found: everything the scan has heard
    SelectDevicesWindow(const QStringList& devices, Kind kind, const QStringList& found);

    const QList<QPushButton*>& buttons() const { return buttons_; }
    QPushButton* keepButton() const { return keep_; }
    bool isPicked(QPushButton* button) const { return pressed_.value(button); }
    bool blinking() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buttonClicked(QAbstractButton* button);
    void blinkCandidates();
    void stopBlinking();

    QStringList devices_;
    QList<QPushButton*> buttons_;
    QHash<QPushButton*, bool> pressed_;
    QButtonGroup* group_;
    QButtonGroup* speedGroup_;
    QButtonGroup* cadenceGroup_;
    QPushButton* keep_;
    // (picking the devices picked before, as it opens, doesn't save them again)
    bool initialized_ = false;
    bool blinkOn_ = false;
    QTimer* blinkTimer_;
};

class ConnectDevicesWindow : public QWidget {
    Q_OBJECT

public:
    explicit ConnectDevicesWindow(ant::AntBridge* bridge);

    QString message() const;
    QPushButton* connectButton() const { return connect_; }
    QPushButton* reselectButton() const { return reselect_; }

    // what it says about the picked and found devices
    static QString connectingMessage(const QStringList& selected, const QStringList& found);

private:
    void connectDevices();

    ant::AntBridge* bridge_;
    QLabel* label_;
    QPushButton* connect_;
    QPushButton* reselect_;
};

}  // namespace awo
