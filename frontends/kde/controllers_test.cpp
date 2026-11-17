/*
 * controllers_test -- the KDE Controllers window, offscreen: hidden at start,
 * fixed size, the mouse holding a key, a fire button and the stick (and
 * letting go when it drags off, and when the window hides), keys reaching the
 * console, Map taking a key and a gamepad button, F9 hiding it, and a pad's
 * row with its Automatic / Controller 1 / Controller 2 picker -- on an SDL
 * virtual gamepad, with no machine and no BIOS. Waits poll with a five second
 * ceiling, so a slow runner is merely slow. SKIPs (77) where SDL has no
 * virtual joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "adamsession.h"
#include "controllers/ControllersWindow.h"

/* the gamepad layer's start, which the session runs after a machine starts;
 * here it runs alone, so a pad is found with no BIOS */
extern "C" int gamepad_start(adamsession *sess);

namespace {

/* the window's drawing constants (ControllersWindow.cpp) */
constexpr double kUnit = 40.0;
constexpr int kCasePad = 18;
constexpr int kTop = 34;

int failures = 0;

void check(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

/* Waits until the controller holds exactly `want`; the window's own timers
 * and the pad thread both need the event loop. */
void expectHeld(adamsession *sess, int port, unsigned want, const char *what)
{
    unsigned held = 0;
    for (int tries = 0; tries < 250; tries++) {
        held = adamsession_buttons_held(sess, port);
        if (held == want) {
            std::printf("ok: %s\n", what);
            return;
        }
        QTest::qWait(20);
    }
    std::printf("FAIL: %s (held %05X want %05X)\n", what, held, want);
    failures++;
}

QWidget *findWindow()
{
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String("controllers")) return top;
    return nullptr;
}

/* the console widget (the two controllers) and the keyboard widget */
QWidget *findChildNamed(QWidget *win, const char *name)
{
    return win->findChild<QWidget *>(QString::fromLatin1(name), Qt::FindDirectChildrenOnly);
}
QWidget *findConsole(QWidget *win) { return findChildNamed(win, "console"); }

QPoint centreOf(const char *label, int port)
{
    int count = 0;
    float wide = 0, tall = 0;
    const adam_control *layout = adamsession_controls_layout(&count, &wide, &tall);
    for (int idx = 0; idx < count; idx++) {
        if (layout[idx].port != port || !layout[idx].label || std::strcmp(layout[idx].label, label) != 0)
            continue;
        return QPoint(int(kCasePad + (layout[idx].xpos + layout[idx].width / 2) * kUnit),
                      int(kTop + (layout[idx].ypos + layout[idx].height / 2) * kUnit));
    }
    return QPoint(-1, -1);
}

QPushButton *findButton(QWidget *win, const char *text)
{
    for (QPushButton *button : win->findChildren<QPushButton *>())
        if (button->text() == QLatin1String(text)) return button;
    return nullptr;
}

QComboBox *waitForPadRow(QWidget *win)
{
    for (int tries = 0; tries < 250; tries++) {
        for (QComboBox *box : win->findChildren<QComboBox *>())
            if (box->count() == 3 && box->itemText(1) == QLatin1String("Controller 1")) return box;
        QTest::qWait(20);
    }
    return nullptr;
}

void keyTests(adamsession *sess, QWidget *win)
{
    /* Keypad 8 steers controller 1 (the default); Keypad 0 is its left fire button */
    QTest::keyPress(win, Qt::Key_8, Qt::KeypadModifier);
    expectHeld(sess, 0, 1u << ADAM_ACT_UP, "a typed Keypad 8 steers controller 1");
    QTest::keyRelease(win, Qt::Key_8, Qt::KeypadModifier);
    expectHeld(sess, 0, 0, "and its release lets go");

    QTest::keyPress(win, Qt::Key_0, Qt::KeypadModifier);
    QTest::keyPress(win, Qt::Key_4, Qt::KeypadModifier);
    expectHeld(sess, 0, (1u << ADAM_ACT_FIRE_L) | (1u << ADAM_ACT_LEFT), "fire and a direction together");
    QTest::keyRelease(win, Qt::Key_4, Qt::KeypadModifier);
    QTest::keyRelease(win, Qt::Key_0, Qt::KeypadModifier);
    expectHeld(sess, 0, 0, "both released");

    /* an unbound key types on the ADAM instead of holding a control */
    QTest::keyClick(win, Qt::Key_A);
    expectHeld(sess, 0, 0, "an ordinary key holds no control");
}

void mouseTests(adamsession *sess, QWidget *win)
{
    QWidget *console = findConsole(win);
    check(console != nullptr, "the console widget exists");
    if (!console) return;
    const unsigned five = 1u << (ADAM_ACT_KEYPAD + 5);

    const QPoint key5 = centreOf("5", 0);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, key5);
    expectHeld(sess, 0, five, "pressing key 5 holds it");
    QTest::mouseMove(console, QPoint(2, 2));
    expectHeld(sess, 0, 0, "dragging off lets it go");
    QTest::mouseMove(console, key5);
    expectHeld(sess, 0, five, "and back on holds it again");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, key5);
    expectHeld(sess, 0, 0, "releasing lets go");

    const QPoint fire = centreOf("Right Fire", 1);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, fire);
    expectHeld(sess, 1, 1u << ADAM_ACT_FIRE_R, "the right fire button of controller 2");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, fire);

    /* the stick: a press tilts it, a drag moves on from one direction to the next */
    const QPoint stick = centreOf("Stick", 0);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, stick + QPoint(45, 0));
    expectHeld(sess, 0, 1u << ADAM_ACT_RIGHT, "a press right of the cap tilts right");
    QTest::mouseMove(console, stick + QPoint(0, -45));
    expectHeld(sess, 0, 1u << ADAM_ACT_UP, "dragging round moves to up");
    QTest::mouseMove(console, stick + QPoint(32, -32));
    expectHeld(sess, 0, (1u << ADAM_ACT_UP) | (1u << ADAM_ACT_RIGHT), "and to the up-right diagonal");
    win->hide();
    expectHeld(sess, 0, 0, "hiding the window lets go of what the mouse held");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, stick);
}

void mapTests(adamsession *sess, QWidget *win, SDL_Joystick *vjoy)
{
    QPushButton *mapButton = findButton(win, "Map");
    QWidget *console = findConsole(win);
    check(mapButton && console, "the Map button exists");
    if (!mapButton || !console) return;
    const int target = ADAM_TARGET_PORT(0, ADAM_ACT_KEYPAD + 5);

    mapButton->click();
    check(mapButton->text() == QLatin1String("Done"), "Map arms");
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("5", 0));
    QTest::keyClick(win, Qt::Key_Q);
    check(adamsession_binding_get(sess, target).keysym == 'q', "the next key is bound to the picked part");
    expectHeld(sess, 0, 0, "a key taken by Map is not typed into the console");
    QTest::keyClick(win, Qt::Key_F9);
    check(!win->isVisible(), "F9 hides the window");
    win->show();

    mapButton = findButton(win, "Map");
    check(mapButton != nullptr, "the window comes back");
    if (!mapButton) return;
    mapButton->click();
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("5", 0));
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        for (int tries = 0; tries < 250 && adamsession_binding_get(sess, target).button != ADAM_PAD_BTN_RIGHT_STICK; tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        check(adamsession_binding_get(sess, target).button == ADAM_PAD_BTN_RIGHT_STICK,
              "the next gamepad button is bound to the picked part");
    }
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::keyClick(win, Qt::Key_Escape);
    check(mapButton->text() == QLatin1String("Map"), "Escape twice leaves Map");

    adamsession_bindings_reset(sess);
    check(adamsession_binding_get(sess, target).keysym == 0, "Defaults put the key back (key 5 starts unbound)");
}

/* the ADAM keyboard under the controllers: a click sends the cap, SHIFT latches */
void keyboardTests(adamsession *sess, QWidget *win)
{
    QWidget *keys = findChildNamed(win, "keyboard");
    check(keys != nullptr, "the keyboard widget exists");
    if (!keys) return;
    adamsession_set_int(sess, "controllers_keyboard", 1);
    keys->show();
    int count = 0;
    float wide = 0, high = 0;
    const adam_keycap *caps = adamsession_keyboard_layout(&count, &wide, &high);
    auto centre = [&](const char *label) {
        for (int idx = 0; idx < count; idx++)
            if (std::strcmp(caps[idx].label, label) == 0)
                return QPoint(int(10 + (caps[idx].left + caps[idx].wide / 2) * 24.0),
                              int(16 + 10 + (caps[idx].top + caps[idx].high / 2) * 24.0));
        return QPoint(-1, -1);
    };
    auto indexOf = [&](const char *label) {
        for (int idx = 0; idx < count; idx++)
            if (std::strcmp(caps[idx].label, label) == 0) return idx;
        return -1;
    };
    const int shift = indexOf("SHIFT"), keyA = indexOf("A");
    check(shift >= 0 && keyA >= 0, "the layout has SHIFT and A");
    if (shift < 0 || keyA < 0) return;
    QTest::mousePress(keys, Qt::LeftButton, Qt::NoModifier, centre("A"));
    check(adamsession_keyboard_held(sess, keyA) & 1, "pressing a cap lights it");
    QTest::mouseRelease(keys, Qt::LeftButton, Qt::NoModifier, centre("A"));
    QTest::mouseClick(keys, Qt::LeftButton, Qt::NoModifier, centre("SHIFT"));
    check(adamsession_keyboard_held(sess, shift) & 2, "SHIFT latches");
    QTest::mouseClick(keys, Qt::LeftButton, Qt::NoModifier, centre("A"));
    check(!(adamsession_keyboard_held(sess, shift) & 2), "and the next key spends it");
    win->hide();
    win->show();
    check(!(adamsession_keyboard_held(sess, shift) & 2), "hiding the window leaves nothing latched");
}

void padRowTests(adamsession *sess, QWidget *win, const char *padName)
{
    QComboBox *box = waitForPadRow(win);
    check(box != nullptr, "a pad's row appears with its three choices");
    if (!box) return;
    int slot = -1;
    for (int idx = 0; idx < adamsession_gamepad_count(sess); idx++) {
        char got[64];
        adamsession_gamepad_name(sess, idx, got, sizeof got);
        if (std::strcmp(got, padName) == 0) slot = idx;
    }
    check(slot >= 0, "the pad is in the session");
    if (slot < 0) return;
    check(box->currentIndex() == 0, "it starts on Automatic");
    box->setCurrentIndex(2);
    check(adamsession_gamepad_assignment(sess, slot) == 1, "choosing Controller 2 assigns it");
    check(adamsession_gamepad_effective_port(sess, slot) == 1, "and it drives controller 2");
    box->setCurrentIndex(0);
    check(adamsession_gamepad_assignment(sess, slot) == -1, "Automatic clears the assignment");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir config, data;
    if (!config.isValid() || !data.isValid()) return 1;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    static const char padName[] = "adamtest kde pad";
    desc.name = padName;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Joystick *vjoy = SDL_OpenJoystick(vid);

    const QByteArray cfg = config.path().toLocal8Bit(), dat = data.path().toLocal8Bit();
    adamsession_paths paths;
    std::memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg.constData();
    paths.data_dir = dat.constData();
    paths.fujinet_lib = "";
    adamsession *sess = adamsession_new(&paths);
    if (!sess || gamepad_start(sess) != 0) {
        std::printf("SKIP: the gamepad layer would not start\n");
        return 77;
    }

    QWidget host;
    check(!adamControllersVisible(), "hidden at start");
    adamControllersToggle(&host, sess);
    check(adamControllersVisible(), "F9 / the menu shows it");
    QWidget *win = findWindow();
    check(win != nullptr, "the window exists");
    if (win) {
        check(win->windowTitle() == QLatin1String("Controllers"), "titled Controllers");
        check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
              "fixed size: minimum == maximum == size");
        keyTests(sess, win);
        mouseTests(sess, win);
        win->show();
        mapTests(sess, win, vjoy);
        keyboardTests(sess, win);
        padRowTests(sess, win, padName);
        adamControllersToggle(&host, sess);
        check(!adamControllersVisible(), "toggling again hides it");
    }

    if (vjoy) SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    adamsession_free(sess);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
