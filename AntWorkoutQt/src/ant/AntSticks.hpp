#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <tuple>

namespace awo::ant {

// One ANT USB stick in the USB device list
struct Stick {
    std::uint32_t index{};  // the device number antnative opens it by
    std::uint32_t vid{};
    std::uint32_t pid{};
    std::uint32_t serialNumber{};
    QString product;  // the USB product description, empty when it has none

    // The stick across replugs, where its index can change: a stick whose identity wasn't in the last list is newly
    // plugged in (the bridge then tries to open it again, as the Python bridge's _stick_ids() does)
    auto identity() const { return std::tuple(vid, pid, serialNumber); }
};

// Every ANT USB stick attached right now; empty when there are none or the USB list can't be read
QList<Stick> attachedSticks();

// "0: ANT USBStick2 (serial 1234567)", for the log and the setup screen
QString describe(const Stick& stick);

}  // namespace awo::ant
