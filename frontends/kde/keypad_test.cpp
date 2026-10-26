/*
 * keypad_test -- the Qt key translation (KeyForward.cpp) and the Controllers
 * window (keypad/KeypadWindow.cpp): raw press/release, the held highlight,
 * Map / Clear / Defaults, and the key path, driven with synthetic events on
 * the offscreen platform against an unstarted session.
 *
 * Scancodes are evdev + 8, the convention both Qt platform plugins use.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QKeyEvent>
#include <QTemporaryDir>
#include <QtTest>

#include "KeyForward.h"
#include "adamsession.h"
#include "keypad/KeypadWindow.h"

namespace {

constexpr quint32 kEv = 8; /* nativeScanCode = evdev code + kEv */
enum { EV_ESC = 1, EV_8 = 9, EV_TAB = 15, EV_LSHIFT = 42, EV_KPASTERISK = 55,
       EV_RSHIFT = 54, EV_F9 = 67, EV_KP8 = 72, EV_KP6 = 77, EV_KPENTER = 96 };

QKeyEvent keyEvent(QEvent::Type type, int key, quint32 evdev,
                   Qt::KeyboardModifiers mods = {}, bool autorepeat = false,
                   const QString &text = QString())
{
    return QKeyEvent(type, key, mods, evdev ? evdev + kEv : 0, 0, 0, text,
                     autorepeat);
}

constexpr int UP = ADAM_TARGET_PORT(0, ADAM_ACT_UP);
constexpr int RIGHT = ADAM_TARGET_PORT(0, ADAM_ACT_RIGHT);
constexpr int FIRE_L = ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_L);

} // namespace

class KeypadTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    adamsession *m_s = nullptr;

    void press(QWidget *w, int key, quint32 evdev, Qt::KeyboardModifiers m = {})
    {
        QKeyEvent e = keyEvent(QEvent::KeyPress, key, evdev, m);
        QCoreApplication::sendEvent(w, &e);
    }
    void release(QWidget *w, int key, quint32 evdev, Qt::KeyboardModifiers m = {})
    {
        QKeyEvent e = keyEvent(QEvent::KeyRelease, key, evdev, m);
        QCoreApplication::sendEvent(w, &e);
    }
    PadButton *pad(QWidget *w, int target)
    {
        for (PadButton *b : w->findChildren<PadButton *>())
            if (b->target() == target) return b;
        return nullptr;
    }
    QPushButton *named(QWidget *w, const QString &text)
    {
        for (QPushButton *b : w->findChildren<QPushButton *>())
            if (b->text() == text) return b;
        return nullptr;
    }
    QLabel *hint(QWidget *w)
    {
        const auto labels = w->findChildren<QLabel *>();
        return labels.isEmpty() ? nullptr : labels.last();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        adamsession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = adamsession_new(&paths);
        QVERIFY(m_s);
    }
    void init() { adamsession_bindings_reset(m_s); adamsession_release_all(m_s); }
    void cleanupTestCase() { adamsession_free(m_s); }

    void keysymFromScancode()
    {
        auto ks = [](QKeyEvent e) { return adamKeysymFromQt(&e); };
        /* Physical keys: the numeric keypad stays distinct from the top row,
         * and Shift cannot turn 8 into '*'. */
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_8, EV_KP8, Qt::KeypadModifier)),
                 (uint32_t)ADAMSESSION_KEYSYM_KP_8);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_8, EV_8)), (uint32_t)'8');
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Asterisk, EV_8, Qt::ShiftModifier)),
                 (uint32_t)'8');
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Asterisk, EV_KPASTERISK, Qt::KeypadModifier)),
                 (uint32_t)ADAMSESSION_KEYSYM_KP_MULTIPLY);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Enter, EV_KPENTER, Qt::KeypadModifier)),
                 (uint32_t)ADAMSESSION_KEYSYM_KP_ENTER);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Shift, EV_LSHIFT)),
                 (uint32_t)ADAMSESSION_KEYSYM_LSHIFT);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Shift, EV_RSHIFT)),
                 (uint32_t)ADAMSESSION_KEYSYM_RSHIFT);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Escape, EV_ESC)),
                 (uint32_t)ADAMSESSION_KEYSYM_ESCAPE);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_F9, EV_F9)),
                 (uint32_t)ADAMSESSION_KEYSYM_F9);
    }

    void keysymFromQtKey()
    {
        /* No scancode (a synthetic event): Qt's own key code. */
        auto ks = [](QKeyEvent e) { return adamKeysymFromQt(&e); };
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_A, 0)), (uint32_t)'a');
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_5, 0)), (uint32_t)'5');
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_5, 0, Qt::KeypadModifier)),
                 (uint32_t)ADAMSESSION_KEYSYM_KP_5);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_Tab, 0)),
                 (uint32_t)ADAMSESSION_KEYSYM_TAB);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_F12, 0)),
                 (uint32_t)ADAMSESSION_KEYSYM_F12);
        QCOMPARE(ks(keyEvent(QEvent::KeyPress, Qt::Key_unknown, 0)), (uint32_t)0);
    }

    void forwardKey()
    {
        /* A bound key drives its target and is consumed; the release lets go. */
        QKeyEvent down = keyEvent(QEvent::KeyPress, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QKeyEvent up = keyEvent(QEvent::KeyRelease, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QVERIFY(adamForwardKey(m_s, &down, true));
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        /* An auto-repeat pair must not let go for an instant. */
        QKeyEvent rup = keyEvent(QEvent::KeyRelease, Qt::Key_8, EV_KP8, Qt::KeypadModifier, true);
        QVERIFY(!adamForwardKey(m_s, &rup, false));
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        QVERIFY(adamForwardKey(m_s, &up, false));
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);

        /* The top-row 8 is not bound: it falls through to the ADAM keyboard. */
        QKeyEvent top = keyEvent(QEvent::KeyPress, Qt::Key_8, EV_8, {}, false,
                                 QStringLiteral("8"));
        QVERIFY(adamForwardKey(m_s, &top, true));
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);

        /* Ctrl + a digit is the port 1 keypad chord. */
        QKeyEvent chord = keyEvent(QEvent::KeyPress, Qt::Key_3, 4, Qt::ControlModifier);
        QVERIFY(adamForwardKey(m_s, &chord, true));
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << 3));
        adamsession_release_all(m_s);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
    }

    void windowIsFixedAndFloating()
    {
        KeypadWindow w(m_s);
        QVERIFY(!w.isVisible());                       /* hidden until asked for */
        QVERIFY(w.windowType() == Qt::Dialog);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(w.minimumSize(), w.maximumSize());    /* one size, no resizing */
        QCOMPARE(w.size(), w.minimumSize());
        /* Two controllers: 2 x (12 keypad + 4 stick + 2 fire) + 2 reset. */
        QCOMPARE(w.findChildren<PadButton *>().size(), 2 * 18 + 2);
    }

    void rawPressAndRelease()
    {
        KeypadWindow w(m_s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        PadButton *up = pad(&w, UP);
        QVERIFY(up);

        QTest::mousePress(up, Qt::LeftButton);
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        QTest::mouseRelease(up, Qt::LeftButton);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);

        /* Dragging off a held control lets go of it. */
        QTest::mousePress(up, Qt::LeftButton);
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(up, &leave);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
        QTest::mouseRelease(up, Qt::LeftButton);

        /* Controller 2 is its own port. */
        PadButton *fire2 = pad(&w, ADAM_TARGET_PORT(1, ADAM_ACT_FIRE_R));
        QTest::mousePress(fire2, Qt::LeftButton);
        QVERIFY(adamsession_buttons_held(m_s, 1) & (1u << ADAM_ACT_FIRE_R));
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
        QTest::mouseRelease(fire2, Qt::LeftButton);
        QCOMPARE(adamsession_buttons_held(m_s, 1), 0u);

        /* Hiding the window while the mouse holds a control releases it. */
        QTest::mousePress(up, Qt::LeftButton);
        w.hide();
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
    }

    void heldHighlight()
    {
        KeypadWindow w(m_s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        PadButton *up = pad(&w, UP);
        QVERIFY(!up->isChecked());
        /* A numpad press in the window lights the stick. */
        press(&w, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QTRY_VERIFY_WITH_TIMEOUT(up->isChecked(), 1000);
        release(&w, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!up->isChecked(), 1000);
        /* Losing focus lets go of everything the keyboard held. */
        press(&w, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        adamsession_release_all(m_s);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
    }

    void mapRebindClearDefaults()
    {
        KeypadWindow w(m_s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QPushButton *map = named(&w, QStringLiteral("Map"));
        QPushButton *clear = named(&w, QStringLiteral("Clear"));
        QPushButton *defaults = named(&w, QStringLiteral("Defaults"));
        QVERIFY(map && clear && defaults);
        PadButton *up = pad(&w, UP);
        QVERIFY(!map->isChecked());
        QVERIFY(!clear->isEnabled());

        QTest::mouseClick(map, Qt::LeftButton);
        QVERIFY(map->isChecked());                      /* armed: accent */
        QCOMPARE(hint(&w)->text(), QStringLiteral("Click a control to remap"));
        QCOMPARE(up->text(), QStringLiteral("Keypad 8")); /* bound key names */
        QCOMPARE(pad(&w, ADAM_TARGET_PORT(1, ADAM_ACT_UP))->text(), QStringLiteral("—"));

        /* While armed a click picks, and does not press the machine. */
        QTest::mousePress(up, Qt::LeftButton);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u);
        QTest::mouseRelease(up, Qt::LeftButton);
        QVERIFY(up->isChecked());                       /* the picked one */
        QVERIFY(clear->isEnabled());
        QVERIFY(hint(&w)->text().contains(QStringLiteral("Controller 1: Up")));

        /* The next key binds, and takes the key from its holder. */
        press(&w, Qt::Key_6, EV_KP6, Qt::KeypadModifier);
        QCOMPARE(adamsession_binding_get(m_s, UP).keysym, (uint32_t)ADAMSESSION_KEYSYM_KP_6);
        QCOMPARE(adamsession_binding_get(m_s, RIGHT).keysym, (uint32_t)0);
        QCOMPARE(adamsession_buttons_held(m_s, 0), 0u); /* not played */
        QVERIFY(hint(&w)->text().contains(QStringLiteral("Controller 1: Up = Keypad 6")));
        QVERIFY(hint(&w)->text().contains(QStringLiteral("taken from Controller 1: Right")));
        QVERIFY(!up->isChecked());                      /* waiting for a target */
        QVERIFY(map->isChecked());
        release(&w, Qt::Key_6, EV_KP6, Qt::KeypadModifier);

        /* Escape and a bare modifier bind like any other key. */
        QTest::mouseClick(pad(&w, FIRE_L), Qt::LeftButton);
        press(&w, Qt::Key_Escape, EV_ESC);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).keysym, (uint32_t)ADAMSESSION_KEYSYM_ESCAPE);
        QTest::mouseClick(pad(&w, FIRE_L), Qt::LeftButton);
        press(&w, Qt::Key_Shift, EV_LSHIFT);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).keysym, (uint32_t)ADAMSESSION_KEYSYM_LSHIFT);

        /* F10-F12 are the main window's chords, which a binding could never see. */
        QTest::mouseClick(pad(&w, FIRE_L), Qt::LeftButton);
        press(&w, Qt::Key_F10, 68);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).keysym, (uint32_t)ADAMSESSION_KEYSYM_LSHIFT);

        /* Clear drops both the key and the gamepad button of the picked one. */
        adamsession_binding_set_button(m_s, FIRE_L, ADAM_PAD_BTN_SOUTH, nullptr, 0);
        QTest::mouseClick(clear, Qt::LeftButton);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).keysym, (uint32_t)0);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).button, (int)ADAM_PAD_BTN_NONE);
        QVERIFY(hint(&w)->text().contains(QStringLiteral("cleared")));

        QTest::mouseClick(defaults, Qt::LeftButton);
        QCOMPARE(adamsession_binding_get(m_s, UP).keysym, (uint32_t)ADAMSESSION_KEYSYM_KP_8);
        QCOMPARE(adamsession_binding_get(m_s, FIRE_L).button, (int)ADAM_PAD_BTN_SOUTH);

        /* Leaving Map restores the faces and plays the machine again. */
        QTest::mouseClick(map, Qt::LeftButton);
        QVERIFY(!map->isChecked());
        QCOMPARE(up->text(), QStringLiteral("▲"));
        QVERIFY(!clear->isEnabled());
        press(&w, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
        QVERIFY(adamsession_buttons_held(m_s, 0) & (1u << ADAM_ACT_UP));
        release(&w, Qt::Key_8, EV_KP8, Qt::KeypadModifier);
    }

    void f9ClosesAndCancelsMap()
    {
        KeypadWindow w(m_s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QPushButton *map = named(&w, QStringLiteral("Map"));
        QTest::mouseClick(map, Qt::LeftButton);
        QTest::mouseClick(pad(&w, UP), Qt::LeftButton);
        press(&w, Qt::Key_F9, EV_F9);
        QVERIFY(!w.isVisible());
        QVERIFY(!map->isChecked());                     /* capture cancelled */
        QCOMPARE(adamsession_binding_get(m_s, UP).keysym, (uint32_t)ADAMSESSION_KEYSYM_KP_8);
    }
};

QTEST_MAIN(KeypadTest)
#include "keypad_test.moc"
