#include "AntSticks.hpp"

#include <antnative.hpp>

namespace awo::ant {

QList<Stick> attachedSticks() {
    QList<Stick> sticks;
    try {
        for (const antnative::UsbDevice& device : antnative::list_devices()) {
            Stick stick;
            stick.index = device.index;
            stick.vid = device.vid;
            stick.pid = device.pid;
            stick.serialNumber = device.serial_number;
            if (device.product_description) {
                stick.product = QString::fromStdString(*device.product_description);
            }
            sticks.append(stick);
        }
    } catch (const antnative::Error&) {
        // the USB list couldn't be read: as good as no stick, the caller looks again later
        return {};
    }
    return sticks;
}

QString describe(const Stick& stick) {
    const QString name = stick.product.isEmpty() ? QStringLiteral("ANT USB stick") : stick.product;
    return QStringLiteral("%1: %2 (serial %3)").arg(stick.index).arg(name).arg(stick.serialNumber);
}

}  // namespace awo::ant
