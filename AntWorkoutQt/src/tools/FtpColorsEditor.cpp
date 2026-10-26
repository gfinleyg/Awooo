// FtpColorsEditor.exe: LayoutViewer's FTP Colors tab and its mockup window on their own (FtpColorsEditor.py), on the
// theme the app last used: the power zones' colors edited and seen on the parts for any FTP and power, loaded from and
// saved to a gradients file (themes\gradients, the ftpColors part of a theme).

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "AppStyle.hpp"
#include "FtpColorsEditor.hpp"
#include "SavedSettings.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

using namespace awo;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // (the theme's files, settings and fonts beside the exe, as the app's)
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    theme::setDefaultFont(QApplication::font());
    theme::setAppStyleSheet(style::kAppStyleSheet);
    const QString themeFile = settings::load().value(QStringLiteral("themeFile")).toString();
    try {
        if (!themeFile.isEmpty()) {
            theme::loadFile(themeFile);
        } else {
            theme::clear();
        }
    } catch (const theme::ThemeError&) {
        theme::clear();
    }

    QWidget window;
    window.setWindowTitle(QStringLiteral("FTP Colors"));
    theme::setExempt(&window);
    auto* layout = new QVBoxLayout(&window);
    auto* tab = new FtpColorsTab;
    layout->addWidget(tab, 1);
    auto* buttons = new QHBoxLayout;
    auto* load = new QPushButton(QStringLiteral("Load Gradients..."));
    auto* save = new QPushButton(QStringLiteral("Save Gradients..."));
    buttons->addWidget(load);
    buttons->addWidget(save);
    layout->addLayout(buttons);
    const QString folder = theme::partDir(QStringLiteral("ftpColors"));
    QObject::connect(load, &QPushButton::clicked, &window, [&window, folder] {
        const QString path = QFileDialog::getOpenFileName(&window, QStringLiteral("Load Gradients"), folder,
                                                          QStringLiteral("Gradients (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        try {
            theme::loadFile(path);
        } catch (const theme::ThemeError& e) {
            QMessageBox::warning(&window, QStringLiteral("Load Gradients"), QString::fromUtf8(e.what()));
        }
    });
    QObject::connect(save, &QPushButton::clicked, &window, [&window, folder] {
        const QString path = QFileDialog::getSaveFileName(&window, QStringLiteral("Save Gradients"), folder,
                                                          QStringLiteral("Gradients (*.json)"));
        if (!path.isEmpty()) {
            theme::saveFile(path, QStringLiteral("ftpColors"));
        }
    });
    window.resize(720, 520);
    window.show();
    FtpMockupWindow mockup;
    mockup.move(window.x() + window.width() + 20, window.y());
    mockup.show();
    QObject::connect(&app, &QApplication::lastWindowClosed, &app, &QApplication::quit);
    return app.exec();
}
