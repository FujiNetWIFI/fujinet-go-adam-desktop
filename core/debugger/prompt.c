/*
 * prompt.c -- the debugger's command line (adamdebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the console can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "adammemtools.h"
#include "debugger_internal.h"

#define WAIT_MS 1500

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->len >= o->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->dst + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += n < o->cap - o->len ? n : o->cap - o->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "go", "stop", "runto", "scanline",
    "frame", "break", "bp", "bpr", "bpw", "bpa", "bpin", "bpout", "delete",
    "enable", "disable", "clear", "hits", "breaks", "print", "regs", "set",
    "mem", "dump", "poke", "vram", "vpoke", "vdp", "io", "map", "cart",
    "sound", "video", "fujinet", "label", "labels", "symbols", "disasm", "dis",
    "trace", "watch", "unwatch", "find", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a CALL/RST/DJNZ/block op/HALT (F8)\n"
    "out                    run to the return (Shift+F8)\n"
    "run | go | stop        go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]   n beam lines / frames\n"
    "break <addr>[-<end>] [cond]     execute breakpoint (alias bp)\n"
    "bpr | bpw | bpa <addr>[-<end>] [cond]   read / write / read-or-write\n"
    "bpin | bpout <port>[-<end>] [cond]  I/O breakpoint\n"
    "delete <id|addr> | enable <id> | disable <id> | clear | breaks\n"
    "hits [clear]           the breakpoints with their hit counts / zero them\n"
    "print <expr>           e.g. print hl, print [$6000], print {sp}\n"
    "regs                   CPU registers\n"
    "set <reg> <expr>       e.g. set pc $8024, set zf 1, set af' 0, set ixh 5\n"
    "mem <addr> [len]       dump memory (side-effect free; alias dump)\n"
    "poke <addr> <v...>     write memory\n"
    "vram <addr> [len] | vpoke <addr> <v...> | vdp\n"
    "map                    the memory map, region by region\n"
    "io                     the memory map, ports, controllers, VDP, CPU, FujiNet\n"
    "cart | sound           the cartridge and memory config / the sound chips\n"
    "video [section]        the Video tab as text\n"
    "fujinet                the FujiNet link\n"
    "label <addr> <name> | labels [file]    set a label / load a symbol file\n"
    "symbols [text]         the labels, filtered by name\n"
    "disasm [addr] [n]      (alias dis)\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "watch [expr|lo-hi] | unwatch <n|all>   the watch list\n"
    "find [hex] <bytes> | find text <string>   search memory (stopped)\n"
    "save <dis|mem|vram|regs> <file>\n"
    "Numbers: $C000 0xC000 C000h 49152 %1010 (a bare 1-4 digit address is hex\n"
    "unless it is a label); registers a..l af..iy sp pc i r im iff1 iff2 af'\n"
    "bc' de' hl' ixh ixl iyh iyl; flags cf nf pf hf zf sf yf xf; [addr] {addr};\n"
    "labels; line frame cycles vline vdpaddr vdpsecond; in a condition value,\n"
    "old and addr; C operators. Tab completes.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s) {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(adamdebug *d, outbuf *o, const char *text, long *v)
{
    char err[96];
    if (adamdebug_eval(d, text, v, err, sizeof err) != 0) {
        out(o, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* An address argument: a bare token of 1-4 hex digits is hex unless it names
 * a label (as in the memory fields); anything else is an expression. */
static int eval_addr(adamdebug *dbg, outbuf *buf, const char *text, long *val)
{
    size_t len = strlen(text), idx;
    for (idx = 0; idx < len && isxdigit((unsigned char)text[idx]); idx++) {}
    if (len >= 1 && len <= 4 && idx == len && adamdebug_label_address(dbg, text) < 0) {
        *val = strtol(text, NULL, 16);
        return 0;
    }
    return eval(dbg, buf, text, val);
}

/* "<addr>" or "<addr>-<end>" */
static int range(adamdebug *d, outbuf *o, char *tok, long *a, long *b)
{
    char *dash = strchr(tok + 1, '-');
    if (dash) {
        *dash = '\0';
        if (eval_addr(d, o, tok, a) || eval_addr(d, o, dash + 1, b))
            return -1;
        return 0;
    }
    if (eval_addr(d, o, tok, a))
        return -1;
    *b = *a;
    return 0;
}

static void show_regs(adamdebug *d, outbuf *o)
{
    adamdebug_cpu c;
    adamdebug_cpu_get(d, &c);
    out(o, "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X\n",
        c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy);
    out(o, "AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X IM%d IFF1=%d IFF2=%d%s\n",
        c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2,
        c.halted ? " HALT" : "");
    out(o, "flags %c%c%c%c%c%c%c%c  line %d  frame %u  cycles %llu\n",
        c.sf ? 'S' : '-', c.zf ? 'Z' : '-', c.yf ? 'Y' : '-', c.hf ? 'H' : '-',
        c.xf ? 'X' : '-', c.pf ? 'P' : '-', c.nf ? 'N' : '-', c.cf ? 'C' : '-',
        c.line, c.frame, (unsigned long long)c.cycles);
}

static void dump(outbuf *o, uint16_t addr, const uint8_t *b, int n)
{
    int i, j;
    for (i = 0; i < n; i += 16) {
        out(o, "%04X ", (uint16_t)(addr + i));
        for (j = 0; j < 16; j++) {
            if (j < n - i)
                out(o, " %02X", b[i + j]);
            else
                out(o, "   ");
        }
        out(o, "  ");
        for (j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

static void list_breaks(adamdebug *d, outbuf *o)
{
    adamdebug_breakpoint bp[ADAMDEBUG_MAX_BREAKPOINTS];
    int n = adamdebug_breakpoint_list(d, bp, ADAMDEBUG_MAX_BREAKPOINTS), i;
    if (!n) {
        out(o, "no breakpoints\n");
        return;
    }
    for (i = 0; i < n; i++) {
        char kinds[16] = "";
        if (bp[i].type & ADAMDEBUG_BP_EXEC)  strcat(kinds, "x");
        if (bp[i].type & ADAMDEBUG_BP_READ)  strcat(kinds, "r");
        if (bp[i].type & ADAMDEBUG_BP_WRITE) strcat(kinds, "w");
        if (bp[i].type & ADAMDEBUG_BP_IN)    strcat(kinds, "i");
        if (bp[i].type & ADAMDEBUG_BP_OUT)   strcat(kinds, "o");
        if (bp[i].start == bp[i].end)
            out(o, "#%d %-3s $%04X", bp[i].id, kinds, bp[i].start);
        else
            out(o, "#%d %-3s $%04X-$%04X", bp[i].id, kinds, bp[i].start,
                bp[i].end);
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

static void add_bp(adamdebug *d, outbuf *o, int type, char *args, int io)
{
    char *t = next_tok(&args);
    char err[96];
    long a, b;
    int id;

    if (!t) {
        out(o, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, o, t, &a, &b))
        return;
    if (io && (a > 255 || b > 255)) {
        out(o, "error: ports are 0-255\n");
        return;
    }
    id = adamdebug_breakpoint_add(d, type, (uint16_t)a, (uint16_t)b,
                                  rest_of(args), err, sizeof err);
    if (id < 0)
        out(o, "error: %s\n", err);
    else
        out(o, "breakpoint #%d\n", id);
}

/* The breakpoint ids a delete/enable/disable argument names: "#3" or "3" is
 * an id; otherwise (or when no such id exists) an address, which names every
 * breakpoint starting there. Returns the count (up to max). */
static int bp_ids(adamdebug *d, outbuf *o, char *tok, int *ids, int max)
{
    adamdebug_breakpoint bp[ADAMDEBUG_MAX_BREAKPOINTS];
    int n = adamdebug_breakpoint_list(d, bp, ADAMDEBUG_MAX_BREAKPOINTS), i,
        count = 0;
    long v;

    if (tok[0] == '#')
        v = strtol(tok + 1, NULL, 10);
    else if (eval(d, o, tok, &v))
        return -1;
    for (i = 0; i < n && count < max; i++)
        if (bp[i].id == v) {
            ids[count++] = bp[i].id;
            return count;
        }
    if (tok[0] != '#')
        for (i = 0; i < n && count < max; i++)
            if (bp[i].start == (uint16_t)v)
                ids[count++] = bp[i].id;
    return count;
}

/* Where the machine is, after a step: the stop reason and the line at PC. */
static void show_stop(adamdebug *d, outbuf *o)
{
    char why[128];
    adamdebug_line l;
    int pc = -1, pcl;

    adamdebug_stop_reason(d, why, sizeof why, &pc);
    if (pc < 0) {
        out(o, "running\n");
        return;
    }
    if (adamdebug_disassemble(d, (uint16_t)pc, &l, 1, &pcl) == 1)
        out(o, "%s  $%04X  %s\n", why, pc, l.disasm);
    else
        out(o, "%s  $%04X\n", why, pc);
}

/* Issue an execution command, and say where it ended up. */
static void run_cmd(adamdebug *d, outbuf *o, const char *cmd, long n, long v)
{
    unsigned gen;

    if (!adamdebug_is_stopped(d)) {
        out(o, "the machine is running (stop first)\n");
        return;
    }
    gen = adamdebug_generation(d);
    if (!strcmp(cmd, "step"))
        adamdebug_step_count(d, n);
    else if (!strcmp(cmd, "over"))
        adamdebug_step_over(d);
    else if (!strcmp(cmd, "out"))
        adamdebug_step_out(d);
    else if (!strcmp(cmd, "runto"))
        adamdebug_run_to(d, (uint16_t)v);
    else if (!strcmp(cmd, "scanline"))
        adamdebug_scanline(d, (int)n);
    else
        adamdebug_frame(d, (int)n);
    if (adamdebug_wait_stop(d, gen, WAIT_MS))
        show_stop(d, o);
    else
        out(o, "running\n");
}

static void show_sound(adamdebug *dbg, outbuf *buf)
{
    adamdebug_sound sound;
    int idx;

    if (!adamdebug_sound_get(dbg, &sound)) {
        out(buf, "no machine\n");
        return;
    }
    out(buf, "SN76489: latched register %d\n", sound.sn.latched_reg);
    for (idx = 0; idx < 3; idx++)
        out(buf, "  tone %d: period %4d (%.1f Hz)  attenuation %2d  level %2d\n", idx + 1,
            sound.sn.period[idx], sound.sn.freq[idx], sound.sn.atten[idx], sound.sn.level[idx]);
    out(buf, "  noise: %s, rate %d (%.1f Hz)  attenuation %2d  level %2d\n",
        sound.sn.noise_white ? "white" : "periodic", sound.sn.noise_rate, sound.sn.noise_hz,
        sound.sn.atten[3], sound.sn.level[3]);
    if (sound.ay.present) {
        out(buf, "AY-3-8910 (Super Game Module): register %X\n  ", sound.ay.addr);
        for (idx = 0; idx < 16; idx++)
            out(buf, " R%X=%02X", idx, sound.ay.reg[idx]);
        out(buf, "\n");
        for (idx = 0; idx < 3; idx++)
            out(buf, "  %c: period %4d (%.1f Hz)%s%s level %d%s\n", 'A' + idx,
                sound.ay.period[idx], sound.ay.freq[idx], sound.ay.tone_on[idx] ? " tone" : "",
                sound.ay.noise_on[idx] ? " noise" : "", sound.ay.level[idx],
                sound.ay.envelope[idx] ? " envelope" : "");
        out(buf, "  noise %d  envelope %d shape %X\n", sound.ay.noise_period,
            sound.ay.env_period, sound.ay.env_shape);
    }
}

int adamdebug_command(adamdebug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd, *c;
    outbuf o;
    long v = 0, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    o.dst = dst;
    o.cap = dstsz;
    o.len = 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (c = cmd; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        out(&o, "%s", help_text);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        run_cmd(d, &o, "step", v, 0);
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "o")) {
        run_cmd(d, &o, "over", 1, 0);
    } else if (!strcmp(cmd, "out")) {
        run_cmd(d, &o, "out", 1, 0);
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g")) {
        adamdebug_resume(d);
    } else if (!strcmp(cmd, "stop")) {
        unsigned gen = adamdebug_generation(d);
        if (adamdebug_is_stopped(d)) {
            show_stop(d, &o);
        } else {
            adamdebug_stop(d);
            if (adamdebug_wait_stop(d, gen, WAIT_MS))
                show_stop(d, &o);
        }
    } else if (!strcmp(cmd, "runto")) {
        char *t = next_tok(&p);
        if (!t || eval_addr(d, &o, t, &v)) {
            if (!t)
                out(&o, "usage: runto <addr>\n");
            return o.len;
        }
        run_cmd(d, &o, "runto", 1, v);
    } else if (!strcmp(cmd, "scanline") || !strcmp(cmd, "frame")) {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        run_cmd(d, &o, cmd, v, 0);
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp")) {
        add_bp(d, &o, ADAMDEBUG_BP_EXEC, p, 0);
    } else if (!strcmp(cmd, "bpr")) {
        add_bp(d, &o, ADAMDEBUG_BP_READ, p, 0);
    } else if (!strcmp(cmd, "bpw")) {
        add_bp(d, &o, ADAMDEBUG_BP_WRITE, p, 0);
    } else if (!strcmp(cmd, "bpa")) {
        add_bp(d, &o, ADAMDEBUG_BP_ACCESS, p, 0);
    } else if (!strcmp(cmd, "bpin")) {
        add_bp(d, &o, ADAMDEBUG_BP_IN, p, 1);
    } else if (!strcmp(cmd, "bpout")) {
        add_bp(d, &o, ADAMDEBUG_BP_OUT, p, 1);
    } else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") ||
               !strcmp(cmd, "disable")) {
        char *t = next_tok(&p);
        int ids[128], n, i;
        if (!t) {
            out(&o, "usage: %s <id|addr>\n", cmd);
        } else if ((n = bp_ids(d, &o, t, ids, 128)) < 0) {
            /* the message is out */
        } else if (n == 0) {
            out(&o, "no such breakpoint\n");
        } else {
            for (i = 0; i < n; i++) {
                if (!strcmp(cmd, "delete"))
                    adamdebug_breakpoint_remove(d, ids[i]);
                else
                    adamdebug_breakpoint_enable(d, ids[i],
                                                !strcmp(cmd, "enable"));
            }
        }
    } else if (!strcmp(cmd, "clear")) {
        adamdebug_breakpoint_clear(d);
    } else if (!strcmp(cmd, "hits")) {
        char *t = next_tok(&p);
        if (t && !strcmp(t, "clear")) {
            adamdebug_breakpoint_clear_hits(d);
            out(&o, "hit counts cleared\n");
        } else {
            list_breaks(d, &o);
        }
    } else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl")) {
        list_breaks(d, &o);
    } else if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
        if (eval(d, &o, rest_of(p), &v) == 0) {
            char bin[17];
            int i;
            for (i = 0; i < 16; i++)
                bin[i] = (char)('0' + ((v >> (15 - i)) & 1));
            bin[16] = '\0';
            out(&o, "$%04lX  %ld  %%%s\n", (unsigned long)v & 0xffff, v, bin);
        }
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r")) {
        show_regs(d, &o);
    } else if (!strcmp(cmd, "set")) {
        char *r = next_tok(&p);
        char err[160];
        if (!r || !*rest_of(p)) {
            out(&o, "usage: set <reg> <expr>\n");
        } else if (eval(d, &o, rest_of(p), &v) == 0) {
            if (!adamdebug_is_stopped(d)) {
                out(&o, "the machine is running (stop first)\n");
            } else if (!adamdebug_register_set(d, r, v, err, sizeof err)) {
                out(&o, "error: %s\n", err);
            } else {
                if (err[0])
                    out(&o, "note: %s\n", err);
                show_regs(d, &o);
            }
        }
    } else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m") ||
               !strcmp(cmd, "dump") || !strcmp(cmd, "vram")) {
        char *t = next_tok(&p), *l = next_tok(&p);
        uint8_t buf[256];
        w = 64;
        if (!t) {
            out(&o, "usage: %s <addr> [len]\n", cmd);
            return o.len;
        }
        if (eval_addr(d, &o, t, &v) || (l && eval(d, &o, l, &w)))
            return o.len;
        if (w > 256)
            w = 256;
        if (w < 1)
            w = 1;
        if (!strcmp(cmd, "vram"))
            adamdebug_vdp_read(d, (uint16_t)v, buf, (int)w);
        else
            adamdebug_read(d, (uint16_t)v, buf, (int)w);
        dump(&o, (uint16_t)v, buf, (int)w);
    } else if (!strcmp(cmd, "poke") || !strcmp(cmd, "vpoke")) {
        char *t = next_tok(&p), *b;
        int n = 0;
        if (!t) {
            out(&o, "usage: %s <addr> <val...>\n", cmd);
            return o.len;
        }
        if (eval_addr(d, &o, t, &v))
            return o.len;
        while ((b = next_tok(&p)) != NULL) {
            if (eval(d, &o, b, &w))
                return o.len;
            if (!strcmp(cmd, "vpoke")) {
                uint8_t byte = (uint8_t)w;
                adamdebug_vdp_write(d, (uint16_t)(v + n), &byte, 1);
            } else {
                adamdebug_write(d, (uint16_t)(v + n), (uint8_t)w);
            }
            n++;
        }
        out(&o, "%d byte%s at $%04lX\n", n, n == 1 ? "" : "s",
            (unsigned long)v & 0xffff);
    } else if (!strcmp(cmd, "vdp")) {
        adamvdp_snapshot *s = malloc(sizeof *s);
        char text[2048];
        if (s) {
            adamdebug_vdp_snapshot(d, s);
            adamvdp_format_state(s, text, sizeof text);
            out(&o, "%s", text);
            free(s);
        }
    } else if (!strcmp(cmd, "io")) {
        char text[4096];
        adamdebug_io_state(d, text, sizeof text);
        out(&o, "%s", text);
    } else if (!strcmp(cmd, "map")) {
        adamdebug_region map[16];
        const int count = adamdebug_memory_map(d, map, 16);
        int i;
        if (!count)
            out(&o, "the machine is not running\n");
        for (i = 0; i < count; i++)
            out(&o, "$%04X-$%04X  %s%s%s\n", map[i].low, map[i].high, map[i].name,
                map[i].readable ? "" : " (no read)",
                map[i].writable ? "" : " (read only)");
    } else if (!strcmp(cmd, "cart")) {
        char text[512];
        adamdebug_cart_summary(d, text, sizeof text);
        out(&o, "%s\n", text);
    } else if (!strcmp(cmd, "sound")) {
        show_sound(d, &o);
    } else if (!strcmp(cmd, "video")) {
        const char *(*hook)(const char *section) = adamdebug_video_text_hook();
        char *section = next_tok(&p);
        const char *text = hook ? hook(section ? section : "") : NULL;
        if (text && *text)
            out(&o, "%s", text);
        else
            out(&o, "no video state yet\n");
    } else if (!strcmp(cmd, "fujinet")) {
        char text[2048];
        adamdebug_fujinet_state(d, text, sizeof text);
        out(&o, "%s", text);
    } else if (!strcmp(cmd, "label")) {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || !n || eval_addr(d, &o, t, &v)) {
            if (!t || !n)
                out(&o, "usage: label <addr> <name>\n");
            return o.len;
        }
        adamdebug_set_label(d, (uint16_t)v, n);
        out(&o, "$%04lX = %s\n", (unsigned long)v & 0xffff, n);
    } else if (!strcmp(cmd, "labels")) {
        char msg[300];
        char *f = rest_of(p);
        adamdebug_load_symbols(d, *f ? f : NULL, msg, sizeof msg);
        out(&o, "%s\n", msg);
    } else if (!strcmp(cmd, "symbols")) {
        int picks[40], count, i;
        count = adamdebug_symbols_filter(d, rest_of(p), picks, 40);
        if (!count)
            out(&o, "no labels match\n");
        for (i = 0; i < count; i++) {
            uint16_t addr;
            char name[64];
            if (adamdebug_symbols_get(d, picks[i], &addr, name, sizeof name))
                out(&o, "$%04X  %s\n", addr, name);
        }
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "dis") ||
               !strcmp(cmd, "d")) {
        char *t = next_tok(&p), *n = next_tok(&p);
        adamdebug_line lines[64];
        adamdebug_cpu cp;
        int pcl, cnt, i;
        adamdebug_cpu_get(d, &cp);
        v = cp.pc;
        w = 16;
        if ((t && eval_addr(d, &o, t, &v)) || (n && eval(d, &o, n, &w)))
            return o.len;
        if (w > 64)
            w = 64;
        if (w < 1)
            w = 1;
        cnt = adamdebug_disassemble(d, (uint16_t)v, lines, (int)w, &pcl);
        for (i = 0; i < cnt; i++) {
            if (lines[i].label[0])
                out(&o, "%s:\n", lines[i].label);
            out(&o, "%c%c%04X  %-12s %-22s%s%s\n", lines[i].is_pc ? '>' : ' ',
                lines[i].has_breakpoint ? (lines[i].breakpoint_enabled ? '*' : 'o') : ' ',
                lines[i].address, lines[i].bytes, lines[i].disasm,
                lines[i].comment[0] ? "; " : "", lines[i].comment);
        }
    } else if (!strcmp(cmd, "trace")) {
        char *t = next_tok(&p);
        if (t && !strcmp(t, "on")) {
            adamdebug_trace_enable(d, 1);
        } else if (t && !strcmp(t, "off")) {
            adamdebug_trace_enable(d, 0);
        } else if (t && !strcmp(t, "clear")) {
            adamdebug_trace_clear(d);
        } else {
            adamdebug_trace tr[64];
            int n, i;
            w = 16;
            if (t && eval(d, &o, t, &w))
                return o.len;
            if (w > 64)
                w = 64;
            n = adamdebug_trace_read(d, tr, (int)w);
            if (!n)
                out(&o, "trace is %s and empty%s\n",
                    adamdebug_trace_enabled(d) ? "on" : "off",
                    adamdebug_is_attached(d) ? "" : " (it records while attached)");
            for (i = n - 1; i >= 0; i--) {
                adamdebug_line l;
                int pcl;
                adamdebug_disassemble(d, tr[i].pc, &l, 1, &pcl);
                out(&o, "%10llu %3d  %04X  %-22s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X\n",
                    (unsigned long long)tr[i].cycles, tr[i].line, tr[i].pc,
                    l.disasm, tr[i].af, tr[i].bc, tr[i].de, tr[i].hl,
                    tr[i].sp);
            }
        }
    } else if (!strcmp(cmd, "watch")) {
        char *text = rest_of(p);
        int i;
        if (*text && adamwatch_add(text) < 0)
            out(&o, "no room for another watch\n");
        if (!adamwatch_count())
            out(&o, "no watches (watch <addr|label|lo-hi>)\n");
        for (i = 0; i < adamwatch_count(); i++) {
            char row[160];
            adamwatch_format(d, i, row, sizeof row);
            out(&o, "%2d  %s\n", i, row);
        }
    } else if (!strcmp(cmd, "unwatch")) {
        char *t = next_tok(&p);
        if (!t)
            out(&o, "unwatch <n|all>\n");
        else if (!strcmp(t, "all"))
            adamwatch_clear();
        else if (eval(d, &o, t, &v) == 0)
            adamwatch_remove((int)v);
    } else if (!strcmp(cmd, "find")) {
        char *t = next_tok(&p);
        char joined[200];
        int kind = ADAMSEARCH_HEX, count = 0, flags = 0, found, i;
        uint8_t pat[ADAMSEARCH_MAX], mask;
        uint16_t hits[32];
        if (t && !strcmp(t, "text")) {
            kind = ADAMSEARCH_TEXT;
            t = rest_of(p);
        } else if (t && !strcmp(t, "hex")) {
            t = rest_of(p);
        } else if (t) {
            snprintf(joined, sizeof joined, "%s %s", t, rest_of(p));
            t = joined;
        }
        if (!t || !*t || !adamsearch_parse(kind, t, pat, &count, &mask, &flags)) {
            out(&o, "find [hex] <bytes> | find text <string>\n");
            return o.len;
        }
        found = adamdebug_search(d, pat, count, mask, flags, 0, 0xFFFF, hits, 32);
        if (found < 0)
            out(&o, "stop the machine first\n");
        else if (!found)
            out(&o, "not found\n");
        for (i = 0; i < found; i++)
            out(&o, "$%04X\n", hits[i]);
    } else if (!strcmp(cmd, "save")) {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f) {
            out(&o, "usage: save <dis|mem|vram|regs> <file>\n");
        } else {
            adamdebug_save(d, k, f, msg, sizeof msg);
            out(&o, "%s\n", msg);
        }
    } else {
        out(&o, "unknown command '%s' (try help)\n", cmd);
    }

    adamdebug_touch(d);
    return o.len;
}

int adamdebug_completions(adamdebug *d, const char *prefix, char *dst,
                          int dstsz)
{
    outbuf o;
    const char *word;
    size_t n;
    int count = 0, i;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    o.dst = dst;
    o.cap = dstsz;
    o.len = 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    n = strlen(word);

    if (first_word) {
        for (i = 0; commands[i]; i++)
            if (!strncmp(commands[i], word, n)) {
                out(&o, "%s\n", commands[i]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' ')) {
        static const char *const kinds[] = { "dis", "mem", "vram", "regs",
                                             NULL };
        for (i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n)) {
                out(&o, "%s\n", kinds[i]);
                count++;
            }
        return count;
    }
    for (i = 0; adamdebug_reg_names[i]; i++)
        if (n && !strncmp(adamdebug_reg_names[i], word, n)) {
            out(&o, "%s\n", adamdebug_reg_names[i]);
            count++;
        }
    if (n && o.len < o.cap - 1)
        count += adamdebug_label_completions(d, word, dst + o.len,
                                             o.cap - o.len, 200);
    return count;
}
