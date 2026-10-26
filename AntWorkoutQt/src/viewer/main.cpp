// LayoutViewer.exe (LayoutViewer.py): two windows. One shows the real setup, workout (frozen part way through a
// simulated ride), synopsis, song picker or workout picker screen, in the mode picked on the Scenario tab; the other
// picks which one is shown, changes its fonts, font sizes and colors live (one color list for the whole app, so a
// color changes everywhere it's used, and the power zone colors on their own tab), and previews it on other screen
// sizes (the app scales itself to fit each one, see AppLayout). No ANT stick needed: a stand-in bridge takes its
// place, and Musical Cadence Mode rides made-up songs (nothing is analyzed or played).
//
// A theme .json saved here holds the fonts and colors, and the app loads it from its title bar settings menu.
// Fonts render with the same engine the app uses (Qt 6.7's GDI), so what shows here is exactly what the app will show.
// GDI only sees each variable font's default instance (e.g. "Anybody Thin"); --directwrite browses their other
// weights, which the app can't use yet.

#include <QApplication>
#include <QDir>
#include <QScreen>
#include <QWindow>

#include <cstring>

#include "Theme.hpp"
#include "ViewerControls.hpp"
#include "ViewerDisplay.hpp"
#include "WorkoutHelpers.hpp"

using namespace awo;

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--directwrite") == 0 && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "windows:fontengine=directwrite");
        }
    }
    QApplication app(argc, argv);
    // settings, workouts and themes are all beside the exe, as the app's are
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    // (the views are built with stand-in devices picked: the app's own picks are put back on the way out)
    const QStringList selected = selectedDevices::read();

    // the system font, for buttons and the controls (the theme sets the app wide one)
    theme::setDefaultFont(QApplication::font());
    const QStringList families = theme::registerFonts();
    // starting where the app does with no theme picked
    theme::clear();

    int code = 0;
    {
        viewer::DisplayWindow display;
        viewer::ControlWindow controls(&display, families);
        controls.rebuild();

        // the display maximized on the main screen; the controls on a second screen if there is one
        const QList<QScreen*> screens = QGuiApplication::screens();
        display.setScreen(screens.first());
        // (Windows may otherwise open it on another monitor)
        display.move(screens.first()->availableGeometry().topLeft());
        controls.setHomeScreen(screens.first());
        // its native window, to hear about moves to another monitor from the start
        display.winId();
        QObject::connect(display.windowHandle(), &QWindow::screenChanged, &controls,
                         [&controls] { controls.displayScreenChanged(); });
        display.showMaximized();
        controls.resize(520, 960);
        if (screens.size() > 1) {
            controls.move(screens[1]->availableGeometry().topLeft());
        } else {
            const QRect available = screens.first()->availableGeometry();
            controls.move(available.right() - 540, available.top() + 40);
        }
        controls.show();
        code = app.exec();
    }
    if (selected.isEmpty()) {
        selectedDevices::clear();
    } else {
        selectedDevices::write(selected);
    }
    return code;
}
