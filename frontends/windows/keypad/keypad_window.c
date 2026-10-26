/*
 * The Win32 Controllers window: both ColecoVision hand controllers side by
 * side -- a 3x4 keypad, a four-way stick and Fire L / Fire R each -- then the
 * System row (Reset Computer, Reset Game) and the Map row.
 *
 * One window draws every button itself, so none of them can take the keyboard
 * focus: keys typed here reach the window and go down the same path as the
 * main window's (keys.c), except in Map mode. A control lights in the accent
 * colour whenever the machine sees it held, from whatever source (keyboard,
 * gamepad, or a click here), so the window doubles as an input tester.
 *
 * Controls are driven by WM_LBUTTONDOWN / WM_LBUTTONUP, not BN_CLICKED: a
 * keypad key or fire button is HELD, and a game polls it, so a value present
 * only for the instant of a click falls between frames. The mouse is captured
 * on press and released on button-up wherever that happens, so dragging off a
 * control cannot strand the machine with it held. The two resets act on
 * release, over the button.
 *
 * Map: click Map (accent while armed), click a control to pick it -- every
 * control then shows the key bound to it -- and press a key or a gamepad
 * button to bind it (any key, modifiers and Escape included; F9-F12 belong to
 * the app). Clear unbinds the picked control's key and pad button, Defaults
 * restores them all, and leaving Map (or hiding the window) cancels.
 *
 * Fixed size and floating: a tool window with a caption and no thick frame or
 * maximize box, scaled to the monitor's DPI.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <stdio.h>
#include <string.h>

#include "../accent_button.h"
#include "../keys.h"
#include "../resource.h"

#define PAD_CLASS "AdamKeypadWindow"
#define IDT_TICK 1                 /* held highlights + gamepad capture poll */

#define S(v) MulDiv((v), g_dpi, 96)

enum { K_CONTROL, K_MAP, K_CLEAR, K_DEFAULTS };
enum { MAP_OFF, MAP_ARMED, MAP_PICKED };

typedef struct {
    RECT rc;
    int kind;
    int target;                    /* K_CONTROL: an ADAM_TARGET_* */
    int wide;                      /* label carries the key under its name */
    char face[24];                 /* UTF-8 */
} pad_button;

static HWND g_panel;
static adamsession *g_session;
static int g_dpi = 96;
static pad_button g_btn[ADAM_TARGET_COUNT + 3];
static int g_nbtn;
static int g_held = -1;            /* control under the captured mouse */
static int g_hot = -1;             /* control under the pointer */
static int g_map = MAP_OFF;
static int g_map_target = -1;
static RECT g_head_rc[2], g_hint_rc;
static uint32_t g_shown_held[2];
static char g_hint[240];
static HFONT g_font, g_bold, g_big;   /* the stick's arrows are drawn larger */
static HTHEME g_theme;
static int g_total_w, g_total_h;

/* ---- layout -------------------------------------------------------------- */

static int dpi_for(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    UINT dpi = 0;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd)
        dpi = fn(hwnd);
    if (!dpi) {
        HDC dc = GetDC(NULL);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return dpi ? (int)dpi : 96;
}

static void add_button(int kind, int target, const char *face, int wide, int x, int y, int w, int h)
{
    pad_button *b;
    if (g_nbtn >= (int)(sizeof g_btn / sizeof g_btn[0]))
        return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + h);
    b->kind = kind;
    b->target = target;
    b->wide = wide;
    snprintf(b->face, sizeof b->face, "%s", face);
}

/* The keypad as on the hand controller: 1-9 in reading order, then * 0 #
 * (ADAM_ACT_KEYPAD + 0..9 are the digits, 10 '*', 11 '#'). */
static const int k_order[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
static const char *const k_face[12] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#" };

/* One controller, headed and ready for its keys; returns the y below it. */
static int build_controller(int port, int x0, int y0, int bw, int bh, int gap)
{
    const int col = bw + gap, row = bh + gap, pw = 3 * bw + 2 * gap;
    int y = y0, i;

    SetRect(&g_head_rc[port], x0, y, x0 + pw, y + S(20));
    y += S(20) + gap;

    for (i = 0; i < 12; i++)
        add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + k_order[i]), k_face[k_order[i]], 0,
                   x0 + (i % 3) * col, y + (i / 3) * row, bw, bh);
    y += 4 * row + gap;

    /* the stick, as a cross */
    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_UP), "\xE2\x86\x91", 0, x0 + col, y, bw, bh);
    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_LEFT), "\xE2\x86\x90", 0, x0, y + row, bw, bh);
    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_RIGHT), "\xE2\x86\x92", 0, x0 + 2 * col, y + row, bw, bh);
    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_DOWN), "\xE2\x86\x93", 0, x0 + col, y + 2 * row, bw, bh);
    y += 3 * row + gap;

    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_L), "Fire L", 1, x0, y, (pw - gap) / 2, bh);
    add_button(K_CONTROL, ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_R), "Fire R", 1, x0 + (pw - gap) / 2 + gap, y,
               pw - (pw - gap) / 2 - gap, bh);
    return y + bh;
}

static void layout(void)
{
    const int bw = S(58), bh = S(34), gap = S(4), margin = S(10);
    const int pw = 3 * bw + 2 * gap;
    int y, w;

    g_nbtn = 0;
    y = build_controller(0, margin, margin, bw, bh, gap);
    build_controller(1, margin + pw + S(24), margin, bw, bh, gap);
    g_total_w = 2 * margin + 2 * pw + S(24);
    w = g_total_w - 2 * margin;

    y += 3 * gap;
    add_button(K_CONTROL, ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_COMPUTER), "Reset Computer", 1, margin, y,
               (w - gap) / 2, bh);
    add_button(K_CONTROL, ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME), "Reset Game", 1, margin + (w - gap) / 2 + gap,
               y, w - (w - gap) / 2 - gap, bh);
    y += bh + 3 * gap;

    add_button(K_MAP, -1, "Map", 0, margin, y, S(80), S(28));
    add_button(K_CLEAR, -1, "Clear", 0, margin + S(84), y, S(80), S(28));
    add_button(K_DEFAULTS, -1, "Defaults", 0, margin + 2 * S(84), y, S(80), S(28));
    y += S(28) + gap;

    SetRect(&g_hint_rc, margin, y, g_total_w - margin, y + S(34));
    g_total_h = y + S(34) + margin;
}

static void make_fonts(void)
{
    LOGFONTA lf;
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    if (g_big) DeleteObject(g_big);
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(9, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "Segoe UI");
    g_font = CreateFontIndirectA(&lf);
    lf.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectA(&lf);
    lf.lfWeight = FW_NORMAL;
    lf.lfHeight = -MulDiv(13, g_dpi, 72);
    g_big = CreateFontIndirectA(&lf);
}

/* The window's outer size for a client of g_total_w x g_total_h at g_dpi. */
static void outer_size(int *w, int *h)
{
    typedef BOOL (WINAPI *adjust_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static adjust_fn fn;
    static int looked;
    RECT want;
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (adjust_fn)(void (*)(void))GetProcAddress(user, "AdjustWindowRectExForDpi") : NULL;
        looked = 1;
    }
    SetRect(&want, 0, 0, g_total_w, g_total_h);
    if (!fn || !fn(&want, style, FALSE, WS_EX_TOOLWINDOW, (UINT)g_dpi))
        AdjustWindowRectEx(&want, style, FALSE, WS_EX_TOOLWINDOW);
    *w = want.right - want.left;
    *h = want.bottom - want.top;
}

/* ---- state --------------------------------------------------------------- */

static int hit(int x, int y)
{
    POINT p;
    int i;
    p.x = x;
    p.y = y;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void key_text(uint32_t keysym, char *out, int outsz)
{
    adamsession_keysym_name(keysym, out, outsz);
    if (!out[0])
        snprintf(out, (size_t)outsz, "\xE2\x80\x94");    /* an em dash: unbound */
}

/* "Keypad 8, pad D-pad Up", or "nothing". */
static void binding_text(int target, char *out, int outsz)
{
    const adam_binding b = adamsession_binding_get(g_session, target);
    char key[48];
    adamsession_keysym_name(b.keysym, key, sizeof key);
    if (key[0] && b.button != ADAM_PAD_BTN_NONE)
        snprintf(out, (size_t)outsz, "%s, pad %s", key, adam_pad_button_name(b.button));
    else if (key[0])
        snprintf(out, (size_t)outsz, "%s", key);
    else if (b.button != ADAM_PAD_BTN_NONE)
        snprintf(out, (size_t)outsz, "pad %s", adam_pad_button_name(b.button));
    else
        snprintf(out, (size_t)outsz, "nothing");
}

static void set_map(int state, int target)
{
    g_map = state;
    g_map_target = state == MAP_PICKED ? target : -1;
    adamsession_gamepad_capture_cancel(g_session);
    if (state == MAP_OFF) {
        g_hint[0] = '\0';
    } else if (state == MAP_ARMED) {
        snprintf(g_hint, sizeof g_hint, "Click a control to remap");
    } else {
        char name[64], now[96];
        snprintf(name, sizeof name, "%s", adam_target_name(target));     /* a static buffer */
        binding_text(target, now, sizeof now);
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s (now %s)", name, now);
        adamsession_gamepad_capture_begin(g_session);
    }
    InvalidateRect(g_panel, NULL, FALSE);
}

/* After a rebind: name what the control now does, and whom it was taken from. */
static void bound_hint(int target, const char *what, const char *stolen)
{
    char name[64];
    snprintf(name, sizeof name, "%s", adam_target_name(target));
    if (stolen[0])
        snprintf(g_hint, sizeof g_hint, "%s = %s (taken from %s)", name, what, stolen);
    else
        snprintf(g_hint, sizeof g_hint, "%s = %s", name, what);
}

static void press_target(int target, int down)
{
    /* A system target acts on release (a real button, and dragging off must
     * not reset the machine); the session's own press would act on press. */
    if (target >= ADAM_TARGET_SYSACT(0))
        return;
    adamsession_press(g_session, target, down);
}

static void refresh_held(void)
{
    int port, changed = 0;
    for (port = 0; port < 2; port++) {
        const uint32_t now = adamsession_buttons_held(g_session, port);
        if (now != g_shown_held[port]) {
            g_shown_held[port] = now;
            changed = 1;
        }
    }
    if (changed) InvalidateRect(g_panel, NULL, FALSE);
}

static void poll_capture(void)
{
    int button;
    if (g_map != MAP_PICKED || !adamsession_gamepad_capture_poll(g_session, &button))
        return;
    {
        char stolen[128], what[64];
        const int target = g_map_target;
        stolen[0] = '\0';
        adamsession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
        snprintf(what, sizeof what, "pad %s", adam_pad_button_name(button));
        set_map(MAP_ARMED, -1);    /* stay armed: remapping several in a row is normal */
        bound_hint(target, what, stolen);
        InvalidateRect(g_panel, NULL, FALSE);
    }
}

/* ---- painting ------------------------------------------------------------ */

static int draw_text(HDC dc, const char *utf8, RECT *rc, UINT flags)
{
    WCHAR w[160];
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 160);
    return n > 0 ? DrawTextW(dc, w, n - 1, rc, flags) : 0;
}

/* Vertically centred, broken at spaces and at the newline a wide control's
 * Map label carries between its name and its key. */
static void draw_label(HDC dc, const RECT *r, const char *label, int nudge)
{
    RECT t = *r;
    int h;
    InflateRect(&t, -S(2), 0);
    h = draw_text(dc, label, &t, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    t.left = r->left + S(2);
    t.right = r->right - S(2);
    t.top = r->top + ((r->bottom - r->top) - h) / 2 + nudge;
    t.bottom = r->bottom;
    draw_text(dc, label, &t, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int hot, int accent)
{
    RECT r = *rc;
    if (accent) {
        FillRect(dc, &r, hot ? adam_accent_hot_brush() : adam_accent_brush());
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, ADAM_ACCENT_TEXT);
    } else {
        if (g_theme)
            DrawThemeBackground(g_theme, dc, BP_PUSHBUTTON, pushed ? PBS_PRESSED : hot ? PBS_HOT : PBS_NORMAL, &r,
                                NULL);
        else
            DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
    draw_label(dc, &r, label, pushed ? 1 : 0);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (b->kind == K_CONTROL && g_map != MAP_OFF) {
        char key[48];
        key_text(adamsession_binding_get(g_session, b->target).keysym, key, sizeof key);
        if (b->wide)
            snprintf(out, (size_t)outsz, "%s\n%s", b->face, key);
        else
            snprintf(out, (size_t)outsz, "%s", key);
    } else {
        snprintf(out, (size_t)outsz, "%s", b->face);
    }
}

static void paint_panel(HDC dc, const RECT *client)
{
    HGDIOBJ old = SelectObject(dc, g_font);
    int i;

    FillRect(dc, client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    draw_text(dc, "Controller 1", &g_head_rc[0], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    draw_text(dc, "Controller 2", &g_head_rc[1], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_font);

    for (i = 0; i < g_nbtn; i++) {
        const pad_button *b = &g_btn[i];
        char label[96];
        int accent = 0, pushed = i == g_held;
        if (b->kind == K_MAP) {
            accent = g_map != MAP_OFF;
            pushed = 0;
        } else if (b->kind == K_CONTROL) {
            if (g_map == MAP_OFF && b->target < ADAM_TARGET_SYSACT(0))
                accent = ((g_shown_held[b->target / ADAM_ACT_PER_PORT] >> (b->target % ADAM_ACT_PER_PORT)) & 1) ||
                         pushed;
            else if (g_map == MAP_PICKED)
                accent = b->target == g_map_target;
        } else {
            pushed = 0;
        }
        button_label(b, label, sizeof label);
        if (b->kind == K_CONTROL && g_map == MAP_OFF && b->target < ADAM_TARGET_SYSACT(0) &&
            b->target % ADAM_ACT_PER_PORT >= ADAM_ACT_UP && b->target % ADAM_ACT_PER_PORT <= ADAM_ACT_RIGHT)
            SelectObject(dc, g_big);
        draw_button(dc, &b->rc, label, pushed, i == g_hot && g_held < 0, accent);
        SelectObject(dc, g_font);
    }

    if (g_hint[0]) {
        RECT r = g_hint_rc;
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        draw_text(dc, g_hint, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

/* ---- keyboard ------------------------------------------------------------ */

static int is_hotkey(WPARAM vk)
{
    return vk == VK_F9 || vk == VK_F11 || vk == VK_F12;
}

/* Same path as the main window, except in Map mode: armed, a key goes to Map
 * and not the machine; with a control picked, the next key binds. */
static int on_key(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const int press = down && !(lp & (1 << 30));

    if (down && wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return 0;                       /* Alt+F4 closes the window */

    if (g_map == MAP_PICKED && wp != VK_F10 && !is_hotkey(wp)) {
        if (press) {
            const uint32_t ks = adam_win_keysym(wp, lp);
            if (ks) {
                char stolen[128], name[48];
                const int target = g_map_target;
                stolen[0] = '\0';
                adamsession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
                key_text(ks, name, sizeof name);
                set_map(MAP_ARMED, -1);
                bound_hint(target, name, stolen);
            } else {
                snprintf(g_hint, sizeof g_hint, "That key has no name here. Press another.");
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 1;
    }
    if (g_map == MAP_PICKED && (wp == VK_F10 || is_hotkey(wp))) {
        if (press) {
            snprintf(g_hint, sizeof g_hint, "F9-F12 belong to the app. Press another key or gamepad button.");
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 1;
    }

    if (is_hotkey(wp)) {
        /* the app's own hotkeys act as in the main window */
        HWND owner = GetWindow(hwnd, GW_OWNER);
        if (press && owner)
            SendMessageA(owner, WM_KEYDOWN, wp, lp);
        return 1;
    }
    if (g_map == MAP_ARMED) {
        if (press && wp == VK_ESCAPE)
            set_map(MAP_OFF, -1);
        return wp != VK_F10;
    }
    if (wp == VK_F10)
        return 0;
    return adam_win_key_msg(g_session, msg, wp, lp);
}

/* ---- mouse --------------------------------------------------------------- */

static void click_control(HWND hwnd, int i)
{
    const pad_button *b = &g_btn[i];

    switch (b->kind) {
    case K_MAP:
        set_map(g_map == MAP_OFF ? MAP_ARMED : MAP_OFF, -1);
        return;
    case K_CLEAR:
        if (g_map == MAP_PICKED) {
            char name[64];
            const int target = g_map_target;
            adamsession_binding_set_key(g_session, target, 0, NULL, 0);
            adamsession_binding_set_button(g_session, target, ADAM_PAD_BTN_NONE, NULL, 0);
            snprintf(name, sizeof name, "%s", adam_target_name(target));
            set_map(MAP_ARMED, -1);
            snprintf(g_hint, sizeof g_hint, "%s cleared", name);
        } else {
            snprintf(g_hint, sizeof g_hint, "Turn Map on and click a control, then Clear");
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    case K_DEFAULTS:
        adamsession_bindings_reset(g_session);
        if (g_map == MAP_PICKED)
            set_map(MAP_ARMED, -1);
        snprintf(g_hint, sizeof g_hint, "Every key and button is back to its default");
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    default:
        break;
    }

    if (g_map != MAP_OFF) {
        set_map(MAP_PICKED, b->target);
        return;
    }
    g_held = i;
    SetCapture(hwnd);
    press_target(b->target, 1);
    InvalidateRect(hwnd, &b->rc, FALSE);
}

/* Releases the held control; fire says whether a system action goes off. */
static void release_held(HWND hwnd, int fire)
{
    const pad_button b = g_btn[g_held];
    g_held = -1;
    if (b.target >= ADAM_TARGET_SYSACT(0)) {
        if (fire)
            adamsession_sysaction(g_session, b.target - ADAM_TARGET_SYSACT(0));
    } else {
        press_target(b.target, 0);
    }
    InvalidateRect(hwnd, &b.rc, FALSE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        paint_panel(mem, &c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int i = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        if (i >= 0 && g_held < 0)
            click_control(hwnd, i);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the control. */
        if (g_held >= 0) {
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            release_held(hwnd, PtInRect(&g_btn[g_held].rc, p));
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        /* Lost the mouse some other way (Alt+Tab mid-press): let go, without
         * firing a system action nobody finished clicking. */
        if (g_held >= 0 && (HWND)lp != hwnd)
            release_held(hwnd, 0);
        break;
    case WM_MOUSEMOVE: {
        const int i = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i != g_hot) {
            TRACKMOUSEEVENT t;
            g_hot = i;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd;
            TrackMouseEvent(&t);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hot >= 0) {
            g_hot = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_TICK) {
            refresh_held();
            poll_capture();
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (on_key(hwnd, msg, wp, lp))
            return 0;
        break;
    case WM_CHAR:
        if (g_map == MAP_OFF)
            adam_win_char(g_session, wp);
        return 0;
    case WM_SYSCHAR:
        if (g_map != MAP_OFF)
            return 0;
        break;
    case WM_KILLFOCUS:
        adamsession_release_all(g_session);
        break;

    case WM_SHOWWINDOW:
        if (wp) {
            refresh_held();
            SetTimer(hwnd, IDT_TICK, 50, NULL);
        } else {
            KillTimer(hwnd, IDT_TICK);
            if (g_held >= 0) {
                release_held(hwnd, 0);
                ReleaseCapture();
            }
            set_map(MAP_OFF, -1);
        }
        break;
    case WM_THEMECHANGED:
        if (g_theme) CloseThemeData(g_theme);
        g_theme = OpenThemeData(hwnd, L"BUTTON");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const RECT *sug = (const RECT *)lp;
        int w, h;
        g_dpi = LOWORD(wp);
        make_fonts();
        layout();
        outer_size(&w, &h);
        if (g_theme) CloseThemeData(g_theme);
        g_theme = OpenThemeData(hwnd, L"BUTTON");
        SetWindowPos(hwnd, NULL, sug->left, sug->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void adam_keypad_window_toggle(HWND parent, adamsession *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        int w, h;

        g_dpi = dpi_for(parent);
        make_fonts();
        layout();

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(IDI_APPICON));
        wc.hIconSm = wc.hIcon;
        RegisterClassExA(&wc);

        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also keeps
         * it off the taskbar. */
        outer_size(&w, &h);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Controllers",
                                  WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel)
            return;
        g_theme = OpenThemeData(g_panel, L"BUTTON");
    }

    if (IsWindowVisible(g_panel)) {
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
        SetFocus(g_panel);
    }
}
