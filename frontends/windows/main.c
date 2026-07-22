/*
 * FujiNet Go Adam -- native Win32 frontend.
 *
 * Mirrors the GTK/Qt/AppKit frontends over the shared adamsession API: a
 * GDI-blitted display letterboxed to the chosen aspect, a present thread
 * that feeds the session's vsync phase-lock from DwmFlush (the Windows
 * analog of the GTK frame clock / CVDisplayLink), full keyboard mapping
 * with the Ctrl+digit controller keypad, a menu bar, media import, and the
 * FujiNet configuration (default browser) and console-log windows.
 * Gamepads are handled inside the core's SDL thread. No on-screen input
 * panels appear unless the user asks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "adamsession.h"
#include "resource.h"

#define FB_W ADAMSESSION_FB_WIDTH
#define FB_H ADAMSESSION_FB_HEIGHT

static adamsession *g_session;
static HWND g_hwnd;
static HWND g_log_window;
static HWND g_log_edit;

/* Frame hand-off: the present thread converts the latest RGB565 frame to
 * top-down BGRA under this lock; WM_PAINT blits it. */
static CRITICAL_SECTION g_fb_lock;
static uint8_t g_bgra[FB_W * FB_H * 4];
static uint16_t g_fb[FB_W * FB_H];
static uint64_t g_serial;

static pthread_t g_present_thread;
static volatile int g_present_run;
static int g_aspect_mode;
static int g_smooth;
static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- present thread: vsync pacing + frame conversion --------------------- */

static void *present_main(void *arg)
{
    (void)arg;
    while (g_present_run) {
        BOOL composited = FALSE;
        DwmIsCompositionEnabled(&composited);
        if (composited && SUCCEEDED(DwmFlush())) {
            /* DwmFlush returned at the compositor's vsync. */
        } else {
            Sleep(16); /* no DWM composition (RDP, safe mode): wall clock */
        }
        adamsession_notify_vsync(g_session, monotonic_ns());

        if (adamsession_copy_frame(g_session, g_fb, &g_serial)) {
            int i;
            EnterCriticalSection(&g_fb_lock);
            for (i = 0; i < FB_W * FB_H; i++) {
                uint16_t p = g_fb[i];
                uint8_t r = (uint8_t)((p >> 11) & 0x1F);
                uint8_t g = (uint8_t)((p >> 5) & 0x3F);
                uint8_t b = (uint8_t)(p & 0x1F);
                g_bgra[i * 4 + 0] = (uint8_t)(b << 3 | b >> 2);
                g_bgra[i * 4 + 1] = (uint8_t)(g << 2 | g >> 4);
                g_bgra[i * 4 + 2] = (uint8_t)(r << 3 | r >> 2);
                g_bgra[i * 4 + 3] = 0xFF;
            }
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    return NULL;
}

/* ---- display ------------------------------------------------------------- */

static RECT dest_rect(int cw, int ch)
{
    double dw, dh;
    RECT r;
    if (g_aspect_mode == 2) { /* integer scale */
        int scale = (int)((cw / FB_W < ch / FB_H) ? cw / FB_W : ch / FB_H);
        if (scale < 1) scale = 1;
        dw = scale * FB_W;
        dh = scale * FB_H;
    } else {
        double aspect =
            g_aspect_mode == 1 ? 4.0 / 3.0 : (double)FB_W / FB_H;
        if ((double)cw / ch > aspect) {
            dh = ch;
            dw = ch * aspect;
        } else {
            dw = cw;
            dh = cw / aspect;
        }
    }
    r.left = (LONG)((cw - dw) / 2);
    r.top = (LONG)((ch - dh) / 2);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    return r;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    BITMAPINFO bmi;
    RECT d;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = FB_W;
    bmi.bmiHeader.biHeight = -FB_H; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    d = dest_rect(client.right, client.bottom);
    SetStretchBltMode(hdc, g_smooth ? HALFTONE : COLORONCOLOR);
    EnterCriticalSection(&g_fb_lock);
    StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top, 0,
                  0, FB_W, FB_H, g_bgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);

    EndPaint(hwnd, &ps);
}

/* ---- keyboard ------------------------------------------------------------ */

/* The EOS special keys (arrows, F1-F6, Home, Insert, Delete) come via
 * WM_KEYDOWN; printable characters and control codes (incl. Ctrl+letter)
 * arrive already translated as WM_CHAR. The two sets do not overlap. */
static int special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     return 0xFF52;
    case VK_DOWN:   return 0xFF54;
    case VK_LEFT:   return 0xFF51;
    case VK_RIGHT:  return 0xFF53;
    case VK_HOME:   return 0xFF50;
    case VK_INSERT: return 0xFF63;
    case VK_DELETE: return 0xFFFF;
    default:
        if (vk >= VK_F1 && vk <= VK_F6)
            return 0xFFBE + (int)(vk - VK_F1);
        return 0;
    }
}

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);

static int on_keydown(HWND hwnd, WPARAM vk)
{
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int keysym;

    switch (vk) {
    case VK_F10:
        return 0; /* let the system open the menu */
    case VK_F11:
        toggle_fullscreen(hwnd);
        return 1;
    case VK_F12:
        open_debugger();
        return 1;
    default:
        break;
    }

    if (ctrl && vk >= '0' && vk <= '9') {
        adamsession_joystick_raw(
            g_session, 0,
            adam_controller_encode(0, 0, 0, 0, 0, 0, (int)(vk - '0')));
        return 1;
    }

    keysym = special_keysym(vk);
    if (keysym) {
        int code = adam_key_from_event((uint32_t)keysym, 0, 0);
        if (code >= 0)
            adamsession_key(g_session, (uint8_t)code);
        return 1;
    }
    return 0;
}

static void on_keyup(WPARAM vk)
{
    if (vk >= '0' && vk <= '9')
        adamsession_joystick_raw(g_session, 0,
                                 adam_controller_encode(0, 0, 0, 0, 0, 0, -1));
}

static void on_char(WPARAM ch)
{
    if (ch >= 1 && ch <= 0x7E)
        adamsession_key(g_session, (uint8_t)ch);
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
    ShellExecuteA(NULL, "open", adamsession_fujinet_webui_url(g_session),
                  NULL, NULL, SW_SHOWNORMAL);
}

/* ---- console log window -------------------------------------------------- */

#define LOG_TIMER_ID 1

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = adamsession_fujinet_copy_log(g_session, buf, sizeof(buf));
        SetWindowTextA(g_log_edit,
                       n > 0 ? buf : "(no FujiNet output yet)");
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
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
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    ShowWindow(g_log_window, SW_SHOW);
}

static void open_debugger(void)
{
    /* The native Win32 debugger window is a follow-up; the shared engine
     * beneath it is already portable. */
    MessageBoxA(g_hwnd,
                "The native Windows debugger is still in progress.\n"
                "Use the GNOME, KDE, or macOS build for debugging today.",
                "Debugger", MB_ICONINFORMATION);
}

/* ---- fullscreen ---------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
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

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
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
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* paint() clears; avoid flicker */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (on_keydown(hwnd, wp))
            return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_keyup(wp);
        break;
    case WM_CHAR:
        on_char(wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_RESET_ADAM: adamsession_reset(g_session, 0); break;
        case IDM_RESET_CV:   adamsession_reset(g_session, 1); break;
        case IDM_IMPORT_MEDIA: import_media(hwnd); break;
        case IDM_LOAD_CART:  load_cart(hwnd); break;
        case IDM_EJECT_CART: eject_cart(); break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd,
                                                         GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                        "FujiNet Go Adam 0.1.0\n"
                        "Self-contained Coleco ADAM with built-in FujiNet.\n"
                        "Copyright (C) 2026 Thomas Cherryhomes\n"
                        "GPL-3.0-or-later",
                        "About FujiNet Go Adam", MB_ICONINFORMATION);
            break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
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
    InitializeCriticalSection(&g_fb_lock);

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
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "AdamMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        0, "AdamMainWindow", "FujiNet Go Adam", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1088, 950, NULL, build_menu(), inst,
        NULL);
    if (!g_hwnd)
        return 1;
    ShowWindow(g_hwnd, show);

    g_present_run = 1;
    pthread_create(&g_present_thread, NULL, present_main, NULL);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_present_run = 0;
    pthread_join(g_present_thread, NULL);
    adamsession_free(g_session);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
