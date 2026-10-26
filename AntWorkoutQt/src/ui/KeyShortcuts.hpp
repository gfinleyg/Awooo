#pragma once

// A key for every button, and holding Ctrl shows them: a small key label on each button of the active window
// (KeyShortcuts.py).
//
//   add(widget, keys, label, ...)   the key(s) press the button (or run action) while it's showing
//   tabCycle(fields, anchor)        Tab/Shift+Tab move between just these text boxes
//   install(app)                    once, at startup
//   pinLabels(window)/unpinLabels() every label on window, kept up (LayoutViewer's Scenario tab)
//
// How keys are shared out:
//   - Ctrl (or Alt) held: the key isn't one of these (Ctrl+C, Ctrl+V... keep their meaning). Ctrl on its own, held a
//     moment, shows the labels
//   - letters always go to the buttons, a text box focused or not (every text box in the app takes numbers only)
//   - the keys a focused editor uses itself (digits, Space, Enter, Del, the arrows...) stay with it, unless the key
//     is bound to that editor (field, e.g. Enter in the FTP box presses Set new FTP)
//   - a key whose button is hidden or disabled does nothing (its label shows greyed out)
// Buttons never take keyboard focus (so Space/Enter can't press whichever one was last clicked), and closing one of
// the popup windows gives the main window its keys back.

#include <QList>
#include <QString>

#include <functional>

class QApplication;
class QLabel;
class QListWidget;
class QWidget;

namespace awo::keys {

// Ctrl held this long on its own shows the labels (a Ctrl+C on the way doesn't flash them)
constexpr int kRevealDelayMs = 300;
extern const QList<int> kEnter;

enum class Place { Corner, Left, Inside };

struct Options {
    QString label;                // what holding Ctrl shows on the widget (empty: nothing)
    std::function<void()> action;  // instead of clicking the widget
    std::function<bool()> when;    // only while this holds
    QWidget* field = nullptr;      // only while this editor has focus
    Place place = Place::Corner;   // top right, beside it, or inside its top right
    bool repeat = false;           // held keys repeat
};

// keys (empty with a label just shows it, for a key handled elsewhere such as Esc) press widget, a button, or run
// options.action, while widget is showing on the active window
void add(QWidget* widget, const QList<int>& keys, const Options& options = {});
void add(QWidget* widget, int key, const QString& label = {});
void tabCycle(const QList<QWidget*>& fields, QWidget* anchor);
// Up/Down for a list that doesn't have focus: the next/previous row becomes the current one
void stepList(QListWidget* list, int step);
void install(QApplication* app);
void pinLabels(QWidget* window);
void unpinLabels();

// (tests)
int bindingCount();
void clearBindings();
void showLabels();
// the labels Ctrl would show, on window (as if Ctrl were held with it active)
void showLabelsOn(QWidget* window);
void hideLabels();
bool revealPending();
bool inStandardDialog(QWidget* widget);
extern const QString kLabelColors;
extern const QString kLabelDisabledColors;

}  // namespace awo::keys
