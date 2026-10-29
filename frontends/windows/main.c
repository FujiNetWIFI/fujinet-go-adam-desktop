/*
 * FujiNet Go Adam -- native Win32 frontend.
 *
 * Mirrors the GTK/Qt/AppKit frontends over the shared adamsession API: a
 * Direct3D 11 display (GDI as the fallback, see display.c) letterboxed to
 * the chosen aspect, whose render thread feeds the session's vsync
 * phase-lock (the Windows analog of the GTK frame clock / CVDisplayLink),
 * full keyboard mapping through the remappable controller bindings (keys.c),
 * a menu bar, media import, the Controllers window (F9), and the FujiNet
 * configuration (default browser) and console-log windows. Gamepads are
 * handled inside the core's SDL thread. No on-screen input panels appear
 * unless the user asks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamsession.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "keypad/keypad_window.h"
#include "keys.h"
#include "resource.h"

static adamsession *g_session;
static HWND g_hwnd;
static HWND g_display;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

static int g_aspect_mode;
static int g_smooth;
static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement;

/* ---- display ------------------------------------------------------------- */

/* The picture fills the client. */
static void layout_display(void)
{
    RECT rc;
    if (!g_display)
        return;
    GetClientRect(g_hwnd, &rc);
    MoveWindow(g_display, 0, 0, rc.right, rc.bottom, TRUE);
}

/* ---- keyboard ------------------------------------------------------------ */

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);
static void open_keypad(void);

/* F9 / F11 / F12 are the app's own and never reach the controller bindings
 * (Map mode refuses them); F10 stays with the system menu. */
static int on_key(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int press =
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30));

    switch (wp) {
    case VK_F10:
        return 0; /* let the system open the menu */
    case VK_F9:
        if (press) open_keypad();
        return 1;
    case VK_F11:
        if (press) toggle_fullscreen(hwnd);
        return 1;
    case VK_F12:
        if (press) open_debugger();
        return 1;
    default:
        return adam_win_key_msg(g_session, msg, wp, lp);
    }
}

/* ---- menu actions -------------------------------------------------------- */

static void restart_session(void)
{
    adamsession_start_opts opts;
    adamsession_stop(g_session);
    adamsession_default_opts(g_session, &opts);
    if (adamsession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, adamsession_last_error(g_session),
                    "FujiNet Go Adam", MB_ICONWARNING);
}

static int open_file(HWND hwnd, const char *filter, char *out, int outsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = outsz;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void import_media(HWND hwnd)
{
    char src[MAX_PATH], dest[1024];
    if (!open_file(hwnd,
                   "ADAM disk / data pack (*.dsk;*.ddp)\0*.dsk;*.ddp\0", src,
                   sizeof(src)))
        return;
    if (adamsession_import_media(g_session, src, dest, sizeof(dest)) != 0)
        MessageBoxA(hwnd, adamsession_last_error(g_session),
                    "FujiNet Go Adam", MB_ICONWARNING);
}

static void load_cart(HWND hwnd)
{
    char src[MAX_PATH], dest[1024];
    if (!open_file(hwnd,
                   "ColecoVision cartridges (*.rom;*.col;*.bin)\0"
                   "*.rom;*.col;*.bin\0",
                   src, sizeof(src)))
        return;
    if (adamsession_import_media(g_session, src, dest, sizeof(dest)) != 0) {
        MessageBoxA(hwnd, adamsession_last_error(g_session),
                    "FujiNet Go Adam", MB_ICONWARNING);
        return;
    }
    adamsession_set_str(g_session, "cart_path", dest);
    adamsession_set_int(g_session, "machine", 1);
    restart_session();
}

static void eject_cart(void)
{
    adamsession_set_str(g_session, "cart_path", "");
    adamsession_set_int(g_session, "machine", 0);
    restart_session();
}

static void show_fujinet_config(void)
{
    if (adamsession_fujinet_external(g_session)) {
        MessageBoxA(g_hwnd, "FujiNet is external: use the web UI of the "
                    "instance that connects to this app.",
                    "FujiNet Go Adam", MB_ICONINFORMATION);
        return;
    }
    ShellExecuteA(NULL, "open", adamsession_fujinet_webui_url(g_session),
                  NULL, NULL, SW_SHOWNORMAL);
}

/* ---- console log window -------------------------------------------------- */

#define LOG_TIMER_ID 1

static char g_log_shown[128 * 1024]; /* what the edit control holds now */

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = adamsession_fujinet_copy_log(g_session, buf, sizeof(buf));
        const char *text = n > 0 ? buf : "(no FujiNet output yet)";
        LRESULT first, lines;
        RECT rc;
        HDC dc;
        int visible = 1, at_end;

        /* Unchanged: leave the control alone (keeps the scroll position and
         * any selection). */
        if (strcmp(text, g_log_shown) == 0)
            return 0;
        snprintf(g_log_shown, sizeof(g_log_shown), "%s", text);

        /* Follow the tail only while the reader is already at the bottom;
         * otherwise put them back on the line they were reading. */
        first = SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        lines = SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
        GetClientRect(g_log_edit, &rc);
        dc = GetDC(g_log_edit);
        if (dc) {
            HGDIOBJ old = SelectObject(
                dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            TEXTMETRICA tm;
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, old);
            ReleaseDC(g_log_edit, dc);
        }
        at_end = first + (visible > 0 ? visible : 1) >= lines;

        SendMessageA(g_log_edit, WM_SETREDRAW, FALSE, 0);
        SetWindowTextA(g_log_edit, text);
        if (at_end) {
            /* caret to the end (a (-1, -1) selection only drops the old one,
             * leaving the caret at the top of the fresh text) */
            int end = GetWindowTextLengthA(g_log_edit);
            SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
            SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
        } else {
            LRESULT now =
                SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessageA(g_log_edit, EM_LINESCROLL, 0, (LPARAM)(first - now));
        }
        SendMessageA(g_log_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_log_edit, NULL, TRUE);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, LOG_TIMER_ID);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    static int registered;
    if (adamsession_fujinet_external(g_session)) {
        MessageBoxA(g_hwnd, "FujiNet is external: its console output is in "
                    "the process that is running it.", "FujiNet Go Adam",
                    MB_ICONINFORMATION);
        return;
    }
    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "AdamLogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA(
        "AdamLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, NULL, NULL, inst, NULL);
    g_log_edit = CreateWindowA(
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_log_window, NULL, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT,
                 (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    g_log_shown[0] = '\0';
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    SendMessageA(g_log_window, WM_TIMER, LOG_TIMER_ID, 0);
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- FujiNet connection window ------------------------------------------- */

#define IDC_CONN_EXTERNAL 301
#define IDC_CONN_ADDR     302
#define IDC_CONN_OK       303
#define IDC_CONN_CANCEL   304
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

static HWND g_conn_window;

static HWND conn_child(HWND parent, const char *cls, const char *text,
                       DWORD style, int x, int y, int cx, int cy, int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, parent,
        (HMENU)(UINT_PTR)id,
        (HINSTANCE)GetWindowLongPtr(parent, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
                 TRUE);
    return h;
}

static void conn_close(HWND hwnd)
{
    EnableWindow(g_hwnd, TRUE);
    DestroyWindow(hwnd);
    SetForegroundWindow(g_hwnd);
}

static void conn_apply(HWND hwnd)
{
    char addr[96], host[64];
    int port, external, changed;
    const char *old;

    external = SendDlgItemMessageA(hwnd, IDC_CONN_EXTERNAL, BM_GETCHECK, 0,
                                   0) == BST_CHECKED;
    GetDlgItemTextA(hwnd, IDC_CONN_ADDR, addr, sizeof(addr));
    /* trim */
    {
        char *s = addr, *e;
        while (*s == ' ' || *s == '\t') s++;
        memmove(addr, s, strlen(s) + 1);
        e = addr + strlen(addr);
        while (e > addr && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    }
    if (external &&
        adamsession_parse_hostport(addr, host, sizeof(host), &port) != 0) {
        MessageBoxA(hwnd, "That is not an address:port.\n\nExample: "
                    "0.0.0.0:65216", "External FujiNet", MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd, IDC_CONN_ADDR));
        return;
    }
    old = adamsession_get_str(g_session, "fujinet_external_addr", "");
    changed = adamsession_get_int(g_session, "fujinet_external", 0) !=
                  external ||
              strcmp(old, addr) != 0;
    adamsession_set_int(g_session, "fujinet_external", external);
    adamsession_set_str(g_session, "fujinet_external_addr", addr);
    conn_close(hwnd);
    if (changed)
        restart_session();
}

static LRESULT CALLBACK conn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CONN_EXTERNAL:
            EnableWindow(GetDlgItem(hwnd, IDC_CONN_ADDR),
                         SendDlgItemMessageA(hwnd, IDC_CONN_EXTERNAL,
                                             BM_GETCHECK, 0, 0) ==
                             BST_CHECKED);
            return 0;
        case IDC_CONN_OK:
            conn_apply(hwnd);
            return 0;
        case IDC_CONN_CANCEL:
            conn_close(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        conn_close(hwnd);
        return 0;
    case WM_DESTROY:
        g_conn_window = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* FujiNet > Connection: use the built-in FujiNet, or wait for another
 * instance to dial in to an address:port. The machine restarts on OK. */
static void show_fujinet_connection(HINSTANCE inst)
{
    static int registered;
    HWND w;
    RECT rc;
    int x, y;

    if (g_conn_window) {
        SetForegroundWindow(g_conn_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = conn_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "AdamConnWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    GetWindowRect(g_hwnd, &rc);
    x = rc.left + 60;
    y = rc.top + 80;
    g_conn_window = CreateWindowExA(
        WS_EX_DLGMODALFRAME, "AdamConnWindow", "FujiNet Connection",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, 460, 250, g_hwnd, NULL, inst,
        NULL);
    w = g_conn_window;
    conn_child(w, "BUTTON", "Use external FujiNet",
               BS_AUTOCHECKBOX | WS_TABSTOP, 16, 14, 420, 22,
               IDC_CONN_EXTERNAL);
    conn_child(w, "STATIC", "Listen address:port", 0, 16, 48, 140, 20, 0);
    {
        HWND edit = conn_child(w, "EDIT",
                               adamsession_get_str(g_session,
                                                   "fujinet_external_addr",
                                                   ""),
                               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 160,
                               44, 276, 24, IDC_CONN_ADDR);
        SendMessageA(edit, EM_SETCUEBANNER, FALSE,
                     (LPARAM)L"0.0.0.0:65216");
    }
    conn_child(w, "STATIC",
               "Skips the built-in FujiNet. Point the other instance's "
               "[BOIP] host and port here. 0.0.0.0 accepts connections from "
               "the network; 127.0.0.1 only from this computer.",
               0, 16, 82, 420, 64, 0);
    conn_child(w, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 270, 166, 80,
               26, IDC_CONN_OK);
    conn_child(w, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 356, 166,
               80, 26, IDC_CONN_CANCEL);
    {
        int ext = adamsession_get_int(g_session, "fujinet_external", 0);
        SendDlgItemMessageA(w, IDC_CONN_EXTERNAL, BM_SETCHECK,
                            ext ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(GetDlgItem(w, IDC_CONN_ADDR), ext);
    }
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(w, SW_SHOW);
}

/* F12 / View > Debugger: show it (which stops the machine), or hide it and
 * let the machine run. */
static void open_debugger(void)
{
    adam_debugger_toggle(g_hwnd, g_session);
}

static void open_keypad(void)
{
    adam_keypad_window_toggle(g_hwnd, g_session);
}

/* ---- fullscreen ---------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetWindowPlacement(hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                           &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

/* ---- menu / window ------------------------------------------------------- */

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU media = CreatePopupMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_RESET_ADAM, "Reset Computer (ADAM)");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CV,
                "Reset Game (ColecoVision)");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");

    AppendMenuA(media, MF_STRING, IDM_IMPORT_MEDIA,
                "Import Disk or Data Pack...");
    AppendMenuA(media, MF_STRING, IDM_LOAD_CART, "Load Cartridge...");
    AppendMenuA(media, MF_STRING, IDM_EJECT_CART, "Eject Cartridge");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)media, "M&edia");

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONN, "Connection...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "About FujiNet Go Adam");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

static void apply_display_settings(void)
{
    g_aspect_mode = adamsession_get_int(g_session, "aspect_mode", 0);
    g_smooth = adamsession_get_int(g_session, "smooth_scaling", 0);
    adam_display_set_options(g_aspect_mode, g_smooth);
}

/* The window's size and whether it was maximized, for the next start. The
 * restored (not maximized, not fullscreen) rectangle is the one kept. */
static void save_window_size(HWND hwnd)
{
    WINDOWPLACEMENT wpl;
    int width, height;

    wpl.length = sizeof(wpl);
    if (g_fullscreen)
        wpl = g_prev_placement;
    else if (!GetWindowPlacement(hwnd, &wpl))
        return;
    width = wpl.rcNormalPosition.right - wpl.rcNormalPosition.left;
    height = wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top;
    if (width >= 200 && height >= 200) {
        adamsession_set_int(g_session, "window_width", width);
        adamsession_set_int(g_session, "window_height", height);
    }
    adamsession_set_int(g_session, "window_maximized",
                        wpl.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    adamsession_settings_flush(g_session);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; /* the display child covers the client */
    case WM_SIZE:
        layout_display();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (on_key(hwnd, msg, wp, lp))
            return 0;
        break;
    case WM_CHAR:
        adam_win_char(g_session, wp);
        return 0;
    case WM_KILLFOCUS:
        adamsession_release_all(g_session);
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_RESET_ADAM: adamsession_reset(g_session, 0); break;
        case IDM_RESET_CV:   adamsession_reset(g_session, 1); break;
        case IDM_IMPORT_MEDIA: import_media(hwnd); break;
        case IDM_LOAD_CART:  load_cart(hwnd); break;
        case IDM_EJECT_CART: eject_cart(); break;
        case IDM_FUJINET_CONN:
            show_fujinet_connection(
                (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd,
                                                         GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_KEYPAD:     open_keypad(); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_ABOUT: {
            /* MB_USERICON with the app's own icon, not a stock Windows one. */
            MSGBOXPARAMSA mb;
            char text[512];
            snprintf(text, sizeof(text),
                     "FujiNet Go Adam " ADAM_VERSION_STRING "\n"
                     "Self-contained Coleco ADAM with built-in FujiNet.\n"
                     "Copyright (C) 2026 Thomas Cherryhomes\n"
                     "GPL-3.0-or-later\n\n"
                     "Display: %s",
                     adam_display_renderer());
            memset(&mb, 0, sizeof(mb));
            mb.cbSize = sizeof(mb);
            mb.hwndOwner = hwnd;
            mb.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            mb.lpszText = text;
            mb.lpszCaption = "About FujiNet Go Adam";
            mb.dwStyle = MB_OK | MB_USERICON;
            mb.lpszIcon = MAKEINTRESOURCEA(IDI_APPICON);
            MessageBoxIndirectA(&mb);
            break;
        }
        case IDM_EXIT: save_window_size(hwnd); DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_CLOSE:
        save_window_size(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    adamsession_start_opts opts;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();
    g_prev_placement.length = sizeof(g_prev_placement);

    g_session = adamsession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.",
                    "FujiNet Go Adam", MB_ICONERROR);
        return 1;
    }
    adamsession_default_opts(g_session, &opts);
    if (adamsession_start(g_session, &opts) != 0)
        MessageBoxA(NULL, adamsession_last_error(g_session),
                    "FujiNet Go Adam", MB_ICONWARNING);
    apply_display_settings();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* Redraw the whole client area on any size change; the display is
     * stretched to fit, so a partial repaint leaves stale pixels. */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "AdamMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    {
        int width = adamsession_get_int(g_session, "window_width", 1088);
        int height = adamsession_get_int(g_session, "window_height", 950);
        if (width < 200 || height < 200) {
            width = 1088;
            height = 950;
        }
        g_hwnd = CreateWindowExA(
            0, "AdamMainWindow", "FujiNet Go Adam", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, g_menu, inst,
            NULL);
    }
    if (!g_hwnd)
        return 1;
    g_display = adam_display_create(g_hwnd, g_session);
    layout_display();
    if (adamsession_get_int(g_session, "window_maximized", 0) &&
        show != SW_SHOWMINIMIZED && show != SW_SHOWMINNOACTIVE)
        show = SW_SHOWMAXIMIZED;
    ShowWindow(g_hwnd, show);

    /* Developer affordances, matching the other frontends. */
    if (getenv("ADAM_OPEN_KEYPAD"))
        open_keypad();
    if (getenv("ADAM_OPEN_DEBUGGER"))
        adam_debugger_show(g_hwnd, g_session);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* The debugger's F5/F7/F8/F9/F12 accelerators get first refusal
         * while its window (or one of its fields) has the focus. */
        if (adam_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    adam_display_destroy();
    adamsession_free(g_session);
    return (int)msg.wParam;
}
