/*
 * Debugger window (Win32) over the Z80 / VDP debugger engine, via
 * core/include/adamdebug.h. Mirrors the SMS, MSX, GNOME and Qt debuggers tab
 * for tab -- Console, CPU & Memory, Disassembly, VDP, Breakpoints, Memory Map
 * & I/O, FujiNet -- built from plain common controls, with the disassembly
 * and the VDP pictures drawn by hand.
 *
 * The engine only engages while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it (the close
 * box, F12, or the main window's F12 / View menu item, which toggles it)
 * detaches, which lets the machine run on at full speed. The toolbar carries
 * a single Stop/Run button (the accent while stopped), the stepping controls
 * (Step, Step Over, Step Out, Run to Cursor, Frame), symbol loading and the
 * save menu; F5 F7 F8 Shift+F8 Shift+F7 F9 F12 work wherever the focus is
 * inside the window (adam_debugger_pretranslate).
 *
 * A 100 ms timer polls the engine's generation counter (never its stop
 * callback, which fires off the UI thread) and refreshes the tab showing,
 * and only that one, when something changed; while the machine runs it
 * refreshes the live values of that tab twice a second. Nothing is greyed
 * while the machine runs: a click in the disassembly selects a row, a
 * register edit says "stop the machine first" in the status line. After a
 * step the window waits a few milliseconds for the stop to settle so the
 * status does not flash "Running".
 *
 * The VDP tab (the nametable, pattern, sprite and palette pictures, the
 * decoded register/status text and the VRAM hex editor) and the FujiNet tab
 * (every AdamNet packet on the BoIP link, both ways) carry over from the
 * earlier window unchanged. Address fields take a label, else bare hex, else
 * the engine's grammar ($ or 0x, registers, arithmetic); the register and
 * byte boxes show hex without a prefix and read it back the same way. Only
 * the console reads bare digits as decimal.
 *
 * ADAM_DEBUGGER_TAB (a tab number, or the start of a tab's name or label, in
 * any case) picks the tab the window opens on: 0 Console, 1 CPU & Memory,
 * 2 Disassembly, 3 VDP, 4 Breakpoints, 5 Memory Map & I/O, 6 FujiNet. The
 * old "vram" means the VDP tab; the Trace tab is gone (console: trace on).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accent_button.h"
#include "adamdebug.h"
#include "resource.h"

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MEM_ROWS 256         /* 4K of memory, 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_BPS 128
#define HISTORY_MAX 64
#define TRACE_MAX 512        /* FujiNet packets shown */
#define VRAM_ROWS 24
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000       /* how long a notice holds the status line */

/* A disassembly row's columns, in characters: address, bytes, label, the
 * instruction, and the comment. */
#define COL_BYTES 6
#define COL_LABEL 19
#define LABEL_W 22
#define COL_DISASM (COL_LABEL + LABEL_W + 1)
#define COMMENT_COL (COL_DISASM + 30)

/* Sizes at 96 dpi; S() scales them to the window's. */
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define MONO_PX 13           /* the monospace font */
#define START_W 1400
#define START_H 800

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the console */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */

#define TIMER_REFRESH 1

#define DBG_CLASS "AdamDebuggerWindow"
#define DISASM_CLASS "AdamDbgDisasm"
#define PIXELS_CLASS "AdamDbgPixels"

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_VDP, TAB_BREAKS, TAB_IO, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names ADAM_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "vdp", "VDP" }, { "breakpoints", "Breakpoints" }, { "memmap", "Memory Map & I/O" },
    { "fujinet", "FujiNet" },
};

#define NREGS 15            /* the editable registers */
#define NCHECKS 8           /* six flags and the two interrupt flip-flops */
#define NSAVE 4

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_RUN_TO, IDC_FRAME, IDC_SYMBOLS, IDC_SAVE,
    IDC_STATUS, IDC_TABS, IDC_TOGGLE_BP,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_CHK0, IDC_CHK_LAST = IDC_CHK0 + NCHECKS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_BACK, IDC_MEM_ON, IDC_MEM_WRITE,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_PAT_BANK, IDC_SPRITE_INFO, IDC_VIEW_NT, IDC_VIEW_PAT, IDC_VIEW_SPR, IDC_VIEW_PAL,
    IDC_VDP_STATE, IDC_VRAM_ADDR, IDC_VRAM_VIEW, IDC_VRAM_PREV, IDC_VRAM_NEXT,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND,
    IDC_BP_ADD,
    IDC_IO_TEXT,
    IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_LABEL_FIRST
};

typedef struct {
    const uint8_t *bgra; /* owned by the debugger struct */
    int w, h;
} pixel_view;

typedef struct {
    HWND hwnd, tabs;
    HWND run_btn, step_btn, over_btn, out_btn, run_to_btn, frame_btn, sym_btn, save_btn, status;

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* cpu + memory */
    HWND reg_label[NREGS], reg[NREGS], chk[NCHECKS], cpu_hint, beam;
    HWND mem_title, mem_view, goto_label, mem_goto, mem_back, mem_on, write_label, mem_write;
    int mem_top;                /* the dump's first address */
    int mem_mark;               /* the address Go to found; -1 none */

    /* disassembly */
    HWND follow_pc, jump_label, jump, disasm_hint, disasm;
    int disasm_top;             /* address of the first disassembly line */
    int selected;               /* the selected line's address, or -1 */
    adamdebug_line lines[DISASM_MAX];
    int line_count, pc_line;

    /* vdp */
    HWND view_nt, view_pat, view_spr, view_pal, pat_bank, sprite_info;
    HWND state_label, vdp_state, vram_label, vram_addr, vram_prev, vram_next, vram_status, vram_view;
    uint32_t vram_base;
    adamvdp_snapshot snap;      /* the engine renders RGBA, GDI wants BGRA */
    uint8_t nt_rgba[256 * 192 * 4], pat_rgba[256 * 64 * 4], spr_rgba[128 * 64 * 4];
    uint8_t pal_rgba[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    uint8_t nt_bgra[256 * 192 * 4], pat_bgra[256 * 64 * 4], spr_bgra[128 * 64 * 4];
    uint8_t pal_bgra[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    pixel_view nt_view, pat_view, spr_view, pal_view;

    /* breakpoints */
    HWND bp_hint, bp_remove, bp_clear, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_cond, bp_add, bp_msg;
    adamdebug_breakpoint bps[MAX_BPS];
    int nbps;
    char bp_sig[MAX_BPS * 200]; /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* memory map & I/O */
    HWND io_text;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_clear, fuji_text, fuji_state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    HWND views[12];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[48];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH bp_on, bp_off;

    adamsession *session;
    adamdebug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* The editable registers, in the order the CPU page lays them out. */
static const char *const k_reg_names[NREGS] = { "PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY",
                                                "AF'", "BC'", "DE'", "HL'", "I", "R", "IM" };
static const int k_reg_ids[NREGS] = { ADAM_REG_PC, ADAM_REG_SP, ADAM_REG_AF, ADAM_REG_BC, ADAM_REG_DE,
                                      ADAM_REG_HL, ADAM_REG_IX, ADAM_REG_IY, ADAM_REG_AF2, ADAM_REG_BC2,
                                      ADAM_REG_DE2, ADAM_REG_HL2, ADAM_REG_I, ADAM_REG_R, ADAM_REG_IM };

/* The check boxes: the flags in F, then the interrupt flip-flops. */
static const char *const k_chk_names[NCHECKS] = { "S", "Z", "H", "PV", "N", "C", "IFF1", "IFF2" };
static const int k_chk_ids[NCHECKS] = { ADAM_FLAG_S, ADAM_FLAG_Z, ADAM_FLAG_H, ADAM_FLAG_PV, ADAM_FLAG_N,
                                        ADAM_FLAG_C, ADAM_REG_IFF1, ADAM_REG_IFF2 };

static const char *const k_save_kinds[NSAVE] = { "dis", "mem", "vram", "regs" };
static const char *const k_save_titles[NSAVE] = { "Disassembly ($0000-$FFFF)...", "Memory (the 64K the CPU sees)...",
                                                  "VRAM (the VDP's 16K)...", "Registers (CPU and VDP, text)..." };

static const char *const k_bp_types[5] = { "Execute", "Read", "Write", "Port in", "Port out" };
static const int k_bp_type_ids[5] = { ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE,
                                      ADAMDEBUG_BP_IN, ADAMDEBUG_BP_OUT };

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

static void wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0) out[0] = 0;
    out[outn - 1] = 0;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    wide(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* An address field: a label first, then bare digits as hex (as the dumps
 * show them), else the engine's grammar ($ or 0x, registers, arithmetic),
 * wrapped to 16 bits. -1 with the reason in err when it is none of them.
 * (The console keeps the engine's own rule: bare digits decimal.) */
static int resolve_addr(debugger *d, const char *text, char *err, int errsz)
{
    char buf[128], *s, *end;
    long v = 0;
    int a;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) {
        snprintf(err, (size_t)errsz, "Nothing typed");
        return -1;
    }
    err[0] = '\0';
    a = adamdebug_label_address(d->dbg, s);
    if (a >= 0) return a & 0xFFFF;
    v = strtol(s, &end, 16);
    if (end != s && !*end && *s != '-' && *s != '+') return (int)(v & 0xFFFF);
    if (adamdebug_eval(d->dbg, s, &v, err, errsz) != 0) {
        if (!err[0]) snprintf(err, (size_t)errsz, "Not an address or label");
        return -1;
    }
    return (int)(v & 0xFFFF);
}

/* A register or byte box: hex without a prefix, as the boxes show it, or
 * else the engine's grammar. */
static int parse_hexish(debugger *d, const char *text, long *out)
{
    char buf[64], err[64], *s, *end;
    long v;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) return 0;
    v = strtol(s, &end, 16);
    if (end != s && !*end) {
        *out = v;
        return 1;
    }
    err[0] = '\0';
    return adamdebug_eval(d->dbg, s, out, err, sizeof err) == 0;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

static void rgba_to_bgra(const uint8_t *src, uint8_t *dst, int pixels)
{
    int i;
    for (i = 0; i < pixels; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = 0xFF;
    }
}

/* ---- scale and fonts --------------------------------------------------------- */

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return v > 0 ? v : 96;
}

/* The window's DPI (Windows 10 1607+), else the system's. */
static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        const UINT v = fn(hwnd);
        if (v) return (int)v;
    }
    return system_dpi();
}

static int S(const debugger *d, int v) { return MulDiv(v, d->dpi, 96); }

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static HFONT make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0) lf.lfHeight = -MulDiv(px, dpi, 96);
    else lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *d)
{
    HFONT old[3] = { d->ui, d->ui_bold, d->mono };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int i;

    d->ui = make_font(d->dpi, 0, FW_NORMAL, NULL);
    d->ui_bold = make_font(d->dpi, 0, FW_BOLD, NULL);
    d->mono = make_font(d->dpi, MONO_PX, FW_NORMAL, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
    }
    for (i = 0; i < 3; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthW(d->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageW(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(d->con_out);
    SendMessageW(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], text[220];
    int addr;
    const int stopped = adamdebug_is_stopped(d->dbg) != 0;

    if (notice_showing(d)) {
        snprintf(text, sizeof text, "%s", d->notice);
    } else if (stopped) {
        reason[0] = '\0';
        adamdebug_stop_reason(d->dbg, reason, sizeof reason, &addr);
        snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(d->status, text);
    set_label(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
}

static void refresh_cpu(debugger *d)
{
    adamdebug_cpu c;
    char buf[96];
    int i;
    adamdebug_cpu_get(d->dbg, &c);
    {
        const int v[NREGS] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                               c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im };
        for (i = 0; i < NREGS; i++) {
            if (GetFocus() == d->reg[i]) continue;   /* do not fight the user's typing */
            /* the register pairs as four digits, I and R as two, IM as it is */
            snprintf(buf, sizeof buf, i < 12 ? "%04X" : i < 14 ? "%02X" : "%d", v[i] & 0xFFFF);
            set_label(d->reg[i], buf);
        }
    }
    {
        const int on[NCHECKS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf, c.iff1, c.iff2 };
        d->updating = 1;
        for (i = 0; i < NCHECKS; i++)
            SendMessageA(d->chk[i], BM_SETCHECK, on[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        d->updating = 0;
    }
    snprintf(buf, sizeof buf, "Line %d   Frame %u   Cycles %llu%s", c.line, (unsigned)c.frame,
             (unsigned long long)c.cycles, c.halted ? "   HALT" : "");
    set_label(d->beam, buf);
}

static void refresh_mem(debugger *d)
{
    static uint8_t mem[MEM_BYTES];
    static char t[(MEM_ROWS + 1) * 90];
    size_t len = 0;
    int row, col;

    memset(mem, 0, sizeof mem);
    adamdebug_read(d->dbg, (uint16_t)d->mem_top, mem, MEM_BYTES);
    len = addf(t, len, sizeof t, "      ");
    for (col = 0; col < 16; col++)
        len = addf(t, len, sizeof t, col == 8 ? " %X  " : "%X  ", col);
    len = addf(t, len, sizeof t, "\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        len = addf(t, len, sizeof t, "%04X  ", d->mem_top + row * 16);
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, col == 8 ? " %02X " : "%02X ", r[col]);
        len = addf(t, len, sizeof t, " ");
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, "%c", r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.');
        len = addf(t, len, sizeof t, "\n");
    }
    set_text_at(d->mem_view, t, 0);

    /* the row Go to found stays marked (selected) across refreshes */
    if (d->mem_mark >= d->mem_top && d->mem_mark < d->mem_top + MEM_BYTES) {
        const int line = 1 + (d->mem_mark - d->mem_top) / 16;
        const int s = (int)SendMessageW(d->mem_view, EM_LINEINDEX, (WPARAM)line, 0);
        const int e = s + (int)SendMessageW(d->mem_view, EM_LINELENGTH, (WPARAM)s, 0);
        const int first = (int)SendMessageW(d->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(d->mem_view, EM_SETSEL, (WPARAM)s, (LPARAM)e);
        edit_scroll_to(d->mem_view, first);
    }
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->line_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->line_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static int row_address(debugger *d, int addr, int rows)
{
    const int a = adamdebug_row_address(d->dbg, (uint16_t)addr, rows);
    return a < 0 ? addr : a & 0xFFFF;
}

static int following(debugger *d)
{
    return SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void refresh_disasm(debugger *d)
{
    int pc_line = -1;
    const int rows = disasm_rows(d);

    if (following(d)) {
        /* the PC a third of the way down, so what led to it shows too */
        adamdebug_cpu c;
        adamdebug_cpu_get(d->dbg, &c);
        d->disasm_top = row_address(d, c.pc & 0xFFFF, -(rows / 3));
    }
    d->line_count = adamdebug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    if (d->line_count < 0) d->line_count = 0;
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static void refresh_vdp(debugger *d)
{
    adamvdp_sprite info[32];
    char text[32 * 48 + 128];
    static char state[4096];
    size_t len;
    int bank, i;

    adamdebug_vdp_snapshot(d->dbg, &d->snap);

    adamvdp_render_nametable(&d->snap, d->nt_rgba);
    rgba_to_bgra(d->nt_rgba, d->nt_bgra, 256 * 192);

    bank = (int)SendMessageA(d->pat_bank, CB_GETCURSEL, 0, 0);
    if (bank < 0) bank = 0;
    adamvdp_render_patterns(&d->snap, bank, d->pat_rgba);
    rgba_to_bgra(d->pat_rgba, d->pat_bgra, 256 * 64);

    adamvdp_render_sprites(&d->snap, d->spr_rgba, info);
    rgba_to_bgra(d->spr_rgba, d->spr_bgra, 128 * 64);

    adamvdp_render_palette(&d->snap, d->pal_rgba);
    rgba_to_bgra(d->pal_rgba, d->pal_bgra, ADAMVDP_PAL_W * ADAMVDP_PAL_H);

    InvalidateRect(d->view_nt, NULL, FALSE);
    InvalidateRect(d->view_pat, NULL, FALSE);
    InvalidateRect(d->view_spr, NULL, FALSE);
    InvalidateRect(d->view_pal, NULL, FALSE);

    len = (size_t)snprintf(text, sizeof text, "##  Y    X  PAT CLR EC\n");
    for (i = 0; i < 32; i++)
        len = addf(text, len, sizeof text, "%02d %3d  %3d  %02X  %2d  %d\n", i, info[i].y, info[i].x,
                   info[i].pattern, info[i].color, info[i].early_clock);
    set_text_at(d->sprite_info, text, 0);

    adamvdp_format_state(&d->snap, state, (int)sizeof state);
    set_text_at(d->vdp_state, state, 0);
    adamvdp_format_hex(&d->snap, (uint16_t)d->vram_base, VRAM_ROWS, state, (int)sizeof state);
    set_text_at(d->vram_view, state, 0);
}

static const char *bp_kind(int type, char *buf)
{
    buf[0] = '\0';
    if (type & ADAMDEBUG_BP_EXEC) strcat(buf, "exec");
    if (type & ADAMDEBUG_BP_READ) strcat(buf, buf[0] ? "/read" : "read");
    if (type & ADAMDEBUG_BP_WRITE) strcat(buf, buf[0] ? "/write" : "write");
    if (type & ADAMDEBUG_BP_IN) strcat(buf, buf[0] ? "/in" : "in");
    if (type & ADAMDEBUG_BP_OUT) strcat(buf, buf[0] ? "/out" : "out");
    return buf;
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    wchar_t w[256];
    LVITEMW it;
    wide(text, w, 256);
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = d->page == TAB_BREAKS;
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *d)
{
    static char sig[sizeof d->bp_sig];
    size_t len = 0;
    int i, sel_id = -1;
    const int sel = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    if (sel >= 0 && sel < d->nbps) sel_id = d->bps[sel].id;
    d->nbps = adamdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (i = 0; i < d->nbps; i++)
        len = addf(sig, len, sizeof sig, "%d %d %u %d %u %u %s\n", d->bps[i].id, d->bps[i].enabled,
                   d->bps[i].hits, d->bps[i].type, d->bps[i].start, d->bps[i].end, d->bps[i].condition);
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) { show_bp_list(d); return; }
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const adamdebug_breakpoint *b = &d->bps[i];
        const int port = (b->type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
        LVITEMA it;
        char id[16], kind[40], range[32], hits[24];
        snprintf(id, sizeof id, "#%d", b->id);
        if (b->start == b->end) snprintf(range, sizeof range, port ? "$%02X" : "$%04X", b->start);
        else snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X", b->start, b->end);
        snprintf(hits, sizeof hits, "%u", b->hits);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(d->bp_list, i, 1, bp_kind(b->type, kind));
        lv_set(d->bp_list, i, 2, range);
        lv_set(d->bp_list, i, 3, b->condition);
        lv_set(d->bp_list, i, 4, hits);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        it.state = INDEXTOSTATEIMAGEMASK(b->enabled ? 2 : 1);
        if (b->id == sel_id) it.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    }
    d->updating = 0;
    show_bp_list(d);
}

static void refresh_io(debugger *d)
{
    static char t[8192];
    t[0] = '\0';
    adamdebug_io_state(d->dbg, t, (int)sizeof t);
    set_text_at(d->io_text, t, 0);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *d, int force)
{
    static adam_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];   /* an entry is at most ~300 */
    static char state[4096];
    char note[200];
    size_t len = 0;
    int n = adamdebug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (!adamsession_fujinet_running(d->session) && !adamsession_fujinet_external(d->session))
        snprintf(note, sizeof note, "FujiNet is not running (FujiNet > Console Log).");
    else
        snprintf(note, sizeof note, "%s FujiNet on BoIP %s, %s.",
                 adamsession_fujinet_external(d->session) ? "External" : "Built-in",
                 adamsession_boip_endpoint(d->session),
                 adamsession_fujinet_link(d->session) ? "connected" : "not connected");
    set_label(d->fuji_note, note);
    state[0] = '\0';
    adamdebug_fujinet_state(d->dbg, state, (int)sizeof state);
    set_text_at(d->fuji_state, state, 0);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) len = addf(t, len, sizeof t, " \xc3\x97%u", (unsigned)p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no AdamNet packets since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

/* The tab showing; the others catch up when they are selected. */
static void refresh_page(debugger *d, int page)
{
    switch (page) {
    case TAB_CPU: refresh_cpu(d); refresh_mem(d); break;
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_VDP: refresh_vdp(d); break;
    case TAB_BREAKS: refresh_bps(d); break;
    case TAB_IO: refresh_io(d); break;
    case TAB_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

/* At a stop the console says why, as the other frontends' do. */
static void console_stop(debugger *d)
{
    char reason[160], line[200];
    int addr;
    reason[0] = '\0';
    adamdebug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (!reason[0]) return;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", reason);
    console_output(d, line);
}

/* After anything that may have moved the engine: the status and the tab
 * showing follow it. */
static void resync(debugger *d)
{
    const int stopped = adamdebug_is_stopped(d->dbg) != 0;
    if (stopped && !d->was_stopped) console_stop(d);
    d->was_stopped = stopped;
    d->seen_gen = adamdebug_generation(d->dbg);
    d->running_ticks = 0;
    refresh_status(d);
    refresh_page(d, d->page);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d, int expect_stop)
{
    int i;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (i = 0; expect_stop && i < 40 && !adamdebug_is_stopped(d->dbg); i++)
        Sleep(1);
    resync(d);
}

static void tick(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    gen = adamdebug_generation(d->dbg);
    stopped = adamdebug_is_stopped(d->dbg) != 0;
    if (gen != d->seen_gen || stopped != d->was_stopped) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
    } else if (!stopped && ++d->running_ticks >= 5) {
        /* running: the live values of the tab showing, twice a second */
        d->running_ticks = 0;
        refresh_status(d);
        refresh_page(d, d->page);
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void on_run(debugger *d)
{
    const int stopped = adamdebug_is_stopped(d->dbg);
    if (stopped) adamdebug_resume(d->dbg);
    else adamdebug_stop(d->dbg);
    settle_and_sync(d, !stopped);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    adamdebug_run_to(d->dbg, (uint16_t)d->selected);
    settle_and_sync(d, 1);
}

static void toggle_breakpoint(debugger *d, int addr)
{
    adamdebug_breakpoint_toggle(d->dbg, (uint16_t)addr);
    resync(d);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(d, d->selected);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = adamdebug_is_stopped(d->dbg);

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "> %s\n", cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);

    out[0] = '\0';
    adamdebug_command(d->dbg, cmd, out, sizeof out);
    SetWindowTextA(d->con_in, "");
    console_output(d, out);
    settle_and_sync(d, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        SetWindowTextA(d->con_in, "");
        return;
    }
    SetWindowTextA(d->con_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *d)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *sp, *cur, *nl;
    int n, first = 1;
    edit_text(d->con_in, text, sizeof text);
    n = adamdebug_completions(d->dbg, text, comps, sizeof comps);
    if (n <= 0) return;
    for (cur = comps; *cur; cur = nl ? nl + 1 : cur + strlen(cur)) {
        int k = 0;
        nl = strchr(cur, '\n');
        snprintf(line, sizeof line, "%.*s", nl ? (int)(nl - cur) : (int)strlen(cur), cur);
        if (!line[0]) continue;
        if (first) {
            snprintf(common, sizeof common, "%s", line);
            first = 0;
        } else {
            while (common[k] && line[k] && common[k] == line[k]) k++;
            common[k] = '\0';
        }
    }
    if (n > 1) console_append(d, comps);
    sp = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(sp ? sp + 1 : text))
        snprintf(common, sizeof common, "%s", sp ? sp + 1 : text);   /* never shorter than typed */
    if (sp) sp[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, n == 1 ? " " : "");
        SetWindowTextA(d->con_in, joined);
        SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}

/* A register or flip-flop box. Refused, with a notice, unless stopped. */
static void on_reg(debugger *d, int i)
{
    char buf[32];
    long v;
    edit_text(d->reg[i], buf, sizeof buf);
    if (!parse_hexish(d, buf, &v))
        toast(d, "Not a number (hex, or an expression)");
    else if (!adamdebug_is_stopped(d->dbg))
        toast(d, "Stop the machine first to change a register");
    else
        adamdebug_cpu_set(d->dbg, k_reg_ids[i], (int)v);
    SetFocus(d->hwnd);
    resync(d);
}

static void on_check(debugger *d, int i)
{
    if (d->updating) return;
    if (!adamdebug_is_stopped(d->dbg)) {
        toast(d, "Stop the machine first to change a flag");
        refresh_cpu(d);     /* the box goes back to the CPU's */
        return;
    }
    adamdebug_cpu_set(d->dbg, k_chk_ids[i], SendMessageA(d->chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
    resync(d);
}

/* Go to: the dump from just above an address (or label), its row marked and
 * scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64], err[96];
    int addr, top;
    edit_text(d->mem_goto, buf, sizeof buf);
    addr = resolve_addr(d, buf, err, sizeof err);
    if (addr < 0) {
        d->mem_mark = -1;
        refresh_mem(d);
        toast(d, err);
        return;
    }
    d->mem_mark = addr;
    d->mem_top = (addr > 0x100 ? addr - 0x100 : 0) & ~0xF;
    if (d->mem_top > 0x10000 - MEM_BYTES) d->mem_top = 0x10000 - MEM_BYTES;
    refresh_mem(d);
    /* the row 30% of the way down */
    top = 1 + (addr - d->mem_top) / 16 - edit_rows(d, d->mem_view) * 3 / 10;
    edit_scroll_to(d->mem_view, top > 0 ? top : 0);
}

/* The dump a page of MEM_BYTES on or back. */
static void on_mem_page(debugger *d, int dir)
{
    d->mem_top += dir * MEM_BYTES;
    if (d->mem_top < 0) d->mem_top = 0;
    if (d->mem_top > 0x10000 - MEM_BYTES) d->mem_top = 0x10000 - MEM_BYTES;
    refresh_mem(d);
}

/* "address value [value...]": bytes into memory from the address on. */
static void on_mem_write(debugger *d)
{
    char buf[256], err[96], *tok, *save = NULL, msg[96];
    int addr, n = 0;
    edit_text(d->mem_write, buf, sizeof buf);
    tok = strtok_s(buf, " \t,", &save);
    if (!tok) return;
    addr = resolve_addr(d, tok, err, sizeof err);
    if (addr < 0) {
        toast(d, err);
        return;
    }
    while ((tok = strtok_s(NULL, " \t,", &save)) != NULL) {
        long v;
        if (!parse_hexish(d, tok, &v) || v < 0 || v > 0xFF) {
            toast(d, "A value is a byte (00-FF)");
            return;
        }
        adamdebug_write(d->dbg, (uint16_t)(addr + n), (uint8_t)v);
        n++;
    }
    if (!n) {
        toast(d, "Type: address value [value...]");
        return;
    }
    snprintf(msg, sizeof msg, "Wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", addr);
    toast(d, msg);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64], err[96];
    int addr;
    edit_text(d->jump, buf, sizeof buf);
    addr = resolve_addr(d, buf, err, sizeof err);
    if (addr < 0) {
        toast(d, err);
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    d->selected = addr;
    refresh_disasm(d);
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < d->nbps ? i : -1;
}

static void on_bp_remove(debugger *d)
{
    const int i = bp_selected(d);
    if (i < 0) {
        set_label(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(d->bp_msg, "");
    adamdebug_breakpoint_remove(d->dbg, d->bps[i].id);
    resync(d);
}

static void on_bp_add(debugger *d)
{
    char a[64], b[64], cond[256], err[160];
    int start, end, sel, id, port;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 4) sel = 0;
    port = (k_bp_type_ids[sel] & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
    start = resolve_addr(d, a, err, sizeof err);
    if (start < 0) {
        set_label(d->bp_msg, err);
        return;
    }
    end = strip(b)[0] ? resolve_addr(d, b, err, sizeof err) : start;
    if (end < 0) {
        set_label(d->bp_msg, err);
        return;
    }
    if (end < start) end = start;
    if (port && end > 0xFF) {
        set_label(d->bp_msg, "An I/O port is $00-$FF");
        return;
    }
    err[0] = '\0';
    id = adamdebug_breakpoint_add(d->dbg, k_bp_type_ids[sel], (uint16_t)start, (uint16_t)end,
                                  strip(cond), err, sizeof err);
    if (id < 0) {
        set_label(d->bp_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(d->bp_msg, "");
    SetWindowTextA(d->bp_start, "");
    SetWindowTextA(d->bp_end, "");
    SetWindowTextA(d->bp_cond, "");
    resync(d);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *d)
{
    const int sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    const int port = sel >= 3;
    SendMessageW(d->bp_start, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"port, e.g. FF" : L"8033 / label"));
    SendMessageW(d->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)(port ? L"(last port)" : L"(end)"));
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void on_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file(d, 0, "Load symbols",
                   "Symbol files (*.sym;*.map;*.noi;*.txt)\0*.sym;*.map;*.noi;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    msg[0] = '\0';
    if (adamdebug_load_symbols(d->dbg, path, msg, sizeof msg) < 0 && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    say(d, msg);
    resync(d);
}

static void on_save(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    size_t n;
    snprintf(title, sizeof title, "Save %s", k_save_titles[which]);
    n = strlen(title);
    while (n > 0 && title[n - 1] == '.') title[--n] = '\0';
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    msg[0] = '\0';
    adamdebug_save(d->dbg, k_save_kinds[which], path, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "Saved %s", base_name(path));
    say(d, msg);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    int i;
    for (i = 0; i < NSAVE; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), k_save_titles[i]);
    GetWindowRect(d->save_btn, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_MEM_WRITE: on_mem_write(d); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(d); break;
    case IDC_VRAM_ADDR: {
        /* One field does both jobs: an address alone scrolls the dump
         * there, an address followed by hex bytes writes them first. */
        uint8_t bytes[POKE_MAX];
        uint16_t vaddr;
        char msg[96];
        int n;

        edit_text(d->vram_addr, buf, sizeof buf);
        n = adamvdp_parse_poke(buf, &vaddr, bytes, POKE_MAX);
        if (n < 0) {
            set_label(d->vram_status, "expected: addr [bytes...], e.g. 1800: 41 42");
            break;
        }
        if (n > 0) {
            adamdebug_vdp_write(d->dbg, vaddr, bytes, n);
            snprintf(msg, sizeof msg, "wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", (unsigned)vaddr);
            SetWindowTextA(d->vram_addr, "");
        } else {
            snprintf(msg, sizeof msg, "$%04X", (unsigned)vaddr);
        }
        set_label(d->vram_status, msg);
        d->vram_base = vaddr & 0x3FF0u;
        refresh_vdp(d);
        break;
    }
    default: break;
    }
}

/* ---- the VDP pictures ----------------------------------------------------------- */

static void paint_pixels(HWND hwnd, HDC hdc)
{
    pixel_view *pv = (pixel_view *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    RECT c;
    GetClientRect(hwnd, &c);
    if (pv && pv->bgra) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = pv->w;
        bmi.bmiHeader.biHeight = -pv->h; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(hdc, COLORONCOLOR); /* nearest: keep pixels */
        StretchDIBits(hdc, 0, 0, c.right, c.bottom, 0, 0, pv->w, pv->h, pv->bgra, &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
}

static LRESULT CALLBACK pixels_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_pixels(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        paint_pixels(hwnd, (HDC)wp);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the disassembly -------------------------------------------------------------- */

/* One run of a row's text at a column. */
static void put(debugger *d, HDC dc, int col, int ty, COLORREF fg, const char *text, int len)
{
    SetTextColor(dc, fg);
    TextOutA(dc, S(d, GUTTER_W) + d->mono_w * col, ty, text, len);
}

/* Drawn by hand, one row per line: the breakpoint gutter (a red dot when the
 * breakpoint is enabled, grey when disabled), address, bytes, label,
 * instruction, and the comment (a jump target's label, a port's name); the
 * label and the comment in grey. The selected line takes the system
 * selection colours, the PC's line (applied last, so it wins) is filled with
 * the accent and drawn in black, the accent being light. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        put(d, dc, 0, 0, GetSysColor(COLOR_GRAYTEXT), "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const adamdebug_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int is_pc = l->is_pc, sel = l->address == d->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT), dim = GetSysColor(COLOR_GRAYTEXT);
        RECT row;
        char text[240];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (sel) {
            FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            fg = dim = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (is_pc) {
            FillRect(dc, &row, adam_accent_brush());
            fg = dim = ADAM_ACCENT_TEXT;
        }
        if (l->has_breakpoint) {
            const int size = d->line_h - S(d, 6) > 4 ? d->line_h - S(d, 6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->breakpoint_enabled ? d->bp_on : d->bp_off);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(is_pc ? BLACK_PEN : NULL_PEN));
            Ellipse(dc, S(d, 4), y + (d->line_h - size) / 2, S(d, 4) + size + 1, y + (d->line_h - size) / 2 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        len = snprintf(text, sizeof text, "%04X", l->address);
        put(d, dc, 0, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.12s", l->bytes);
        put(d, dc, COL_BYTES, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.*s", LABEL_W, l->label);
        if (len > 0) put(d, dc, COL_LABEL, ty, dim, text, len);
        len = snprintf(text, sizeof text, "%s", l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len > 0) put(d, dc, COL_DISASM, ty, fg, text, len);
        if (l->comment[0]) {
            len = snprintf(text, sizeof text, "; %s", l->comment);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            put(d, dc, COMMENT_COL, ty, dim, text, len);
        }
    }
}

/* A click selects a line (it works while the machine runs, too); a click on
 * the gutter, or a double-click, toggles its breakpoint. The wheel and the
 * keys browse by instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(d, hwnd, (HDC)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && d->page == TAB_DISASM) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = row_address(d, d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int half = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -half : wp == VK_NEXT ? half : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = row_address(d, d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrW(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wp == VK_TAB) {
            PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wp == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

/* The standard controls are made as Unicode windows, so the engine's UTF-8
 * (the arrows and dashes of its text) shows as it is. */
static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    wchar_t wcls[64], wtext[512];
    HWND h;
    wide(cls, wcls, 64);
    wide(text, wtext, 512);
    h = CreateWindowExW(ex, wcls, wtext, WS_CHILD | style, 0, 0, 10, 10, d->hwnd, (HMENU)(INT_PTR)id,
                        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND pixels(debugger *d, int id, pixel_view *pv, const uint8_t *bgra, int w, int h)
{
    HWND v = child(d, PIXELS_CLASS, "", 0, id, d->ui);
    pv->bgra = bgra;
    pv->w = w;
    pv->h = h;
    SetWindowLongPtrA(v, GWLP_USERDATA, (LONG_PTR)pv);
    return v;
}

static void size_columns(debugger *d)
{
    static const int widths[5] = { 70, 90, 150, 380, 80 };
    int i;
    for (i = 0; i < 5; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

static void build_controls(debugger *d)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int i;

    d->tabs = child(d, "SysTabControl32", "", WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        /* "&" would underline the next letter */
        char text[32];
        const char *s;
        size_t o = 0;
        for (s = k_tabs[i].label; *s && o < sizeof text - 2; s++) {
            if (*s == '&') text[o++] = '&';
            text[o++] = *s;
        }
        text[o] = '\0';
        item.pszText = text;
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->frame_btn = button(d, "Frame (Shift+F7)", IDC_FRAME);
    d->sym_btn = button(d, "Load Symbols...", IDC_SYMBOLS);
    d->save_btn = button(d, "Save...", IDC_SAVE);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;
    {
        HWND bar[9] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->frame_btn,
                        d->sym_btn, d->save_btn, d->status };
        for (i = 0; i < 9; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Console */
    d->con_out = view(d, IDC_CON_OUT, 0);
    SetWindowTextA(d->con_out, "ADAM debugger console. Type 'help' for every command; Tab completes, Up/Down "
                               "recall. Numbers are decimal; $ (or 0x) for hex.\r\n");
    d->con_prompt = child(d, "STATIC", ">", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, L"help, step, over, break $8033, bpw $6000, mem $6000, disasm, regs, ...");

    /* CPU & Memory */
    for (i = 0; i < NREGS; i++) {
        d->reg_label[i] = dim_label(d, k_reg_names[i], SS_RIGHT);
        d->reg[i] = field(d, IDC_REG0 + i, NULL);
        SendMessageA(d->reg[i], EM_SETLIMITTEXT, 6, 0);
    }
    for (i = 0; i < NCHECKS; i++) d->chk[i] = checkbox(d, k_chk_names[i], IDC_CHK0 + i);
    d->cpu_hint = dim_label(d, "Enter sets a register (hex); they change only while the machine is stopped.",
                            SS_LEFT | SS_ENDELLIPSIS);
    d->beam = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    d->mem_title = label(d, "Memory (the CPU's view through the live bank map)", d->ui_bold, SS_LEFT);
    d->mem_view = view(d, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"6000 / label");
    d->mem_back = button(d, "Back 4K", IDC_MEM_BACK);
    d->mem_on = button(d, "On 4K", IDC_MEM_ON);
    d->write_label = dim_label(d, "Write", SS_LEFT);
    d->mem_write = field(d, IDC_MEM_WRITE, L"6000 3F 00 (address, hex bytes)");

    /* Disassembly. Follow PC is a toggle button: the accent while it is on. */
    d->follow_pc = child(d, "BUTTON", "Follow PC", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_FOLLOW_PC,
                         d->ui);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"8033 or a label");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                  "press F9 for a breakpoint; scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10,
                                d->hwnd, (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* VDP: the pictures on the left, the decoded state and a VRAM hex
     * editor stacked on the right. */
    d->view_nt = pixels(d, IDC_VIEW_NT, &d->nt_view, d->nt_bgra, 256, 192);
    d->view_pat = pixels(d, IDC_VIEW_PAT, &d->pat_view, d->pat_bgra, 256, 64);
    d->view_spr = pixels(d, IDC_VIEW_SPR, &d->spr_view, d->spr_bgra, 128, 64);
    d->view_pal = pixels(d, IDC_VIEW_PAL, &d->pal_view, d->pal_bgra, ADAMVDP_PAL_W, ADAMVDP_PAL_H);
    d->pat_bank = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_PAT_BANK, d->ui);
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM)"Bank 0");
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM)"Bank 1");
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM)"Bank 2");
    SendMessageA(d->pat_bank, CB_SETCURSEL, 0, 0);
    d->sprite_info = view(d, IDC_SPRITE_INFO, 0);
    d->state_label = label(d, "Registers & tables", d->ui_bold, SS_LEFT);
    d->vdp_state = view(d, IDC_VDP_STATE, WS_HSCROLL);
    d->vram_label = label(d, "VRAM (Enter writes; addr [bytes...]; wraps at 16K)", d->ui_bold, SS_LEFT);
    d->vram_addr = field(d, IDC_VRAM_ADDR, L"1800: 41 42");
    d->vram_prev = button(d, "<", IDC_VRAM_PREV);
    d->vram_next = button(d, ">", IDC_VRAM_NEXT);
    d->vram_status = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->vram_view = view(d, IDC_VRAM_VIEW, WS_HSCROLL);

    /* Breakpoints */
    d->bp_hint = dim_label(d, "Click a disassembly line's gutter to set an execute breakpoint; the box enables it.",
                           SS_LEFT | SS_ENDELLIPSIS);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, "SysListView32", "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[5] = { "#", "Type", "Range", "Condition", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 5; i++) {
            col.pszText = (char *)cols[i];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    d->bp_type = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_TYPE, d->ui);
    for (i = 0; i < 5; i++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[i]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_start = field(d, IDC_BP_START, NULL);
    d->bp_end = field(d, IDC_BP_END, NULL);
    d->bp_cond = field(d, IDC_BP_COND, L"condition, e.g. a == $FF  (in a watch: value, addr)");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(d);

    /* Memory Map & I/O */
    d->io_text = view(d, IDC_IO_TEXT, WS_HSCROLL);

    /* FujiNet: the note and the log are UTF-8 (see wide_view_text) */
    d->fuji_title = label(d, "AdamNet packets over BoIP", d->ui_bold, SS_LEFT);
    d->fuji_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = view(d, IDC_FUJI_TEXT, WS_HSCROLL);
    d->fuji_state_title = label(d, "BoIP link", d->ui_bold, SS_LEFT);
    d->fuji_state = view(d, IDC_FUJI_STATE, WS_HSCROLL);
}

static void show_page(debugger *d, int page)
{
    HWND console[] = { d->con_out, d->con_prompt, d->con_in };
    HWND cpu[] = { d->cpu_hint, d->beam, d->mem_title, d->mem_view, d->goto_label, d->mem_goto, d->mem_back,
                   d->mem_on, d->write_label, d->mem_write };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND vdp[] = { d->view_nt, d->view_pat, d->view_spr, d->view_pal, d->pat_bank, d->sprite_info,
                   d->state_label, d->vdp_state, d->vram_label, d->vram_addr, d->vram_prev, d->vram_next,
                   d->vram_status, d->vram_view };
    HWND brk[] = { d->bp_hint, d->bp_remove, d->bp_clear, d->bp_type, d->bp_start, d->bp_end, d->bp_cond,
                   d->bp_add, d->bp_msg };
    HWND fuji[] = { d->fuji_title, d->fuji_note, d->fuji_clear, d->fuji_text, d->fuji_state_title,
                    d->fuji_state };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) \
                         ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(console, TAB_CONSOLE);
    SHOW(cpu, TAB_CPU);
    SHOW(d->reg_label, TAB_CPU);
    SHOW(d->reg, TAB_CPU);
    SHOW(d->chk, TAB_CPU);
    SHOW(dis, TAB_DISASM);
    SHOW(vdp, TAB_VDP);
    SHOW(brk, TAB_BREAKS);
    ShowWindow(d->io_text, page == TAB_IO ? SW_SHOW : SW_HIDE);
    SHOW(fuji, TAB_FUJINET);
#undef SHOW
    show_bp_list(d);
}

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar: stepping, then symbols and save, the status after them.
     * Too narrow for the status beside them, it goes on a line of its own. */
    {
        HWND bar[8] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->frame_btn,
                        d->sym_btn, d->save_btn };
        const char *btext[8] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                 "Run to Cursor", "Frame (Shift+F7)", "Load Symbols...", "Save..." };
        x = S(d, 8);
        for (i = 0; i < 8; i++) {
            w = btn_w(d, btext[i]);
            place(bar[i], x, S(d, 8), w, bh);
            x += w + gap + (i == 5 ? S(d, 14) : 0);
        }
        y = S(d, 8) + bh + S(d, 8);
        if (client.right - S(d, 8) - x >= S(d, 220)) {
            place(d->status, x, S(d, 8) + (bh - lh) / 2, client.right - S(d, 8) - x, lh);
        } else {
            place(d->status, S(d, 8), y - S(d, 2), client.right - S(d, 16), lh);
            y += lh + S(d, 4);
        }
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = y + page.top + S(d, 6);
    pw = client.right - 2 * px;
    ph = client.bottom - S(d, 10) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 240)) ph = S(d, 240);

    /* Console: the transcript, then the prompt and its line */
    w = d->mono_w + S(d, 6);
    place(d->con_out, px, py, pw, ph - eh - S(d, 8));
    place(d->con_prompt, px, py + ph - eh + (eh - d->mono_h) / 2, w, d->mono_h);
    place(d->con_in, px + w + S(d, 2), py + ph - eh, pw - w - S(d, 2), eh);

    /* CPU & Memory: the registers across (PC, SP and the main set, then the
     * shadow set and I, R, IM), the flags and flip-flops, the beam line, the
     * dump, Go to and Write under it */
    {
        const int rw = d->mono_w * 5 + S(d, 10), cell = (pw + S(d, 8)) / 8;
        const int lw = text_w(d, d->ui, "WWW") + S(d, 4);
        for (i = 0; i < NREGS; i++) {
            const int row = i / 8, col = i % 8;
            const int ry = py + row * (eh + S(d, 8));
            place(d->reg_label[i], px + col * cell, ry + (eh - lh) / 2, lw, lh);
            place(d->reg[i], px + col * cell + lw + S(d, 4), ry, rw, eh);
        }
    }
    y = py + 2 * (eh + S(d, 8));
    x = px;
    for (i = 0; i < NCHECKS; i++) {
        w = text_w(d, d->ui, k_chk_names[i]) + S(d, 28);
        place(d->chk[i], x, y, w, S(d, 22));
        x += w + S(d, 6);
    }
    place(d->cpu_hint, x + S(d, 16), y + (S(d, 22) - lh) / 2, pw - (x - px) - S(d, 16), lh);
    y += S(d, 28);
    place(d->beam, px, y, pw, lh);
    y += lh + S(d, 10);
    {
        const int bottom = py + ph - eh;
        place(d->mem_title, px, y, pw, lh);
        y += lh + S(d, 4);
        place(d->mem_view, px, y, pw, bottom - S(d, 6) - y);
        y = bottom + (eh - lh) / 2;
        x = px;
        w = text_w(d, d->ui, "Go to") + S(d, 6);
        place(d->goto_label, x, y, w, lh);
        x += w;
        place(d->mem_goto, x, bottom, S(d, 150), eh);
        x += S(d, 156);
        w = btn_w(d, "Back 4K");
        place(d->mem_back, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 4);
        w = btn_w(d, "On 4K");
        place(d->mem_on, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 24);
        w = text_w(d, d->ui, "Write") + S(d, 6);
        place(d->write_label, x, y, w, lh);
        x += w;
        place(d->mem_write, x, bottom, S(d, 260), eh);
    }

    /* Disassembly: Follow PC and Jump to over the lines */
    w = btn_w(d, "Follow PC");
    place(d->follow_pc, px, py - S(d, 1), w, bh);
    x = px + w + S(d, 12);
    w = text_w(d, d->ui, "Jump to") + S(d, 6);
    place(d->jump_label, x, py + (eh - lh) / 2, w, lh);
    x += w;
    place(d->jump, x, py, S(d, 150), eh);
    x += S(d, 162);
    place(d->disasm_hint, x, py + (eh - lh) / 2, px + pw - x, lh);
    place(d->disasm, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));

    /* VDP: nametable + palette left, patterns + sprites middle, decoded
     * state + VRAM hex editor right (under everything when narrow). */
    {
        int vx = px, vy = py, rx, rw, sh;
        place(d->view_nt, vx, vy, S(d, 512), S(d, 384));
        place(d->view_pal, vx, vy + S(d, 392), S(d, ADAMVDP_PAL_W), S(d, ADAMVDP_PAL_H));
        vx += S(d, 524);
        place(d->pat_bank, vx, vy, S(d, 120), S(d, 200));
        place(d->view_pat, vx, vy + S(d, 30), S(d, 256), S(d, 64));
        place(d->view_spr, vx, vy + S(d, 100), S(d, 128), S(d, 64));
        place(d->sprite_info, vx, vy + S(d, 170), S(d, 270), ph - S(d, 174));
        vx += S(d, 282);
        rx = vx;
        rw = px + pw - rx;
        sh = ph;
        if (rw < S(d, 280)) {
            /* narrow: park the state/VRAM column under the pictures */
            rx = px;
            rw = pw;
            vy += S(d, 480);
            sh = ph - S(d, 480);
        }
        place(d->state_label, rx, vy, rw, lh);
        place(d->vdp_state, rx, vy + lh + S(d, 2), rw, sh / 2 - lh - S(d, 6));
        vy += sh / 2;
        place(d->vram_label, rx, vy, rw, lh);
        place(d->vram_addr, rx, vy + lh + S(d, 2), rw - S(d, 260), eh);
        place(d->vram_prev, rx + rw - S(d, 256), vy + lh + S(d, 1), S(d, 32), eh + S(d, 2));
        place(d->vram_next, rx + rw - S(d, 220), vy + lh + S(d, 1), S(d, 32), eh + S(d, 2));
        place(d->vram_status, rx + rw - S(d, 182), vy + lh + S(d, 5), S(d, 182), lh);
        place(d->vram_view, rx, vy + lh + eh + S(d, 8), rw, sh / 2 - (lh + eh + S(d, 8)));
    }

    /* Breakpoints: the hint, Remove and Clear All over the list; the add
     * row and a message under it */
    {
        const int msg_y = py + ph - lh, add_y = msg_y - S(d, 8) - eh;
        const int list_y = py + bh + S(d, 6);
        int bw = btn_w(d, "Clear All"), rbw = btn_w(d, "Remove");
        place(d->bp_clear, px + pw - bw, py, bw, bh);
        place(d->bp_remove, px + pw - bw - gap - rbw, py, rbw, bh);
        place(d->bp_hint, px, py + (bh - lh) / 2, pw - bw - rbw - 3 * gap, lh);
        place(d->bp_list, px, list_y, pw, add_y - gap - list_y);
        place(d->bp_empty, px, list_y + S(d, 12), pw, lh);
        x = px;
        place(d->bp_type, x, add_y, S(d, 110), S(d, 240));
        x += S(d, 116);
        place(d->bp_start, x, add_y, S(d, 140), eh);
        x += S(d, 146);
        place(d->bp_end, x, add_y, S(d, 110), eh);
        x += S(d, 116);
        bw = btn_w(d, "Add");
        place(d->bp_cond, x, add_y, px + pw - x - bw - gap, eh);
        place(d->bp_add, px + pw - bw, add_y - S(d, 1), bw, bh);
        place(d->bp_msg, px, msg_y, pw, lh);
    }

    /* Memory Map & I/O */
    place(d->io_text, px, py, pw, ph);

    /* FujiNet: the packets left (a note and Clear over them), the link's
     * state and counts right */
    {
        const int rw = pw / 3 > S(d, 340) ? S(d, 340) : pw / 3 > S(d, 220) ? pw / 3 : S(d, 220);
        const int lw = pw - rw - S(d, 12);
        const int cw = btn_w(d, "Clear");
        place(d->fuji_title, px, py, lw, lh);
        place(d->fuji_clear, px + lw - cw, py + lh + S(d, 4), cw, bh);
        place(d->fuji_note, px, py + lh + S(d, 4) + (bh - lh) / 2, lw - cw - gap, lh);
        place(d->fuji_text, px, py + lh + bh + S(d, 10), lw, ph - lh - bh - S(d, 10));
        place(d->fuji_state_title, px + lw + S(d, 12), py, rw, lh);
        place(d->fuji_state, px + lw + S(d, 12), py + lh + S(d, 4), rw, ph - lh - S(d, 4));
    }

    /* everything again: what the controls left behind where they were */
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    adamdebug_detach(d->dbg);    /* lets the machine run */
    ShowWindow(d->hwnd, SW_HIDE);
}

static void attach_and_refresh(debugger *d)
{
    adamdebug_attach(d->dbg);    /* stops the machine */
    d->was_stopped = 0;
    d->seen_gen = adamdebug_generation(d->dbg);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    settle_and_sync(d, 1);
}

/* Stop/Run: the accent with black text while the machine is stopped (the
 * button that will start it), the stock look while it runs. */
static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = adamdebug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, adam_accent_brush());
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, ADAM_ACCENT_TEXT);
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, d->ui);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static void select_tab(debugger *d, int page)
{
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)page, 0);
    show_page(d, page);
    layout(d);
    refresh_page(d, page);
}

static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int page = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        show_page(d, page);
        refresh_page(d, page);
        return 0;
    }
    if (nh->hwndFrom == d->follow_pc && nh->code == (UINT)NM_CUSTOMDRAW)
        return adam_accent_customdraw((const NMCUSTOMDRAW *)nh, following(d));
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                adamdebug_breakpoint_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (nh->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int i = bp_selected(d);
        if (i >= 0 && !(d->bps[i].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT))) {
            char a[16];
            snprintf(a, sizeof a, "$%04X", d->bps[i].start);
            select_tab(d, TAB_DISASM);
            SetWindowTextA(d->jump, a);
            on_jump(d);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(860, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);
        size_columns(d);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_console(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (h == d->status && notice_showing(d)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (d->page == TAB_DISASM && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_CHK0 && id <= IDC_CHK_LAST && HIWORD(wp) == BN_CLICKED) {
            on_check(d, id - IDC_CHK0);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) {
            on_save(d, id - IDC_SAVE_KIND0);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: adamdebug_step(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OVER: adamdebug_step_over(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OUT: adamdebug_step_out(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_FRAME: adamdebug_frame(d->dbg, 1); settle_and_sync(d, 1); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(d); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_MEM_BACK: on_mem_page(d, -1); return 0;
        case IDC_MEM_ON: on_mem_page(d, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_PAT_BANK:
            if (HIWORD(wp) == CBN_SELCHANGE) refresh_vdp(d);
            return 0;
        case IDC_VRAM_PREV:
            d->vram_base = (d->vram_base - VRAM_PAGE) & 0x3FFFu;
            refresh_vdp(d);
            return 0;
        case IDC_VRAM_NEXT:
            d->vram_base = (d->vram_base + VRAM_PAGE) & 0x3FFFu;
            refresh_vdp(d);
            return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) on_bp_type(d);
            return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_CLEAR:
            adamdebug_breakpoint_clear(d->dbg);
            set_label(d->bp_msg, "");
            resync(d);
            return 0;
        case IDC_FUJI_CLEAR:
            adamdebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        if (adamdebug_is_attached(d->dbg)) adamdebug_detach(d->dbg);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->bp_on);
        DeleteObject(d->bp_off);
        DestroyAcceleratorTable(d->accel);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    if (proc == dbg_proc) wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    RegisterClassExA(&wc);
}

/* ADAM_DEBUGGER_TAB: a tab number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory", "map", "i/o"...). */
static int tab_from_env(void)
{
    const char *tab = getenv("ADAM_DEBUGGER_TAB");
    char *end;
    long n;
    size_t len;
    int i, pass;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    if (_stricmp(tab, "vram") == 0) return TAB_VDP;       /* folded into the VDP tab */
    if (_stricmp(tab, "trace") == 0) return TAB_CONSOLE;  /* now `trace` in the console */
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && _strnicmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (_strnicmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

/* Symbols beside the cartridge (<cart>.sym/.map/.noi), once, when the window
 * is first made; the engine does not look for them itself. */
static void load_cart_symbols(debugger *d)
{
    char msg[512];
    msg[0] = '\0';
    if (adamdebug_load_symbols(d->dbg, NULL, msg, sizeof msg) > 0 && msg[0]) console_output(d, msg);
}

void adam_debugger_show(HWND parent, adamsession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[7];
    int tab, w, h;
    MONITORINFO mi;

    if (g_dbg) {
        d = g_dbg;
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        if (!adamdebug_is_attached(d->dbg) || !IsWindowVisible(d->hwnd) || !adamdebug_is_stopped(d->dbg))
            attach_and_refresh(d);
        else
            resync(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        register_class(inst, PIXELS_CLASS, pixels_proc, NULL, 0);
        registered = 1;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = adamsession_debugger(session);
    if (!d->dbg) {
        free(d);
        MessageBoxA(parent, "The debugger could not be created for this session.", "Debugger Unavailable",
                    MB_ICONERROR);
        return;
    }
    d->selected = -1;
    d->mem_mark = -1;
    d->pc_line = -1;
    d->fuji_count = -1;
    g_dbg = d;

    /* the start size, held inside the monitor's work area */
    d->dpi = window_dpi(parent);
    w = S(d, START_W);
    h = S(d, START_H);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (w > mi.rcWork.right - mi.rcWork.left - S(d, 40)) w = mi.rcWork.right - mi.rcWork.left - S(d, 40);
        if (h > mi.rcWork.bottom - mi.rcWork.top - S(d, 40)) h = mi.rcWork.bottom - mi.rcWork.top - S(d, 40);
    }
    /* owned by the main window: over it, and gone with it */
    d->hwnd = CreateWindowExA(0, DBG_CLASS, "ADAM Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h, parent, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }
    if (window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        const int was = d->dpi;
        d->dpi = window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, MulDiv(w, d->dpi, was), MulDiv(h, d->dpi, was),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));
    d->bp_off = CreateSolidBrush(RGB(0x9a, 0x99, 0x96));

    build_controls(d);

    /* F5/F7/F8/Shift+F8/Shift+F7/F9/F12 work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY | FSHIFT; accels[4].key = VK_F7;  accels[4].cmd = IDC_FRAME;
    accels[5].fVirt = FVIRTKEY;          accels[5].key = VK_F9;  accels[5].cmd = IDC_TOGGLE_BP;
    accels[6].fVirt = FVIRTKEY;          accels[6].key = VK_F12; accels[6].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 7);

    tab = tab_from_env();
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);
    load_cart_symbols(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    attach_and_refresh(d);
}

int adam_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void adam_debugger_toggle(HWND parent, adamsession *session)
{
    if (adam_debugger_visible()) hide_window(g_dbg);
    else adam_debugger_show(parent, session);
}

int adam_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    /* F12 closes without waiting for the accelerator (Windows keeps F12 for
     * debuggers); its key-up goes with it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
