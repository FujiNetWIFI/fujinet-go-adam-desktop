/*
 * The Input settings window: gamepads on or off, the swapped controller
 * buttons, and which controller each connected pad drives -- over the same
 * settings keys as the other frontends' Preferences ▸ Input, so a machine
 * configured in one comes up the same in another.
 *
 * Enabling the gamepads and swapping the buttons wait for the window to
 * close, which hands the main window one "dirty" flag so it restarts the
 * session once. The gamepad assignments apply at once.
 *
 * Laid out in 96-dpi units through adam_scale() and rebuilt on
 * WM_DPICHANGED.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "controllers/controllers_window.h"
#include "help/help_window.h"
#include "resource.h"

#define PREFS_CLASS "FujiNetGoAdamPrefs"
#define PREFS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define MAX_ITEMS 24

/* Layout, in 96-dpi units. */
#define PREFS_W 520         /* client width */
#define MARGIN 14
#define PAGE_H 296

enum {
    IDC_GAMEPAD = 100, IDC_SWAP, IDC_PAD_LIST, IDC_PAD_PORT, IDC_RESET_CONTROLS, IDC_OPEN_CONTROLLERS,
    IDC_PREFS_HELP, IDC_CLOSE
};

#define IDT_PADS 1

typedef struct {
    HWND ctrl;
    int xpos, ypos, width, height;      /* 96 dpi */
    int bold;               /* a heading */
} item;

static HWND g_win, g_close, g_help;
static HWND g_pad_list, g_pad_port, g_input_note;
static item g_items[MAX_ITEMS];
static int g_numitems;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static adamsession *g_session;
static HWND g_parent;
static adam_prefs_hooks g_hooks;
static int g_dirty;
static unsigned g_pad_seen;

static int SCL(int val) { return adam_scale(val, g_dpi); }

/* ---- building -------------------------------------------------------------- */

static HWND add(const wchar_t *wclass, const char *utf8, DWORD style, int ident,
                int xpos, int ypos, int width, int height)
{
    wchar_t *text = adam_wide_dup(utf8 ? utf8 : "");
    HWND ctrl;
    if (g_numitems >= MAX_ITEMS) { free(text); return NULL; }
    ctrl = CreateWindowExW(0, wclass, text ? text : L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_win,
                           (HMENU)(INT_PTR)ident, GetModuleHandleA(NULL), NULL);
    free(text);
    g_items[g_numitems].ctrl = ctrl;
    g_items[g_numitems].xpos = xpos;
    g_items[g_numitems].ypos = ypos;
    g_items[g_numitems].width = width;
    g_items[g_numitems].height = height;
    g_items[g_numitems].bold = 0;
    g_numitems++;
    return ctrl;
}

static HWND label(const char *text, int xpos, int ypos, int width, int height)
{
    return add(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 0, xpos, ypos, width, height);
}

static HWND heading(const char *text, int ypos)
{
    HWND ctrl = label(text, MARGIN, ypos, PREFS_W - 2 * MARGIN, 18);
    if (ctrl) g_items[g_numitems - 1].bold = 1;
    return ctrl;
}

static HWND checkbox(int ident, const char *text, int xpos, int ypos, int width, int checked)
{
    HWND ctrl = add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, ident, xpos, ypos, width, 22);
    if (ctrl) SendMessageW(ctrl, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return ctrl;
}

static HWND button(int ident, const char *text, int xpos, int ypos, int width)
{
    return add(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, ident, xpos, ypos, width, 28);
}

static void combo_add(HWND ctrl, const char *text)
{
    wchar_t *wide = adam_wide_dup(text);
    if (wide) { SendMessageW(ctrl, CB_ADDSTRING, 0, (LPARAM)wide); free(wide); }
}

static void set_text(HWND ctrl, const char *utf8)
{
    wchar_t current[512], *wide = adam_wide_dup(utf8);
    if (!wide) return;
    GetWindowTextW(ctrl, current, 512);
    if (wcscmp(current, wide) != 0) SetWindowTextW(ctrl, wide);
    free(wide);
}

/* ---- input ------------------------------------------------------------------------ */

static void refresh_pad_list(void)
{
    int idx, num, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    num = adamsession_gamepad_count(g_session);
    if (num == 0)
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected - plug one in)");
    for (idx = 0; idx < num; idx++) {
        char name[128], line[220];
        wchar_t wide[220];
        const int effective = adamsession_gamepad_effective_port(g_session, idx);
        adamsession_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0)
            snprintf(line, sizeof line, "%s  [controller %d%s]", name, effective + 1,
                     adamsession_gamepad_assignment(g_session, idx) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 220);
        SendMessageW(g_pad_list, LB_ADDSTRING, 0, (LPARAM)wide);
    }
    if (sel >= 0 && sel < num) {
        SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(adamsession_gamepad_assignment(g_session, sel) + 1), 0);
    }
    g_pad_seen = adamsession_gamepad_generation(g_session);
}

static void build_input(void)
{
    int ypos = 6;
    heading("Input", ypos); ypos += 24;
    checkbox(IDC_GAMEPAD, "Enable gamepads", MARGIN, ypos, 340,
             adamsession_get_int(g_session, "enable_gamepad", 1));
    ypos += 28;
    checkbox(IDC_SWAP, "Swap controller buttons", MARGIN, ypos, 340,
             adamsession_get_int(g_session, "swap_buttons", 0));
    ypos += 36;
    label("Gamepads (select one, then choose the controller it drives):", MARGIN, ypos,
          PREFS_W - 2 * MARGIN, 18);
    ypos += 24;
    g_pad_list = add(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP | WS_BORDER,
                     IDC_PAD_LIST, MARGIN, ypos, 340, 84);
    g_pad_port = add(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_PAD_PORT,
                     MARGIN + 350, ypos, 142, 220);
    combo_add(g_pad_port, "Automatic");
    combo_add(g_pad_port, "Controller 1");
    combo_add(g_pad_port, "Controller 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    ypos += 96;
    g_input_note = label("Keys and gamepad buttons are re-mapped in the Controllers window (F9).",
                         MARGIN, ypos, PREFS_W - 2 * MARGIN, 36);
    ypos += 44;
    button(IDC_RESET_CONTROLS, "Reset Controls", MARGIN, ypos, 130);
    button(IDC_OPEN_CONTROLLERS, "Open Controllers", MARGIN + 140, ypos, 150);
}

/* ---- layout ------------------------------------------------------------------------ */

/* Places everything at the current DPI and sizes the window to its
 * contents. */
static void layout(void)
{
    int idx;
    const int top = SCL(8);
    for (idx = 0; idx < g_numitems; idx++) {
        const item *entry = &g_items[idx];
        if (!entry->ctrl) continue;
        MoveWindow(entry->ctrl, SCL(entry->xpos), top + SCL(entry->ypos), SCL(entry->width), SCL(entry->height), TRUE);
    }
    MoveWindow(g_help, SCL(MARGIN), top + SCL(PAGE_H) + SCL(6), SCL(100), SCL(28), TRUE);
    MoveWindow(g_close, SCL(PREFS_W) - SCL(MARGIN) - SCL(100), top + SCL(PAGE_H) + SCL(6), SCL(100), SCL(28), TRUE);
    {
        const SIZE outer = adam_outer_size(g_win, SCL(PREFS_W), top + SCL(PAGE_H) + SCL(6) + SCL(28) + SCL(MARGIN), FALSE);
        SetWindowPos(g_win, NULL, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* The window's fonts at the current DPI: plain on every child, bold on the
 * headings. */
static void make_fonts(void)
{
    int idx;
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    g_font = adam_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = adam_make_font(g_dpi, 0, FW_BOLD, NULL);
    adam_set_font_all(g_win, g_font);
    for (idx = 0; idx < g_numitems; idx++)
        if (g_items[idx].bold) SendMessageW(g_items[idx].ctrl, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* ---- actions -------------------------------------------------------------------------- */

static void setting_from_checkbox(int ident, const char *key, int def)
{
    const int checked = SendDlgItemMessageA(g_win, ident, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (adamsession_get_int(g_session, key, def) != checked) {
        adamsession_set_int(g_session, key, checked);
        g_dirty = 1;
    }
}

static void on_command(int ident, int code)
{
    switch (ident) {
    case IDC_GAMEPAD: setting_from_checkbox(ident, "enable_gamepad", 1); break;
    case IDC_SWAP: setting_from_checkbox(ident, "swap_buttons", 0); break;
    case IDC_PAD_LIST:
        if (code == LBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < adamsession_gamepad_count(g_session))
                SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(adamsession_gamepad_assignment(g_session, sel) + 1), 0);
        }
        break;
    case IDC_PAD_PORT:
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            const int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < adamsession_gamepad_count(g_session))
                adamsession_gamepad_assign(g_session, sel, choice - 1);
            refresh_pad_list();
            adam_controllers_window_refresh();
        }
        break;
    case IDC_RESET_CONTROLS:
        adamsession_bindings_reset(g_session);
        adam_controllers_window_refresh();
        set_text(g_input_note, "Every key and gamepad button is back to its default.");
        break;
    case IDC_OPEN_CONTROLLERS:
        adam_controllers_window_show(g_parent, g_session);
        break;
    default: break;
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_PREFS_HELP) { adam_help_for(g_win); return 0; }
        if (LOWORD(wparam) == IDC_CLOSE || LOWORD(wparam) == IDCANCEL) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (LOWORD(wparam) != IDOK) on_command(LOWORD(wparam), HIWORD(wparam));
        return 0;
    case WM_TIMER:
        if (wparam == IDT_PADS && adamsession_gamepad_generation(g_session) != g_pad_seen)
            refresh_pad_list();
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wparam);
        make_fonts();
        layout();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        const int dirty = g_dirty;
        KillTimer(hwnd, IDT_PADS);
        g_win = NULL;
        g_close = g_help = g_pad_list = g_pad_port = g_input_note = NULL;
        g_numitems = 0;
        g_dirty = 0;
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        g_font = g_bold = NULL;
        if (g_hooks.closed) g_hooks.closed(dirty);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void adam_prefs_show(HWND parent, adamsession *session, const adam_prefs_hooks *hooks)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_parent = parent;
    g_hooks = *hooks;
    if (!registered) {
        WNDCLASSEXW wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = prefs_proc;
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = L"" PREFS_CLASS;
        wcls.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExW(&wcls) != 0;
    }
    g_dpi = adam_window_dpi(parent);
    g_numitems = 0;
    g_dirty = 0;
    g_win = CreateWindowExW(WS_EX_CONTROLPARENT, L"" PREFS_CLASS, L"Input Settings", PREFS_STYLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, parent, NULL, inst, NULL);
    if (!g_win) return;

    build_input();
    label("Applied by restarting the session when this window closes.", MARGIN + 110, PAGE_H + 6,
          PREFS_W - 2 * MARGIN - 220, 30);
    g_help = CreateWindowExW(0, L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_PREFS_HELP, inst, NULL);
    g_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_CLOSE, inst, NULL);
    /* Help, F1 and Ctrl+Shift+H open the Input page's own */
    adam_help_set_context(g_win, "prefs/Input", 1);

    make_fonts();
    refresh_pad_list();
    layout();
    SetTimer(g_win, IDT_PADS, 1000, NULL);
    ShowWindow(g_win, SW_SHOW);
}

void adam_prefs_abandon(void)
{
    if (!g_win) return;
    g_hooks.closed = NULL;
    DestroyWindow(g_win);
}

void adam_prefs_gamepads_changed(void)
{
    if (g_win) refresh_pad_list();
}

int adam_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    /* F1 and the Ctrl shortcuts (Ctrl+Shift+H, Help) are the app's, not a dialog's */
    if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
        (msg->wParam == VK_F1 || GetKeyState(VK_CONTROL) < 0))
        return 0;
    return IsDialogMessageW(g_win, msg) ? 1 : 0;
}
