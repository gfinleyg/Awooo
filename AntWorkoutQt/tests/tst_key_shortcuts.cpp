// Key shortcuts (src/ui/KeyShortcuts): keys pressing buttons, which keys a focused editor keeps, Tab cycling and the
// key labels; real widgets on the offscreen platform, real key events through the app wide event filter
// (tests/test_key_shortcuts.py)

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "KeyShortcuts.hpp"
#include "TestSupport.hpp"

using namespace awo;

namespace {

class Window : public QMainWindow {
public:
    explicit Window(const QStringList& names) {
        auto* central = new QWidget;
        setCentralWidget(central);
        box = new QVBoxLayout(central);
        for (const QString& name : names) {
            auto* button = new QPushButton(name);
            QObject::connect(button, &QPushButton::clicked, [this, name] { clicks << name; });
            box->addWidget(button);
            buttons.insert(name, button);
        }
    }

    template <typename T>
    T* add(T* widget) {
        box->addWidget(widget);
        return widget;
    }

    QVBoxLayout* box;
    QMap<QString, QPushButton*> buttons;
    QStringList clicks;
};

void shown(Window& window) {
    window.resize(400, 300);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QApplication::processEvents();
}

// a key press as the app gets it: at the focus widget, else the window
void press(QWidget* widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool autorepeat = false) {
    QWidget* target = QApplication::focusWidget() ? QApplication::focusWidget() : widget->window();
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), autorepeat);
    QApplication::sendEvent(target, &event);
}

QMap<QString, QLabel*> labelsOn(QWidget* window) {
    QMap<QString, QLabel*> labels;
    for (QLabel* label : window->findChildren<QLabel*>(QStringLiteral("keyLabel"))) {
        if (label->isVisible()) {
            labels.insert(label->text(), label);
        }
    }
    return labels;
}

keys::Options with(std::function<void()> action = {}, std::function<bool()> when = {}, QWidget* field = nullptr,
                   bool repeat = false) {
    keys::Options options;
    options.action = std::move(action);
    options.when = std::move(when);
    options.field = field;
    options.repeat = repeat;
    return options;
}

}  // namespace

class TestKeyShortcuts : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { keys::install(qApp); }

    void cleanup() {
        keys::unpinLabels();
        keys::hideLabels();
        keys::clearBindings();
        QApplication::processEvents();
    }

    void keyPressesItsButton() {
        Window window({QStringLiteral("Start"), QStringLiteral("Stop")});
        keys::add(window.buttons[QStringLiteral("Start")], Qt::Key_S, QStringLiteral("S"));
        keys::add(window.buttons[QStringLiteral("Stop")], {Qt::Key_X, Qt::Key_Q});
        shown(window);
        press(&window, Qt::Key_S);
        press(&window, Qt::Key_Q);
        press(&window, Qt::Key_Z);  // not bound
        QCOMPARE(window.clicks, (QStringList{QStringLiteral("Start"), QStringLiteral("Stop")}));
    }

    void hiddenOrDisabledButtonsDoNothing() {
        Window window({QStringLiteral("A"), QStringLiteral("B")});
        keys::add(window.buttons[QStringLiteral("A")], Qt::Key_A);
        keys::add(window.buttons[QStringLiteral("B")], Qt::Key_B);
        shown(window);
        window.buttons[QStringLiteral("A")]->hide();
        window.buttons[QStringLiteral("B")]->setEnabled(false);
        press(&window, Qt::Key_A);
        press(&window, Qt::Key_B);
        QVERIFY(window.clicks.isEmpty());
    }

    void actionAndCondition() {
        Window window({QStringLiteral("Go")});
        QStringList ran;
        bool on = false;
        keys::add(window.buttons[QStringLiteral("Go")], {Qt::Key_Space}, with({}, [&on] { return !on; }));
        keys::add(window.buttons[QStringLiteral("Go")], {Qt::Key_Space},
                  with([&ran] { ran << QStringLiteral("paused"); }, [&on] { return on; }));
        shown(window);
        press(&window, Qt::Key_Space);
        on = true;
        press(&window, Qt::Key_Space);
        QCOMPARE(window.clicks, QStringList{QStringLiteral("Go")});
        QCOMPARE(ran, QStringList{QStringLiteral("paused")});
    }

    void ctrlAndAltKeepTheirMeaning() {
        Window window({QStringLiteral("Copy")});
        keys::add(window.buttons[QStringLiteral("Copy")], Qt::Key_C);
        shown(window);
        press(&window, Qt::Key_C, Qt::ControlModifier);
        press(&window, Qt::Key_C, Qt::AltModifier);
        QVERIFY(window.clicks.isEmpty());
        // Shift is fine
        press(&window, Qt::Key_C, Qt::ShiftModifier);
        QCOMPARE(window.clicks, QStringList{QStringLiteral("Copy")});
    }

    void heldKeysRepeatOnlyWhenAsked() {
        Window window({QStringLiteral("Once"), QStringLiteral("Again")});
        keys::add(window.buttons[QStringLiteral("Once")], Qt::Key_O);
        keys::add(window.buttons[QStringLiteral("Again")], {Qt::Key_A}, with({}, {}, nullptr, true));
        shown(window);
        for (int i = 0; i < 3; ++i) {
            press(&window, Qt::Key_O, Qt::NoModifier, true);
            press(&window, Qt::Key_A, Qt::NoModifier, true);
        }
        QCOMPARE(window.clicks, QStringList(3, QStringLiteral("Again")));
    }

    void focusedTextBoxKeepsItsKeys() {
        Window window({QStringLiteral("Set FTP"), QStringLiteral("Enter anywhere"), QStringLiteral("Letter")});
        auto* field = window.add(new QLineEdit);
        keys::add(window.buttons[QStringLiteral("Set FTP")], keys::kEnter, with({}, {}, field));
        keys::add(window.buttons[QStringLiteral("Enter anywhere")], keys::kEnter);
        keys::add(window.buttons[QStringLiteral("Letter")], {Qt::Key_L, Qt::Key_5, Qt::Key_Delete});
        shown(window);
        field->setFocus();
        QApplication::processEvents();
        QCOMPARE(QApplication::focusWidget(), field);
        // digits and Delete stay with the box, letters still go to the buttons
        QTest::keyClick(field, Qt::Key_5);
        QTest::keyClick(field, Qt::Key_Delete);
        QTest::keyClick(field, Qt::Key_L);
        QCOMPARE(window.clicks, QStringList{QStringLiteral("Letter")});
        QCOMPARE(field->text(), QStringLiteral("5"));
        // Enter bound to the box presses its button
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(window.clicks, (QStringList{QStringLiteral("Letter"), QStringLiteral("Set FTP")}));
        // away from the box, the box's Enter is someone else's
        field->clearFocus();
        QApplication::processEvents();
        press(&window, Qt::Key_Enter);
        QCOMPARE(window.clicks.last(), QStringLiteral("Enter anywhere"));
    }

    void focusedListKeepsArrowsAndSpaceButNotDelete() {
        Window window({QStringLiteral("Remove"), QStringLiteral("Toggle")});
        auto* songs = window.add(new QListWidget);
        songs->addItems({QStringLiteral("one"), QStringLiteral("two")});
        keys::add(window.buttons[QStringLiteral("Remove")], Qt::Key_Delete);
        keys::add(window.buttons[QStringLiteral("Toggle")], Qt::Key_Space);
        shown(window);
        songs->setFocus();
        QApplication::processEvents();
        QTest::keyClick(songs, Qt::Key_Space);
        QTest::keyClick(songs, Qt::Key_Delete);
        QCOMPARE(window.clicks, QStringList{QStringLiteral("Remove")});
    }

    void focusedSliderKeepsItsArrows() {
        Window window({QStringLiteral("Up")});
        auto* slider = window.add(new QSlider(Qt::Horizontal));
        keys::add(window.buttons[QStringLiteral("Up")], Qt::Key_Right);
        shown(window);
        slider->setFocus();
        QApplication::processEvents();
        QTest::keyClick(slider, Qt::Key_Right);
        QVERIFY(window.clicks.isEmpty());
        QCOMPARE(slider->value(), 1);
    }

    void keysBelongToTheirWindow() {
        Window first({QStringLiteral("First")});
        Window second({QStringLiteral("Second")});
        keys::add(first.buttons[QStringLiteral("First")], Qt::Key_F);
        keys::add(second.buttons[QStringLiteral("Second")], Qt::Key_F);
        shown(first);
        shown(second);
        press(&second, Qt::Key_F);
        QVERIFY(first.clicks.isEmpty());
        QCOMPARE(second.clicks, QStringList{QStringLiteral("Second")});
    }

    void deletedWidgetsAreForgotten() {
        Window window({QStringLiteral("Gone"), QStringLiteral("Kept")});
        keys::add(window.buttons[QStringLiteral("Gone")], Qt::Key_G);
        keys::add(window.buttons[QStringLiteral("Kept")], Qt::Key_K);
        shown(window);
        delete window.buttons[QStringLiteral("Gone")];
        press(&window, Qt::Key_G);
        QCOMPARE(keys::bindingCount(), 1);
    }

    void tabCyclesJustTheGivenBoxes() {
        Window window({QStringLiteral("Anchor")});
        QList<QWidget*> boxes;
        for (int i = 0; i < 3; ++i) {
            boxes << window.add(new QLineEdit(QString::number(i)));
        }
        auto* other = window.add(new QLineEdit(QStringLiteral("not in the cycle")));
        keys::tabCycle(boxes, window.buttons[QStringLiteral("Anchor")]);
        shown(window);
        other->setFocus();
        QApplication::processEvents();
        QList<QWidget*> order;
        for (int i = 0; i < 4; ++i) {
            press(&window, Qt::Key_Tab);
            order << QApplication::focusWidget();
        }
        QCOMPARE(order, (QList<QWidget*>{boxes[0], boxes[1], boxes[2], boxes[0]}));
        press(&window, Qt::Key_Backtab);
        QCOMPARE(QApplication::focusWidget(), boxes[2]);
        // hidden ones are skipped
        boxes[0]->hide();
        press(&window, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), boxes[1]);
    }

    void stepList() {
        QListWidget songs;
        keys::stepList(&songs, 1);  // empty: nothing
        songs.addItems({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        keys::stepList(&songs, -1);
        QCOMPARE(songs.currentRow(), 0);
        keys::stepList(&songs, 1);
        keys::stepList(&songs, 5);
        QCOMPARE(songs.currentRow(), 2);
        keys::stepList(&songs, -1);
        QCOMPARE(songs.currentRow(), 1);
    }

    void buttonsNeverTakeFocus() {
        Window window({QStringLiteral("Plain")});
        shown(window);
        QCOMPARE(window.buttons[QStringLiteral("Plain")]->focusPolicy(), Qt::NoFocus);
        QMessageBox box(QMessageBox::Question, QStringLiteral("Sure?"), QStringLiteral("Really?"),
                        QMessageBox::Yes | QMessageBox::No);
        box.show();
        QApplication::processEvents();
        QVERIFY(box.button(QMessageBox::Yes)->focusPolicy() != Qt::NoFocus);
        QVERIFY(keys::inStandardDialog(box.button(QMessageBox::Yes)));
        QVERIFY(!keys::inStandardDialog(window.buttons[QStringLiteral("Plain")]));
        box.close();
    }

    // ---- Labels ----

    void pinnedLabels() {
        Window window({QStringLiteral("Corner"), QStringLiteral("Left"), QStringLiteral("Inside"), QStringLiteral("Hidden"),
                       QStringLiteral("Disabled"), QStringLiteral("Quiet")});
        keys::add(window.buttons[QStringLiteral("Corner")], Qt::Key_C, QStringLiteral("C"));
        keys::Options left;
        left.label = QStringLiteral("L");
        left.place = keys::Place::Left;
        keys::add(window.buttons[QStringLiteral("Left")], {Qt::Key_L}, left);
        keys::Options inside;
        inside.label = QStringLiteral("I");
        inside.place = keys::Place::Inside;
        keys::add(window.buttons[QStringLiteral("Inside")], {Qt::Key_I}, inside);
        keys::add(window.buttons[QStringLiteral("Hidden")], Qt::Key_H, QStringLiteral("H"));
        keys::add(window.buttons[QStringLiteral("Disabled")], Qt::Key_D, QStringLiteral("D"));
        keys::add(window.buttons[QStringLiteral("Quiet")], Qt::Key_Q);
        keys::Options esc;
        esc.label = QStringLiteral("Esc");  // a label for a key handled elsewhere
        keys::add(window.buttons[QStringLiteral("Corner")], {}, esc);
        shown(window);
        window.buttons[QStringLiteral("Hidden")]->hide();
        window.buttons[QStringLiteral("Disabled")]->setEnabled(false);
        QApplication::processEvents();
        keys::pinLabels(&window);
        QMap<QString, QLabel*> labels = labelsOn(&window);
        QCOMPARE(QStringList(labels.keys()),
                 (QStringList{QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("Esc"), QStringLiteral("I"), QStringLiteral("L")}));
        QVERIFY(labels[QStringLiteral("D")]->styleSheet().contains(keys::kLabelDisabledColors));
        QVERIFY(labels[QStringLiteral("C")]->styleSheet().contains(keys::kLabelColors));
        // every label inside the window
        for (QLabel* label : labels) {
            QVERIFY(label->x() >= 0 && label->x() <= window.width() - label->width());
            QVERIFY(label->y() >= 0 && label->y() <= window.height() - label->height());
        }
        // nothing changed: the same labels are kept
        QLabel* before = labels[QStringLiteral("C")];
        keys::pinLabels(&window);
        QCOMPARE(labelsOn(&window)[QStringLiteral("C")], before);
        window.buttons[QStringLiteral("Hidden")]->show();
        keys::pinLabels(&window);
        QApplication::processEvents();
        QVERIFY(labelsOn(&window).contains(QStringLiteral("H")));
        keys::unpinLabels();
        QApplication::processEvents();
        QVERIFY(labelsOn(&window).isEmpty());
    }

    void heldCtrlShowsLabels() {
        Window window({QStringLiteral("Go")});
        keys::add(window.buttons[QStringLiteral("Go")], Qt::Key_G, QStringLiteral("G"));
        shown(window);
        press(&window, Qt::Key_Control);
        QVERIFY(keys::revealPending());
        keys::hideLabels();
        keys::showLabelsOn(&window);
        QCOMPARE(QStringList(labelsOn(&window).keys()), QStringList{QStringLiteral("G")});
        // any other key hides them
        press(&window, Qt::Key_G, Qt::ControlModifier);
        QApplication::processEvents();
        QVERIFY(labelsOn(&window).isEmpty());
    }

    void ctrlReleasedEarlyShowsNothing() {
        Window window({QStringLiteral("Go")});
        keys::add(window.buttons[QStringLiteral("Go")], Qt::Key_G, QStringLiteral("G"));
        shown(window);
        // (no Ctrl held: the last key event's modifiers are what the app goes by)
        press(&window, Qt::Key_Shift);
        keys::showLabels();
        QVERIFY(labelsOn(&window).isEmpty());
    }
};

QTEST_MAIN(TestKeyShortcuts)
#include "tst_key_shortcuts.moc"
