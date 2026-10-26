// AntWorkout.exe. For now (PLAN.md Phase 0) a window listing the ANT USB sticks attached: it shows the app, Qt and
// antnative.dll running together in one process, which the Python app could only do by loading ant_native first
// (PyQt6 carried an older C++ runtime than AntNative needs; build_windows.ps1 ships a new enough one in dist\).

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QStringList>

#include "AntSticks.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Awooo Workout"));

    QStringList sticks;
    for (const awo::ant::Stick& stick : awo::ant::attachedSticks()) {
        sticks << describe(stick);
    }

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Awooo Workout"));
    auto* label = new QLabel(sticks.isEmpty() ? QStringLiteral("No ANT USB stick found")
                                              : QStringLiteral("ANT USB sticks:\n") + sticks.join(u'\n'));
    label->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(label);
    window.resize(800, 600);
    window.show();

    return app.exec();
}
