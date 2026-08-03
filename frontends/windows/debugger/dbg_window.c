/*
 * Debugger window (Win32): disassembly with click-to-toggle breakpoints,
 * editable registers, memory view, breakpoints, instruction trace, and the
 * VDP visualizers, over the shared adamdebug engine -- the same layout as
 * the GTK, Qt and AppKit debuggers, built from plain common controls.
 *
 * Stop events arrive on the emulator thread and are marshaled to the UI
 * thread with PostMessage (the Win32 analog of g_idle_add /
 * QMetaObject::invokeMethod / dispatch_async).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamdebug.h"

#define DISASM_LINES 40
#define MEM_ROWS 16
#define TRACE_ROWS 256

#define WM_DBG_STOPPED (WM_APP + 1)  /* wp: reason, lp: pc */
#define WM_DBG_ACCEPT  (WM_APP + 2)  /* wp: control id -- Enter in an edit */

#define TIMER_REFRESH 1

/* Control ids. The register fields are contiguous from IDC_REG0. */
enum {
    IDC_PAUSE = 1000, IDC_STEP_IN, IDC_STEP_OVER, IDC_STEP_OUT,
    IDC_GOTO, IDC_TRACE, IDC_STATUS, IDC_TABS,
    IDC_DISASM, IDC_FLAGS, IDC_BP_ADD, IDC_BP_CLEAR, IDC_BP_LIST,
    IDC_MEM_ADDR, IDC_MEM_VIEW, IDC_TRACE_VIEW,
    IDC_PAT_BANK, IDC_SPRITE_INFO,
    IDC_VIEW_NT, IDC_VIEW_PAT, IDC_VIEW_SPR, IDC_VIEW_PAL,
    IDC_VDP_STATE, IDC_VRAM_ADDR, IDC_VRAM_VIEW, IDC_VRAM_PREV,
    IDC_VRAM_NEXT,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 7,
    IDC_LABEL_FIRST /* statics that only carry text */
};

/* Accelerator-driven commands (also the toolbar buttons' ids above). */
#define PAGE_CPU 0
#define PAGE_VDP 1
#define PAGE_VRAM 2
#define PAGE_TRACE 3

#define VRAM_ROWS 24
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64

typedef struct {
    const uint8_t *bgra;  /* owned by the debugger struct */
    int w, h;
} pixel_view;

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND pause_btn, step_in, step_over, step_out;
    HWND goto_edit, trace_check, status;
    HWND disasm;
    HWND reg_label[8], reg_edit[8], flags;
    HWND bp_label, bp_add, bp_clear, bp_list;
    HWND mem_label, mem_addr, mem_view;
    HWND trace_view;
    HWND pat_bank, sprite_info;
    HWND view_nt, view_pat, view_spr, view_pal;
    HWND reg_title;
    HWND state_label, vdp_state;
    HWND vram_label, vram_addr, vram_prev, vram_next, vram_status, vram_view;

    HFONT mono;
    HACCEL accel;

    adamsession *session;
    adamdebug *dbg;

    uint16_t disasm_base;
    uint16_t mem_base;
    uint16_t vram_base;
    int follow_pc;
    int page;

    /* VDP scratch: the engine renders RGBA, GDI wants BGRA. */
    adamvdp_snapshot snap;
    uint8_t nt_rgba[256 * 192 * 4], pat_rgba[256 * 64 * 4];
    uint8_t spr_rgba[128 * 64 * 4];
    uint8_t pal_rgba[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    uint8_t nt_bgra[256 * 192 * 4], pat_bgra[256 * 64 * 4];
    uint8_t spr_bgra[128 * 64 * 4];
    uint8_t pal_bgra[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    pixel_view nt_view, pat_view, spr_view, pal_view;
} debugger;

static debugger *g_dbg;

static const char *const kRegNames[8] = {"AF", "BC", "DE", "HL",
                                         "IX", "IY", "SP", "PC"};

/* ---- small helpers ------------------------------------------------------- */

static uint16_t reg_get(const adamcore_z80_regs *r, int i)
{
    switch (i) {
    case 0: return (uint16_t)((r->a << 8) | r->f);
    case 1: return (uint16_t)((r->b << 8) | r->c);
    case 2: return (uint16_t)((r->d << 8) | r->e);
    case 3: return (uint16_t)((r->h << 8) | r->l);
    case 4: return r->ix;
    case 5: return r->iy;
    case 6: return r->sp;
    default: return r->pc;
    }
}

static void reg_set(adamcore_z80_regs *r, int i, uint16_t v)
{
    switch (i) {
    case 0: r->a = (uint8_t)(v >> 8); r->f = (uint8_t)v; break;
    case 1: r->b = (uint8_t)(v >> 8); r->c = (uint8_t)v; break;
    case 2: r->d = (uint8_t)(v >> 8); r->e = (uint8_t)v; break;
    case 3: r->h = (uint8_t)(v >> 8); r->l = (uint8_t)v; break;
    case 4: r->ix = v; break;
    case 5: r->iy = v; break;
    case 6: r->sp = v; break;
    default: r->pc = v; break;
    }
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

/* Edits hold plain LF-separated text internally; the EDIT control needs
 * CRLF, so build with \r\n from the start. */
static void set_text(HWND h, const char *text)
{
    SetWindowTextA(h, text);
}

/* Same, for text from the shared formatters, which end lines with a bare
 * LF -- an EDIT control renders those as a box instead of a line break. */
static void set_text_lf(HWND h, const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = (char *)malloc(n * 2 + 1);

    if (!buf) {
        set_text(h, text);
        return;
    }
    for (i = 0; i < n; i++) {
        if (text[i] == '\n')
            buf[o++] = '\r';
        buf[o++] = text[i];
    }
    buf[o] = '\0';
    set_text(h, buf);
    free(buf);
}

static int parse_addr(debugger *d, const char *text, uint16_t *out)
{
    char buf[128];
    char *p = buf;
    char *end;
    unsigned long v;
    uint16_t addr;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '$')
        p++;
    if (*p) {
        v = strtoul(p, &end, 16);
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            end++;
        if (*end == '\0' && v <= 0xFFFF) {
            *out = (uint16_t)v;
            return 1;
        }
    }
    if (adamdebug_symbol_find(d->dbg, text, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

static void edit_text(HWND h, char *out, int outsz)
{
    GetWindowTextA(h, out, outsz);
}

/* ---- refreshers ---------------------------------------------------------- */

static void refresh_disasm(debugger *d)
{
    adamdasm_line lines[DISASM_LINES];
    adamcore_z80_regs r;
    char *text;
    size_t cap = DISASM_LINES * 160;
    size_t len = 0;
    int i, n;

    adamdebug_get_regs(d->dbg, &r);
    if (d->follow_pc)
        d->disasm_base = r.pc;

    text = (char *)malloc(cap);
    if (!text)
        return;
    text[0] = '\0';

    n = adamdebug_disassemble(d->dbg, d->disasm_base, DISASM_LINES, lines);
    for (i = 0; i < n; i++) {
        char bytes[16] = "";
        int b;
        if (lines[i].symbol)
            len += (size_t)snprintf(text + len, cap - len, "%17s%s:\r\n", "",
                                    lines[i].symbol);
        for (b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 3, 4, "%02X ", lines[i].bytes[b]);
        len += (size_t)snprintf(
            text + len, cap - len, "%c%c%04X  %-13s %s\r\n",
            adamdebug_bp_is_set(d->dbg, lines[i].addr) ? '*' : ' ',
            lines[i].addr == r.pc ? '>' : ' ', lines[i].addr, bytes,
            lines[i].text);
        if (len + 200 >= cap)
            break;
    }
    set_text(d->disasm, text);
    free(text);
}

static void refresh_regs(debugger *d)
{
    adamcore_z80_regs r;
    char buf[128];
    int i;

    adamdebug_get_regs(d->dbg, &r);
    for (i = 0; i < 8; i++) {
        if (GetFocus() == d->reg_edit[i])
            continue; /* do not fight the user's typing */
        snprintf(buf, sizeof(buf), "%04X", reg_get(&r, i));
        set_text(d->reg_edit[i], buf);
    }
    snprintf(buf, sizeof(buf),
             "F: %c%c%c%c%c%c  IFF1:%d IM:%d  cyc:%llu",
             (r.f & 0x80) ? 'S' : '-', (r.f & 0x40) ? 'Z' : '-',
             (r.f & 0x10) ? 'H' : '-', (r.f & 0x04) ? 'P' : '-',
             (r.f & 0x02) ? 'N' : '-', (r.f & 0x01) ? 'C' : '-', r.iff1,
             r.im, (unsigned long long)r.cycles);
    set_text(d->flags, buf);
}

static void refresh_mem(debugger *d)
{
    uint8_t data[MEM_ROWS * 16];
    char text[MEM_ROWS * 96];
    size_t len = 0;
    int row, col;

    adamdebug_read_mem(d->dbg, d->mem_base, data, sizeof(data));
    text[0] = '\0';
    for (row = 0; row < MEM_ROWS; row++) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%04X  ",
                                (uint16_t)(d->mem_base + row * 16));
        for (col = 0; col < 16; col++)
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%02X ",
                                    data[row * 16 + col]);
        len += (size_t)snprintf(text + len, sizeof(text) - len, " ");
        for (col = 0; col < 16; col++) {
            uint8_t c = data[row * 16 + col];
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%c",
                                    (c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        len += (size_t)snprintf(text + len, sizeof(text) - len, "\r\n");
    }
    set_text(d->mem_view, text);
}

static void refresh_bps(debugger *d)
{
    uint16_t bps[128];
    char text[128 * 48];
    size_t len = 0;
    int i, n;

    n = adamdebug_bp_list(d->dbg, bps, 128);
    text[0] = '\0';
    if (!n)
        snprintf(text, sizeof(text), "(no breakpoints)\r\n");
    for (i = 0; i < n; i++) {
        uint16_t off = 0;
        const char *sym = adamdebug_symbol_at(d->dbg, bps[i], &off);
        if (sym)
            len += (size_t)snprintf(text + len, sizeof(text) - len,
                                    "%04X  %s+%X\r\n", bps[i], sym, off);
        else
            len += (size_t)snprintf(text + len, sizeof(text) - len,
                                    "%04X\r\n", bps[i]);
    }
    set_text(d->bp_list, text);
}

static void refresh_trace(debugger *d)
{
    static adamtrace_entry entries[TRACE_ROWS];
    char *text;
    size_t cap = TRACE_ROWS * 48 + 64;
    size_t len = 0;
    int i, n;

    n = adamdebug_trace_read(d->dbg, entries, TRACE_ROWS);
    text = (char *)malloc(cap);
    if (!text)
        return;
    len = (size_t)snprintf(text, cap,
                           n ? "  PC    AF   BC   DE   HL   SP\r\n"
                             : "(trace empty; enable tracing and run)\r\n");
    for (i = 0; i < n; i++)
        len += (size_t)snprintf(text + len, cap - len,
                                "%04X  %04X %04X %04X %04X %04X\r\n",
                                entries[i].pc, entries[i].af, entries[i].bc,
                                entries[i].de, entries[i].hl, entries[i].sp);
    set_text(d->trace_view, text);
    free(text);
}

static void refresh_vdp(debugger *d)
{
    adamvdp_sprite info[32];
    char text[32 * 48 + 128];
    size_t len;
    int bank, i;

    adamdebug_vdp_snapshot(d->dbg, &d->snap);

    adamvdp_render_nametable(&d->snap, d->nt_rgba);
    rgba_to_bgra(d->nt_rgba, d->nt_bgra, 256 * 192);

    bank = (int)SendMessageA(d->pat_bank, CB_GETCURSEL, 0, 0);
    if (bank < 0)
        bank = 0;
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

    len = (size_t)snprintf(text, sizeof(text), "##  Y    X  PAT CLR EC\r\n");
    for (i = 0; i < 32; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len,
                                "%02d %3d  %3d  %02X  %2d  %d\r\n", i,
                                info[i].y, info[i].x, info[i].pattern,
                                info[i].color, info[i].early_clock);
    set_text(d->sprite_info, text);

    {
        static char state[4096];
        adamvdp_format_state(&d->snap, state, (int)sizeof(state));
        set_text_lf(d->vdp_state, state);
        adamvdp_format_hex(&d->snap, d->vram_base, VRAM_ROWS, state,
                           (int)sizeof(state));
        set_text_lf(d->vram_view, state);
    }
}

static void refresh_all(debugger *d)
{
    refresh_disasm(d);
    refresh_regs(d);
    refresh_mem(d);
    refresh_bps(d);
    refresh_trace(d);
    refresh_vdp(d);
    set_text(d->pause_btn, adamdebug_is_paused(d->dbg) ? "Continue (F5)"
                                                       : "Pause (F5)");
}

/* ---- actions ------------------------------------------------------------- */

static void toggle_pause(debugger *d)
{
    if (adamdebug_is_paused(d->dbg)) {
        adamdebug_resume(d->dbg);
        set_text(d->status, "Running");
        refresh_all(d);
    } else {
        adamdebug_pause(d->dbg);
    }
}

static void apply_register(debugger *d, int index)
{
    adamcore_z80_regs r;
    char buf[32];
    unsigned long v;
    char *end;

    if (!adamdebug_is_paused(d->dbg))
        return;
    edit_text(d->reg_edit[index], buf, sizeof(buf));
    v = strtoul(buf, &end, 16);
    if (end == buf || v > 0xFFFF)
        return;
    adamdebug_get_regs(d->dbg, &r);
    reg_set(&r, index, (uint16_t)v);
    adamdebug_set_regs(d->dbg, &r);
    refresh_regs(d);
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[128];
    uint16_t addr;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        apply_register(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_GOTO:
        edit_text(d->goto_edit, buf, sizeof(buf));
        if (parse_addr(d, buf, &addr)) {
            d->follow_pc = 0;
            d->disasm_base = addr;
            refresh_disasm(d);
        }
        break;
    case IDC_MEM_ADDR:
        edit_text(d->mem_addr, buf, sizeof(buf));
        if (parse_addr(d, buf, &addr)) {
            d->mem_base = addr;
            refresh_mem(d);
        }
        break;
    case IDC_BP_ADD:
        edit_text(d->bp_add, buf, sizeof(buf));
        if (parse_addr(d, buf, &addr)) {
            adamdebug_bp_set(d->dbg, addr);
            set_text(d->bp_add, "");
            refresh_bps(d);
            refresh_disasm(d);
        }
        break;
    case IDC_VRAM_ADDR: {
        /* One field does both jobs: an address alone scrolls the dump
         * there, an address followed by hex bytes writes them first. */
        uint8_t bytes[POKE_MAX];
        char msg[96];
        int n;

        edit_text(d->vram_addr, buf, sizeof(buf));
        n = adamvdp_parse_poke(buf, &addr, bytes, POKE_MAX);
        if (n < 0) {
            set_text(d->vram_status,
                     "expected: addr [bytes...], e.g. 1800: 41 42");
            break;
        }
        if (n > 0) {
            adamdebug_vdp_write(d->dbg, addr, bytes, n);
            snprintf(msg, sizeof(msg), "wrote %d byte%s at $%04X", n,
                     n == 1 ? "" : "s", addr);
            set_text(d->vram_addr, "");
        } else {
            snprintf(msg, sizeof(msg), "$%04X", addr);
        }
        set_text(d->vram_status, msg);
        d->vram_base = (uint16_t)(addr & 0x3FF0);
        refresh_vdp(d);
        break;
    }
    default:
        break;
    }
}

static void on_stopped(debugger *d, adamdebug_stop_reason reason, uint16_t pc)
{
    static const char *const names[] = {"paused", "breakpoint", "step",
                                        "run-to"};
    char buf[160];
    uint16_t off = 0;
    const char *sym = adamdebug_symbol_at(d->dbg, pc, &off);

    if (sym)
        snprintf(buf, sizeof(buf), "Stopped (%s) at %04X  %s+%X",
                 names[reason], pc, sym, off);
    else
        snprintf(buf, sizeof(buf), "Stopped (%s) at %04X", names[reason], pc);
    set_text(d->status, buf);
    d->follow_pc = 1;
    refresh_all(d);
}

static void stop_trampoline(void *ud, adamdebug_stop_reason reason,
                            uint16_t pc)
{
    debugger *d = (debugger *)ud;
    /* Emulator thread: hand the event to the UI thread. */
    PostMessageA(d->hwnd, WM_DBG_STOPPED, (WPARAM)reason, (LPARAM)pc);
}

/* ---- pixel view (VDP images) --------------------------------------------- */

static LRESULT CALLBACK pixels_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    pixel_view *pv = (pixel_view *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c;
        BITMAPINFO bmi;

        GetClientRect(hwnd, &c);
        if (pv && pv->bgra) {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = pv->w;
            bmi.bmiHeader.biHeight = -pv->h; /* top-down */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(hdc, COLORONCOLOR); /* nearest: keep pixels */
            StretchDIBits(hdc, 0, 0, c.right, c.bottom, 0, 0, pv->w, pv->h,
                          pv->bgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ----------------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

/* Enter in a single-line field means "apply this value" (and must not beep
 * its way through the default handler). */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_DBG_ACCEPT,
                     (WPARAM)GetWindowLongPtrA(hwnd, GWLP_ID), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
        return 0;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* A click in the disassembly toggles the breakpoint on that line. The
 * address sits in columns 2-5, as in the other frontends. */
static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        char buf[256];
        unsigned addr;

        *(WORD *)buf = (WORD)(sizeof(buf) - 1);
        int n = (int)SendMessageA(hwnd, EM_GETLINE, (WPARAM)line,
                                  (LPARAM)buf);
        if (n >= 6) {
            buf[n] = '\0';
            if (sscanf(buf + 2, "%4X", &addr) == 1 && addr <= 0xFFFF) {
                adamdebug_bp_toggle(g_dbg->dbg, (uint16_t)addr);
                refresh_disasm(g_dbg);
                refresh_bps(g_dbg);
            }
        }
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- construction --------------------------------------------------------- */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style,
                  int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
        (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)d->mono, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                     ES_READONLY | ES_AUTOVSCROLL,
                 id);
}

static HWND field(debugger *d, int id)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, id);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                             (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++);
}

static HWND pixels(debugger *d, int id, pixel_view *pv, const uint8_t *bgra,
                   int w, int h)
{
    HWND view = child(d, "AdamDbgPixels", "", 0, id);
    pv->bgra = bgra;
    pv->w = w;
    pv->h = h;
    SetWindowLongPtrA(view, GWLP_USERDATA, (LONG_PTR)pv);
    return view;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE, IDC_TABS);
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (char *)"CPU";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 0, (LPARAM)&item);
    item.pszText = (char *)"VDP";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 1, (LPARAM)&item);
    item.pszText = (char *)"VDP RAM";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 2, (LPARAM)&item);
    item.pszText = (char *)"Trace";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 3, (LPARAM)&item);

    /* Toolbar */
    d->pause_btn = child(d, "BUTTON", "Pause (F5)",
                         WS_VISIBLE | BS_PUSHBUTTON, IDC_PAUSE);
    d->step_in = child(d, "BUTTON", "Step Into (F7)",
                       WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP_IN);
    d->step_over = child(d, "BUTTON", "Step Over (F8)",
                         WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP_OVER);
    d->step_out = child(d, "BUTTON", "Step Out (Shift+F8)",
                        WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP_OUT);
    d->goto_edit = field(d, IDC_GOTO);
    ShowWindow(d->goto_edit, SW_SHOW);
    d->trace_check = child(d, "BUTTON", "Trace",
                           WS_VISIBLE | BS_AUTOCHECKBOX, IDC_TRACE);
    d->status = child(d, "STATIC", "Running", WS_VISIBLE | SS_LEFT,
                      IDC_STATUS);

    /* CPU page */
    d->disasm = mono_view(d, IDC_DISASM);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC,
                                               (LONG_PTR)disasm_subclass);
    d->reg_title = label(d, "Registers (Enter applies while paused)");
    for (i = 0; i < 8; i++) {
        d->reg_label[i] = label(d, kRegNames[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i);
    }
    d->flags = label(d, "");
    d->bp_label = label(d, "Breakpoints (click a disassembly line to toggle)");
    d->bp_add = field(d, IDC_BP_ADD);
    d->bp_clear = child(d, "BUTTON", "Clear all", BS_PUSHBUTTON, IDC_BP_CLEAR);
    d->bp_list = mono_view(d, IDC_BP_LIST);
    d->mem_label = label(d, "Memory");
    d->mem_addr = field(d, IDC_MEM_ADDR);
    d->mem_view = mono_view(d, IDC_MEM_VIEW);

    /* VDP page */
    d->view_nt = pixels(d, IDC_VIEW_NT, &d->nt_view, d->nt_bgra, 256, 192);
    d->view_pat = pixels(d, IDC_VIEW_PAT, &d->pat_view, d->pat_bgra, 256, 64);
    d->view_spr = pixels(d, IDC_VIEW_SPR, &d->spr_view, d->spr_bgra, 128, 64);
    d->view_pal = pixels(d, IDC_VIEW_PAL, &d->pal_view, d->pal_bgra,
                         ADAMVDP_PAL_W, ADAMVDP_PAL_H);
    d->pat_bank = child(d, "COMBOBOX", "",
                        CBS_DROPDOWNLIST | WS_VSCROLL, IDC_PAT_BANK);
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM) "Bank 0");
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM) "Bank 1");
    SendMessageA(d->pat_bank, CB_ADDSTRING, 0, (LPARAM) "Bank 2");
    SendMessageA(d->pat_bank, CB_SETCURSEL, 0, 0);
    d->sprite_info = mono_view(d, IDC_SPRITE_INFO);

    /* VDP RAM page: the decoded register/table state and a VRAM hex
     * editor. Its own page rather than more columns on the VDP page,
     * which is already three visualizers wide. */
    d->state_label = label(d, "Registers & tables");
    d->vdp_state = mono_view(d, IDC_VDP_STATE);
    d->vram_label = label(d, "VRAM (Enter writes; addresses wrap at 16K)");
    d->vram_addr = field(d, IDC_VRAM_ADDR);
    d->vram_prev = child(d, "BUTTON", "<", BS_PUSHBUTTON, IDC_VRAM_PREV);
    d->vram_next = child(d, "BUTTON", ">", BS_PUSHBUTTON, IDC_VRAM_NEXT);
    d->vram_status = label(d, "");
    d->vram_view = mono_view(d, IDC_VRAM_VIEW);

    /* Trace page */
    d->trace_view = mono_view(d, IDC_TRACE_VIEW);
}

/* Show only the controls belonging to the selected tab. */
static void show_page(debugger *d, int page)
{
    HWND cpu[] = {d->disasm, d->reg_title, d->flags, d->bp_label, d->bp_add,
                  d->bp_clear, d->bp_list, d->mem_label, d->mem_addr,
                  d->mem_view};
    HWND vdp[] = {d->view_nt, d->view_pat, d->view_spr, d->view_pal,
                  d->pat_bank, d->sprite_info};
    HWND vram[] = {d->state_label, d->vdp_state, d->vram_label,
                   d->vram_addr, d->vram_prev, d->vram_next,
                   d->vram_status, d->vram_view};
    size_t i;

    d->page = page;
    for (i = 0; i < sizeof(cpu) / sizeof(cpu[0]); i++)
        ShowWindow(cpu[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    for (i = 0; i < 8; i++) {
        ShowWindow(d->reg_label[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->reg_edit[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    }
    for (i = 0; i < sizeof(vdp) / sizeof(vdp[0]); i++)
        ShowWindow(vdp[i], page == PAGE_VDP ? SW_SHOW : SW_HIDE);
    for (i = 0; i < sizeof(vram) / sizeof(vram[0]); i++)
        ShowWindow(vram[i], page == PAGE_VRAM ? SW_SHOW : SW_HIDE);
    ShowWindow(d->trace_view, page == PAGE_TRACE ? SW_SHOW : SW_HIDE);
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8;
    int pw, ph, px, py;
    int i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->pause_btn, bx, y, 120, 26, TRUE);   bx += 126;
    MoveWindow(d->step_in, bx, y, 120, 26, TRUE);     bx += 126;
    MoveWindow(d->step_over, bx, y, 120, 26, TRUE);   bx += 126;
    MoveWindow(d->step_out, bx, y, 150, 26, TRUE);    bx += 156;
    MoveWindow(d->goto_edit, bx, y + 2, 170, 22, TRUE); bx += 178;
    MoveWindow(d->trace_check, bx, y + 3, 70, 20, TRUE); bx += 78;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){8, 40, client.right - 8, client.bottom - 8};
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4;
    py = page.top + 4;
    pw = page.right - page.left - 8;
    ph = page.bottom - page.top - 8;

    /* CPU: disassembly on the left, everything else stacked on the right. */
    {
        int left_w = pw * 55 / 100;
        int rx = px + left_w + 10;
        int rw = pw - left_w - 10;
        int ry = py;
        int col;

        MoveWindow(d->disasm, px, py, left_w, ph, TRUE);

        MoveWindow(d->reg_title, rx, ry, rw, 18, TRUE);
        ry += 20;
        for (i = 0; i < 8; i++) {
            int row = i / 4;
            col = i % 4;
            MoveWindow(d->reg_label[i], rx + col * 110, ry + row * 26, 26, 18,
                       TRUE);
            MoveWindow(d->reg_edit[i], rx + col * 110 + 28, ry + row * 26 - 2,
                       70, 22, TRUE);
        }
        ry += 56;
        MoveWindow(d->flags, rx, ry, rw, 18, TRUE);
        ry += 26;
        MoveWindow(d->bp_label, rx, ry, rw, 18, TRUE);
        ry += 20;
        MoveWindow(d->bp_add, rx, ry, rw - 110, 22, TRUE);
        MoveWindow(d->bp_clear, rx + rw - 104, ry, 104, 24, TRUE);
        ry += 28;
        MoveWindow(d->bp_list, rx, ry, rw, 110, TRUE);
        ry += 118;
        MoveWindow(d->mem_label, rx, ry, rw, 18, TRUE);
        ry += 20;
        MoveWindow(d->mem_addr, rx, ry, rw, 22, TRUE);
        ry += 28;
        MoveWindow(d->mem_view, rx, ry, rw, ph - (ry - py) - 4, TRUE);
    }

    /* VDP: nametable + palette left, patterns + sprites middle, sprite
     * table right. The views are drawn at 2x (1x for the palette strip). */
    {
        int vx = px, vy = py;

        MoveWindow(d->view_nt, vx, vy, 512, 384, TRUE);
        /* The palette grid is drawn 1:1 so its index labels stay crisp. */
        MoveWindow(d->view_pal, vx, vy + 392, ADAMVDP_PAL_W, ADAMVDP_PAL_H,
                   TRUE);
        vx += 524;
        MoveWindow(d->pat_bank, vx, vy, 120, 200, TRUE);
        MoveWindow(d->view_pat, vx, vy + 30, 512, 128, TRUE);
        MoveWindow(d->view_spr, vx, vy + 170, 256, 128, TRUE);
        vx += 524;
        if (vx < px + pw - 100)
            MoveWindow(d->sprite_info, vx, vy, px + pw - vx, ph, TRUE);
        else /* narrow window: park the sprite table under the images */
            MoveWindow(d->sprite_info, px, vy + 430, pw, ph - 434, TRUE);
    }

    /* VDP RAM: decoded state on the left, hex editor on the right (stacked
     * when there is not room for two readable columns). */
    {
        int half = pw / 2 - 8;
        int wide = half >= 460;
        int lx = px, ly = py;
        int rx = wide ? px + half + 16 : px;
        int ry = wide ? py : py + ph / 2 + 8;
        int lw = wide ? half : pw;
        int rw = wide ? pw - half - 16 : pw;
        int lh = wide ? ph : ph / 2 - 8;
        int rh = wide ? ph : ph - (ry - py);

        MoveWindow(d->state_label, lx, ly, lw, 18, TRUE);
        MoveWindow(d->vdp_state, lx, ly + 20, lw, lh - 20, TRUE);

        MoveWindow(d->vram_label, rx, ry, rw, 18, TRUE);
        MoveWindow(d->vram_addr, rx, ry + 20, rw - 260, 22, TRUE);
        MoveWindow(d->vram_prev, rx + rw - 256, ry + 19, 32, 24, TRUE);
        MoveWindow(d->vram_next, rx + rw - 220, ry + 19, 32, 24, TRUE);
        MoveWindow(d->vram_status, rx + rw - 182, ry + 23, 182, 18, TRUE);
        MoveWindow(d->vram_view, rx, ry + 48, rw, rh - 48, TRUE);
    }

    MoveWindow(d->trace_view, px, py, pw, ph, TRUE);
}

/* ---- window proc ---------------------------------------------------------- */

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;

    if (!d || d->hwnd != hwnd)
        return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && !adamdebug_is_paused(d->dbg)) {
            refresh_regs(d);
            if (d->page == PAGE_VDP || d->page == PAGE_VRAM)
                refresh_vdp(d);
        }
        return 0;
    case WM_DBG_STOPPED:
        on_stopped(d, (adamdebug_stop_reason)wp, (uint16_t)lp);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PAUSE:     toggle_pause(d); return 0;
        case IDC_STEP_IN:   adamdebug_step_into(d->dbg); return 0;
        case IDC_STEP_OVER: adamdebug_step_over(d->dbg); return 0;
        case IDC_STEP_OUT:  adamdebug_step_out(d->dbg); return 0;
        case IDC_BP_CLEAR:
            adamdebug_bp_clear_all(d->dbg);
            refresh_bps(d);
            refresh_disasm(d);
            return 0;
        case IDC_TRACE:
            adamdebug_trace_enable(
                d->dbg,
                SendMessageA(d->trace_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_PAT_BANK:
            if (HIWORD(wp) == CBN_SELCHANGE)
                refresh_vdp(d);
            return 0;
        case IDC_VRAM_PREV:
            d->vram_base = (uint16_t)((d->vram_base - VRAM_PAGE) & 0x3FFF);
            refresh_vdp(d);
            return 0;
        case IDC_VRAM_NEXT:
            d->vram_base = (uint16_t)((d->vram_base + VRAM_PAGE) & 0x3FFF);
            refresh_vdp(d);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); /* keep state; F12 brings it back */
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        adamdebug_set_stop_callback(d->dbg, NULL, NULL);
        DeleteObject(d->mono);
        free(d);
        g_dbg = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points --------------------------------------------------------- */

void adam_debugger_show(HWND parent, adamsession *session)
{
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[4];
    const char *tab;

    if (g_dbg) {
        ShowWindow(g_dbg->hwnd, SW_SHOW);
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AdamDebuggerWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = pixels_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "AdamDbgPixels";
    RegisterClassA(&wc);

    d = (debugger *)calloc(1, sizeof(*d));
    if (!d)
        return;
    d->session = session;
    d->dbg = adamsession_debugger(session);
    d->follow_pc = 1;
    d->mem_base = 0xFC30;
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "AdamDebuggerWindow", "ADAM Debugger",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 1180, 820, NULL, NULL, inst,
                              NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }

    d->mono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH | FF_MODERN, "Consolas");

    build_controls(d);

    /* F5/F7/F8/Shift+F8 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;         accels[0].key = VK_F5;
    accels[0].cmd = IDC_PAUSE;
    accels[1].fVirt = FVIRTKEY;         accels[1].key = VK_F7;
    accels[1].cmd = IDC_STEP_IN;
    accels[2].fVirt = FVIRTKEY;         accels[2].key = VK_F8;
    accels[2].cmd = IDC_STEP_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;
    accels[3].cmd = IDC_STEP_OUT;
    d->accel = CreateAcceleratorTableA(accels, 4);

    adamdebug_set_stop_callback(d->dbg, stop_trampoline, d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);

    /* Dev affordance: ADAM_DEBUGGER_TAB=vdp|vram|trace picks the start
     * tab. */
    tab = getenv("ADAM_DEBUGGER_TAB");
    d->page = PAGE_CPU;
    if (tab && !strcmp(tab, "vdp"))
        d->page = PAGE_VDP;
    else if (tab && !strcmp(tab, "vram"))
        d->page = PAGE_VRAM;
    else if (tab && !strcmp(tab, "trace"))
        d->page = PAGE_TRACE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);

    layout(d);
    refresh_all(d);
    ShowWindow(d->hwnd, SW_SHOW);
}

int adam_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel)
        return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd))
        return 0;
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
