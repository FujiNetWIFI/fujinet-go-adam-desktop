/*
 * keypad_test -- the Qt key translation and the key path (KeyForward.cpp),
 * driven with synthetic events on the offscreen platform against an
 * unstarted session. The Controllers window has its own test
 * (controllers_test.cpp).
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
};

QTEST_MAIN(KeypadTest)
#include "keypad_test.moc"
