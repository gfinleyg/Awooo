#include "KeyShortcuts.hpp"

#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QFileDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include "Theme.hpp"

namespace awo::keys {

const QList<int> kEnter = {Qt::Key_Return, Qt::Key_Enter};
// (the background, text and border: the app's light blue boxes, which themes recolor)
const QString kLabelColors = QStringLiteral("#bdd6de");
const QString kLabelDisabledColors = QStringLiteral("#dadbde");

namespace {

const QString kLabelStyle = QStringLiteral(
    "#keyLabel {background-color: %1; color: %2; border: 2px solid %3; border-radius: 8px; "
    "padding: 0px 6px; font: 14pt 'Segoe UI Black';}");

const QSet<int>& moveKeys() {
    static const QSet<int> keys = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                                   Qt::Key_Home, Qt::Key_End,   Qt::Key_PageUp, Qt::Key_PageDown};
    return keys;
}

const QSet<int>& textKeys() {
    static const QSet<int> keys = [] {
        QSet<int> set = moveKeys();
        for (int i = 0; i < 10; ++i) {
            set.insert(Qt::Key_0 + i);
        }
        set.insert(Qt::Key_Return);
        set.insert(Qt::Key_Enter);
        set.insert(Qt::Key_Space);
        set.insert(Qt::Key_Delete);
        set.insert(Qt::Key_Backspace);
        return set;
    }();
    return keys;
}

const QSet<int>& listKeys() {
    static const QSet<int> keys = [] {
        QSet<int> set = moveKeys();
        set.insert(Qt::Key_Space);
        return set;
    }();
    return keys;
}

// the keys a focused editor keeps for itself
QSet<int> editorKeys(QWidget* widget) {
    if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) ||
        qobject_cast<QPlainTextEdit*>(widget) || qobject_cast<QTextEdit*>(widget)) {
        return textKeys();
    }
    if (qobject_cast<QAbstractItemView*>(widget)) {
        return listKeys();
    }
    if (qobject_cast<QAbstractSlider*>(widget)) {
        return moveKeys();
    }
    return {};
}

struct Binding {
    QPointer<QWidget> widget;
    QSet<int> keys;
    Options options;
    QPointer<QWidget> field;
    bool hasField = false;

    bool deleted() const { return !widget || (hasField && !field); }
    // on screen, and its condition (if any) holds
    bool shown() const { return widget->isVisible() && (!options.when || options.when()); }
    bool usable() const { return shown() && widget->isEnabled(); }
    void trigger() const {
        if (options.action) {
            options.action();
        } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            button->click();
        }
    }
};

QList<Binding>& bindings() {
    static QList<Binding> list;
    return list;
}

QList<Binding> liveBindings(QWidget* window) {
    bindings().removeIf([](const Binding& binding) { return binding.deleted(); });
    QList<Binding> live;
    for (const Binding& binding : bindings()) {
        if (binding.widget->window() == window) {
            live << binding;
        }
    }
    return live;
}

QList<Binding> labeled(QWidget* window) {
    QList<Binding> list;
    for (const Binding& binding : liveBindings(window)) {
        if (!binding.options.label.isEmpty() && binding.shown()) {
            list << binding;
        }
    }
    return list;
}

QLabel* makeLabel(const Binding& binding, QWidget* window) {
    auto* label = new QLabel(binding.options.label, window);
    label->setObjectName(QStringLiteral("keyLabel"));
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    const bool enabled = binding.widget->isEnabled();
    theme::setSheet(label, kLabelStyle.arg(enabled ? kLabelColors : kLabelDisabledColors,
                                           enabled ? QStringLiteral("black") : QStringLiteral("#808080"),
                                           enabled ? QStringLiteral("black") : QStringLiteral("#808080")));
    label->adjustSize();
    QWidget* widget = binding.widget;
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0));
    const int width = label->width();
    const int height = label->height();
    int x = 0;
    int y = 0;
    switch (binding.options.place) {
    case Place::Left:
        x = topLeft.x() - width - 4;
        y = topLeft.y() + (widget->height() - height) / 2;
        break;
    case Place::Inside:
        x = topLeft.x() + widget->width() - width - 8;
        y = topLeft.y() + 8;
        break;
    case Place::Corner:
        // over the top right corner, a little outside it so the button's text stays readable
        x = topLeft.x() + widget->width() - width + 6;
        y = topLeft.y() - 6;
        break;
    }
    label->move(std::max(0, std::min(x, window->width() - width)), std::max(0, std::min(y, window->height() - height)));
    label->show();
    label->raise();
    return label;
}

void removeLabels(QList<QPointer<QLabel>>& labels) {
    for (const QPointer<QLabel>& label : labels) {
        if (label) {
            label->hide();
            label->deleteLater();
        }
    }
    labels.clear();
}

class KeyManager : public QObject {
public:
    explicit KeyManager(QObject* parent) : QObject(parent) {
        revealTimer.setSingleShot(true);
        revealTimer.setInterval(kRevealDelayMs);
        connect(&revealTimer, &QTimer::timeout, this, &KeyManager::showLabels);
    }

    QTimer revealTimer;
    QList<QPointer<QLabel>> labels;

    bool eventFilter(QObject* object, QEvent* event) override {
        switch (event->type()) {
        case QEvent::KeyPress:
            return keyPress(object, static_cast<QKeyEvent*>(event));
        case QEvent::KeyRelease: {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Control && !key->isAutoRepeat()) {
                hideLabels();
            }
            break;
        }
        case QEvent::MouseButtonPress:
        case QEvent::WindowDeactivate:
        case QEvent::ApplicationStateChange:  // (switched to another app with Ctrl down: its release never comes)
            hideLabels();
            break;
        case QEvent::Polish:
            if (qobject_cast<QPushButton*>(object) || qobject_cast<QToolButton*>(object)) {
                // (the standard message boxes keep theirs, they're driven by Tab/Enter)
                auto* button = static_cast<QWidget*>(object);
                if (!inStandardDialog(button)) {
                    button->setFocusPolicy(Qt::NoFocus);
                }
            }
            break;
        case QEvent::Hide: {
            auto* widget = qobject_cast<QWidget*>(object);
            if (widget && widget->isWindow() && !event->spontaneous() && !qobject_cast<QMainWindow*>(widget) &&
                widget->windowType() != Qt::Popup && widget->windowType() != Qt::ToolTip) {
                // a popup window closed: Windows doesn't always hand the keys back to the main window
                QTimer::singleShot(0, this, [] { activateMainWindow(); });
            }
            break;
        }
        default:
            break;
        }
        return false;
    }

    static void activateMainWindow() {
        if (QApplication::activeWindow()) {
            return;
        }
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (qobject_cast<QMainWindow*>(window) && window->isVisible() && !window->isMinimized()) {
                window->activateWindow();
                return;
            }
        }
    }

    bool keyPress(QObject* object, QKeyEvent* event) {
        // a key event reaches here once per widget it passes through (and for the QWindow): only the first, at the
        // focus widget (or the window, with nothing focused)
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget) {
            return false;
        }
        const int key = event->key();
        if (key != Qt::Key_Control) {
            hideLabels();
        }
        QWidget* focus = QApplication::focusWidget();
        if (widget != focus && !(focus == nullptr && widget->isWindow())) {
            return false;
        }
        if (key == Qt::Key_Control) {
            if (!event->isAutoRepeat() && labels.isEmpty()) {
                revealTimer.start();
            }
            return false;
        }
        if (QApplication::activePopupWidget()) {
            return false;  // a menu is open (the settings menu has its own letters)
        }
        if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
            return false;
        }
        const QSet<int> focusKeys = editorKeys(focus);
        for (const Binding& binding : liveBindings(widget->window())) {
            if (!binding.keys.contains(key)) {
                continue;
            }
            if (binding.hasField) {
                if (focus != binding.field) {
                    continue;
                }
            } else if (focusKeys.contains(key)) {
                continue;
            }
            if (!binding.usable()) {
                continue;
            }
            if (!(event->isAutoRepeat() && !binding.options.repeat)) {
                binding.trigger();
            }
            return true;
        }
        return false;
    }

    void showLabels() {
        // still held, and only Ctrl
        if (QApplication::queryKeyboardModifiers() != Qt::ControlModifier) {
            return;
        }
        QWidget* window = QApplication::activeWindow();
        if (!window || QApplication::activePopupWidget()) {
            return;
        }
        showOn(window);
    }

    void showOn(QWidget* window) {
        hideLabels();
        for (const Binding& binding : labeled(window)) {
            labels << makeLabel(binding, window);
        }
    }

    void hideLabels() {
        revealTimer.stop();
        removeLabels(labels);
    }
};

KeyManager* manager = nullptr;

struct Pinned {
    QList<QPointer<QLabel>> labels;
    QString state;
};

Pinned& pinned() {
    static Pinned instance;
    return instance;
}

}  // namespace

bool inStandardDialog(QWidget* widget) {
    for (; widget; widget = widget->parentWidget()) {
        if (qobject_cast<QMessageBox*>(widget) || qobject_cast<QFileDialog*>(widget)) {
            return true;
        }
    }
    return false;
}

void add(QWidget* widget, const QList<int>& keys, const Options& options) {
    Binding binding;
    binding.widget = widget;
    binding.keys = QSet<int>(keys.begin(), keys.end());
    binding.options = options;
    binding.field = options.field;
    binding.hasField = options.field != nullptr;
    bindings().append(binding);
}

void add(QWidget* widget, int key, const QString& label) {
    Options options;
    options.label = label;
    add(widget, QList<int>{key}, options);
}

void tabCycle(const QList<QWidget*>& fields, QWidget* anchor) {
    QList<QPointer<QWidget>> guarded;
    for (QWidget* field : fields) {
        guarded << field;
    }
    const auto step = [guarded](bool forward) {
        QList<QWidget*> shown;
        for (const QPointer<QWidget>& field : guarded) {
            if (field && field->isVisible() && field->isEnabled()) {
                shown << field;
            }
        }
        if (shown.isEmpty()) {
            return;
        }
        const qsizetype at = shown.indexOf(QApplication::focusWidget());
        qsizetype index = 0;
        if (at >= 0) {
            index = (at + (forward ? 1 : -1) + shown.size()) % shown.size();
        } else {
            index = forward ? 0 : shown.size() - 1;
        }
        // (a tab focus selects the box's text, ready to type over)
        shown[index]->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    };
    Options tab;
    tab.label = QStringLiteral("Tab");
    tab.action = [step] { step(true); };
    add(anchor, {Qt::Key_Tab}, tab);
    Options backtab;
    backtab.action = [step] { step(false); };
    add(anchor, {Qt::Key_Backtab}, backtab);
}

void stepList(QListWidget* list, int step) {
    const int count = list->count();
    if (count) {
        const int row = list->currentRow();
        list->setCurrentRow(row >= 0 ? std::max(0, std::min(row + step, count - 1)) : 0);
    }
}

void install(QApplication* app) {
    if (!manager) {
        manager = new KeyManager(app);
        app->installEventFilter(manager);
    }
}

void pinLabels(QWidget* window) {
    // rebuilt only when what they'd show, or where, has changed
    QStringList stateParts;
    const QList<Binding> list = labeled(window);
    for (const Binding& binding : list) {
        const QPoint at = binding.widget->mapTo(window, QPoint(0, 0));
        stateParts << QStringLiteral("%1:%2:%3,%4:%5x%6")
                          .arg(reinterpret_cast<quintptr>(binding.widget.data()))
                          .arg(binding.widget->isEnabled())
                          .arg(at.x())
                          .arg(at.y())
                          .arg(binding.widget->width())
                          .arg(binding.widget->height());
    }
    stateParts << QStringLiteral("%1x%2").arg(window->width()).arg(window->height());
    const QString state = stateParts.join(u'|');
    bool allAlive = true;
    for (const QPointer<QLabel>& label : pinned().labels) {
        allAlive = allAlive && label;
    }
    if (state == pinned().state && allAlive) {
        return;
    }
    removeLabels(pinned().labels);
    for (const Binding& binding : list) {
        pinned().labels << makeLabel(binding, window);
    }
    pinned().state = state;
}

void unpinLabels() {
    removeLabels(pinned().labels);
    pinned().state.clear();
}

int bindingCount() {
    bindings().removeIf([](const Binding& binding) { return binding.deleted(); });
    return static_cast<int>(bindings().size());
}

void clearBindings() {
    bindings().clear();
}

void showLabels() {
    if (manager) {
        manager->showLabels();
    }
}

void showLabelsOn(QWidget* window) {
    if (manager) {
        manager->showOn(window);
    }
}

void hideLabels() {
    if (manager) {
        manager->hideLabels();
    }
}

bool revealPending() {
    return manager && manager->revealTimer.isActive();
}

}  // namespace awo::keys
