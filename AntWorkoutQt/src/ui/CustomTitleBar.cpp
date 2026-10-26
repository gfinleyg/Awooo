#include "CustomTitleBar.hpp"

#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QToolButton>

#include "AppLayout.hpp"
#include "Dialogs.hpp"
#include "KeyShortcuts.hpp"
#include "Logger.hpp"
#include "SavedSettings.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

const QString kMenuStyle = QStringLiteral(R"(
                QMenu {
                    color: black;
                    background-color: #bdd6de;
                    border: 2px solid #bdd6de;
                    font: 900 14pt 'Orbitron';
                }
                QMenu::item {
                    padding: 6px 24px;
                }
                QMenu::item:selected {
                    background-color: #9fc5d1;
                }
                )");

}  // namespace

std::function<void(QWidget*)>& CustomTitleBar::themePicker() {
    // (until the theme picker is built: any theme, colors or gradients file)
    static std::function<void(QWidget*)> picker = [](QWidget* parent) {
        const QString path = QFileDialog::getOpenFileName(parent, QStringLiteral("Pick Theme"), theme::themesDir(),
                                                          QStringLiteral("Theme files (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        try {
            const QString kind = theme::loadFile(path);
            if (kind == u"theme") {
                settings::update({{QStringLiteral("themeFile"), path}, {QStringLiteral("themePartFiles"), QJsonObject()}});
            } else {
                QJsonObject parts = settings::load().value(QStringLiteral("themePartFiles")).toObject();
                parts.insert(kind, path);
                settings::update({{QStringLiteral("themePartFiles"), parts}});
            }
        } catch (const theme::ThemeError& e) {
            log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load theme %1: %2").arg(path, QString::fromUtf8(e.what())));
        }
    };
    return picker;
}

CustomTitleBar::CustomTitleBar(QWidget* parent, bool mainWindow) : QWidget(parent), snaps_(mainWindow) {
    setObjectName(QStringLiteral("customTitleBar"));
    setAutoFillBackground(true);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(2);

    title_ = new QLabel(QStringLiteral("CustomTitleBar"), this);
    if (mainWindow) {
        title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        theme::setSheet(title_, QStringLiteral("font-weight: bold; font: 900 20pt 'Orbitron'; background-color:#bdd6de; "
                                               "border: 2px solid #bdd6de; border-radius: 16px;"));
    } else {
        title_->setAlignment(Qt::AlignCenter);
        theme::setSheet(title_, QStringLiteral("font: 900 20pt 'Orbitron'; background-color:#9fc5d1; "
                                               "border: 2px solid #fffffe; border-radius: 16px;"));
    }
    if (!parent->windowTitle().isEmpty()) {
        title_->setText(parent->windowTitle());
    }
    layout->addWidget(title_);

    QList<QToolButton*> buttons;
    if (mainWindow) {
        // the settings menu, only on the main window's title bar
        settingsButton_ = new QToolButton(this);
        settingsButton_->setText(QStringLiteral("⚙"));
        settingsButton_->setToolTip(QStringLiteral("Settings"));
        settingsButton_->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(settingsButton_);
        theme::setSheet(menu, kMenuStyle);
        // each item's letter (underlined, and shown on its right) picks it while the menu is open
        connect(menu->addAction(QStringLiteral("Show &Logs\tL")), &QAction::triggered, this, &CustomTitleBar::showLogs);
        connect(menu->addAction(QStringLiteral("&Clear Logs\tC")), &QAction::triggered, [] { log::clear(); });
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("Pick &Theme...\tT")), &QAction::triggered, this, &CustomTitleBar::pickTheme);
        connect(menu->addAction(QStringLiteral("Clea&r Theme\tR")), &QAction::triggered, this, &CustomTitleBar::clearTheme);
        QAction* random = menu->addAction(QStringLiteral("Ra&ndomize Theme at Startup\tN"));
        random->setCheckable(true);
        random->setChecked(settings::load().value(QStringLiteral("randomTheme")).toBool());
        connect(random, &QAction::toggled, this, &CustomTitleBar::randomTheme);
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("&Measurements...\tM")), &QAction::triggered,
                [this] { MeasurementsWindow(window()).exec(); });
        connect(menu->addAction(QStringLiteral("&Performance...\tP")), &QAction::triggered,
                [this] { PerformanceWindow(window()).exec(); });
        settingsButton_->setMenu(menu);
        // F10 opens it, Windows' key for a window's menu
        keys::Options f10;
        f10.label = QStringLiteral("F10");
        f10.action = [this] { settingsButton_->showMenu(); };
        keys::add(settingsButton_, {Qt::Key_F10}, f10);
        buttons << settingsButton_;
    }

    auto* minButton = new QToolButton(this);
    minButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMinButton));
    connect(minButton, &QToolButton::clicked, [this] { window()->showMinimized(); });
    auto* maxButton = new QToolButton(this);
    maxButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    connect(maxButton, &QToolButton::clicked, [this] { window()->showMaximized(); });
    closeButton_ = new QToolButton(this);
    closeButton_->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    connect(closeButton_, &QToolButton::clicked, [this] { window()->close(); });
    // Esc is the X on the popup windows (the main window's X asks to exit the app, so it's left off there)
    if (!mainWindow) {
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), parent->window());
        connect(escape, &QShortcut::activated, closeButton_, &QToolButton::click);
        keys::Options esc;
        esc.label = QStringLiteral("Esc");
        esc.place = keys::Place::Left;
        keys::add(closeButton_, {}, esc);
    }
    buttons << minButton << maxButton << closeButton_;
    for (QToolButton* button : buttons) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(28, 28);
        theme::setSheet(button, QStringLiteral("QToolButton { color: black; border: 1px solid %1; border-radius: 12px; }")
                                    .arg(mainWindow ? QStringLiteral("#bdd6de") : QStringLiteral("#fffffe")));
        layout->addWidget(button);
    }
}

void CustomTitleBar::setTitle(const QString& text) {
    layout::setText(title_, text);
}

void CustomTitleBar::showLogs() {
    auto* logs = new ShowLogsWindow();
    logs->setAttribute(Qt::WA_DeleteOnClose);
    logs->move(20, 20);
    logs->show();
}

void CustomTitleBar::pickTheme() {
    themePicker()(window());
}

void CustomTitleBar::clearTheme() {
    // back to the default theme (defaultTheme.json), or the app's own fonts and colors without one
    theme::clear();
    settings::update({{QStringLiteral("themeFile"), QJsonValue::Null}, {QStringLiteral("themePartFiles"), QJsonObject()}});
}

void CustomTitleBar::randomTheme(bool on) {
    // a random colors file and gradients file each time the app opens, and right away when turned on
    settings::update({{QStringLiteral("randomTheme"), on}});
    if (on) {
        theme::loadRandomParts();
    }
}

void CustomTitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        initialPos_ = event->position().toPoint();
    }
    QWidget::mousePressEvent(event);
    event->accept();
}

void CustomTitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (initialPos_) {
        const QPoint delta = event->position().toPoint() - *initialPos_;
        window()->move(window()->x() + delta.x(), window()->y() + delta.y());
    }
    QWidget::mouseMoveEvent(event);
    event->accept();
}

void CustomTitleBar::mouseReleaseEvent(QMouseEvent* event) {
    const bool dragged = initialPos_.has_value();
    initialPos_.reset();
    if (dragged && snaps_ && event->button() == Qt::LeftButton) {
        snapToTop(event->globalPosition().toPoint());
    }
    QWidget::mouseReleaseEvent(event);
    event->accept();
}

void CustomTitleBar::snapToTop(const QPoint& cursor) {
    // dropped at the top edge of a screen: maximized on it (the frameless window misses Windows' own snapping)
    QWidget* main = window();
    // (the layout viewer's display window keeps a simulated screen's exact size)
    if (main->isMaximized() || main->property("awoKeepSize").toBool()) {
        return;
    }
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen || cursor.y() - screen->geometry().top() > kSnapDistance) {
        return;
    }
    // onto that screen first: Windows maximizes a window on the screen holding most of it
    const QRect available = screen->availableGeometry();
    main->move(std::min(std::max(main->x(), available.left()), available.right() - 100), available.top());
    main->showMaximized();
}

void CustomTitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    window()->showMaximized();
    event->accept();
}

}  // namespace awo
