// AntWorkout.exe (AntWorkout.py)

#include <QDir>
#include <QJsonObject>

#include "AppStyle.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "MainWindow.hpp"
#include "SavedSettings.hpp"
#include "SingleInstance.hpp"
#include "SoundEffects.hpp"
#include "Theme.hpp"
#include "TimedApplication.hpp"
#include "Units.hpp"

using namespace awo;

int main(int argc, char* argv[]) {
    TimedApplication app(argc, argv);
    // settings, logs and workouts live next to the exe, wherever it was started from
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    // Qt's warnings, uncaught exceptions and crashes go to the log: the app runs without a console
    log::installErrorLogging();

    // one copy at a time, before anything goes near the stick
    SingleInstance instance;
    const SingleInstance::Start started = instance.start();
    if (started == SingleInstance::Start::ShownOther) {
        log::event(QStringLiteral("APP"), QStringLiteral("AntWorkout is already running, showing it instead of starting again"));
        return 0;
    }
    if (started == SingleInstance::Start::TimedOut) {
        log::event(QStringLiteral("APP"), QStringLiteral("Another AntWorkout didn't finish closing, starting anyway"));
    }

    // a key for every button, Ctrl shows them
    keys::install(&app);
    // the button clicks, screen changes and workout complete sounds
    sound::init(&app);

    // the system default font, before the theme replaces the app wide one: buttons keep it (the 'Orbitron' and
    // 'Segoe UI Black' the stylesheets name only mark which of the theme's fonts to use)
    theme::setDefaultFont(QApplication::font());
    theme::setAppStyleSheet(style::kAppStyleSheet);
    // the theme loaded last time, otherwise the default theme (if there is one)
    const QJsonObject saved = settings::load();
    const QString themeFile = saved.value(QStringLiteral("themeFile")).toString();
    try {
        if (!themeFile.isEmpty()) {
            theme::loadFile(themeFile);
        } else {
            theme::clear();
        }
    } catch (const theme::ThemeError& e) {
        log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load theme %1: %2").arg(themeFile, QString::fromUtf8(e.what())));
        theme::clear();
    }
    // then the colors/gradients files loaded on top of it last time
    const QJsonObject parts = saved.value(QStringLiteral("themePartFiles")).toObject();
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        const QString path = it.value().toString();
        try {
            theme::loadFile(path);
        } catch (const theme::ThemeError& e) {
            log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load %1: %2").arg(path, QString::fromUtf8(e.what())));
        }
    }
    // or, with Randomize Theme at Startup on, random ones (not saved: the picks above come back once it's off)
    if (saved.value(QStringLiteral("randomTheme")).toBool()) {
        theme::loadRandomParts();
    }
    Units::current().load();

    MainWindow window;
    window.show();
    instance.answer([&window] {
        if (window.isMinimized()) {
            window.showMaximized();
        }
        window.raise();
        window.activateWindow();
    });
    const int code = app.exec();
    sound::shutdown();
    return code;
}
