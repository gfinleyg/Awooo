// ColorPicker.exe (ColorPicker.py): the RGBA color picker on its own, starting at a color given on the command line.
//
//   ColorPicker [r g b a]

#include <QApplication>
#include <QStyleFactory>

#include "ColorPicker.hpp"

using awo::colorpicker::ColorPicker;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // Fusion keeps the spin boxes' digits visible; the Windows 11 style's wide arrow buttons crowd them out
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    int rgba[4] = {200, 200, 200, 130};
    if (argc >= 5) {
        for (int i = 0; i < 4; ++i) {
            rgba[i] = QString::fromLocal8Bit(argv[i + 1]).toInt();
        }
    }
    ColorPicker picker(QColor(rgba[0], rgba[1], rgba[2], rgba[3]));
    picker.show();
    return app.exec();
}

