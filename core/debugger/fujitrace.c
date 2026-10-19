/*
 * AdamNet packet decoder for the debugger's FujiNet page (see fujitrace.h).
 *
 * Every packet starts with (type << 4) | address. Master -> node: SEND is
 * the header, a big-endian length, the payload and an XOR checksum; every
 * other type is the header alone. Node -> master: STATUS is six bytes,
 * DATA is laid out like SEND, ACK and NAK are the header alone -- the same
 * framing adamcore's packet_len() reads.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujitrace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fuji_names.h"

#define ARROW "\xe2\x86\x92" /* → */
#define TIMES "\xc3\x97"     /* × */

enum { KIND_PACKET, KIND_JUNK, KIND_CUT };

static const char *const dev_names[16] = {
    "MASTER", "KBD", "PRN", "CLK", "D1",  "D2",  "D3",  "D4",
    "TAPE",   "NET", "NET", "NET", "NET", "NET", "NET", "FujiNet"};

const char *adamfuji_device_name(int dev)
{
    return dev_names[dev & 0x0F];
}

static const char *fuji_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof(fuji_command_names) / sizeof(fuji_command_names[0]);
         i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* Packet type names, master -> node and node -> master. */
static const char *type_name(int to_host, uint8_t hdr)
{
    static const char *const out[16] = {
        "RESET", "STATUS", "ACK",  "CLR",  "RECEIVE", "CANCEL", "SEND", "NACK",
        NULL,    NULL,     NULL,   NULL,   NULL,      "READY",  NULL,   NULL};
    static const char *const in[16] = {
        NULL,     NULL,  NULL, NULL,   NULL,  NULL, NULL, NULL,
        "STATUS", "ACK", NULL, "DATA", "NAK", NULL, NULL, NULL};
    const char *n = (to_host ? in : out)[hdr >> 4];
    return n ? n : "?";
}

static int is_named_dev(int dev)
{
    return dev == 0x0F || (dev >= 0x09 && dev <= 0x0E);
}

static uint8_t xor_ck(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}

static uint64_t hash_bytes(int to_host, const uint8_t *p, int n)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)to_host;
    int i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    h ^= (uint64_t)n;
    return h * 1099511628211ull;
}

static int addf(char *dst, int off, int sz, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (off >= sz - 1) return off;
    va_start(ap, fmt);
    n = vsnprintf(dst + off, (size_t)(sz - off), fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    return off + n < sz - 1 ? off + n : sz - 1;
}

/* Framed size of the packet at p[0..have): >0 complete and this long, 0 a
 * header still waiting for the rest, -1 p[0] cannot begin a packet. */
static int frame_len(int to_host, const uint8_t *p, int have)
{
    int type = p[0] >> 4, len;
    if (to_host) {
        if (type == 0x8) return have >= 6 ? 6 : 0;
        if (type == 0x9 || type == 0xC) return 1;
        if (type != 0xB) return -1;
    } else if (type != 0x6) {
        return 1;
    }
    if (have < 3) return 0;
    len = (p[1] << 8) | p[2];
    if (len > 1024) return -1; /* no real SEND or DATA is this long */
    return have >= 4 + len ? 4 + len : 0;
}

/* ---- naming --------------------------------------------------------------- */

static void describe(adamfuji *t, int to_host, const uint8_t *p, int n,
                     int kind, adam_fuji_packet *e)
{
    const int sz = (int)sizeof(e->text);
    int dev = p[0] & 0x0F, type = p[0] >> 4, off, len, ok;

    memset(e, 0, sizeof(*e));
    e->to_host = to_host;
    e->nbytes = n;
    memcpy(e->data, p, n < (int)sizeof(e->data) ? (size_t)n : sizeof(e->data));
    e->device = e->command = e->length = -1;
    e->repeat = 1;

    if (kind == KIND_JUNK) {
        snprintf(e->text, sizeof(e->text), "%s %02X, not a packet",
                 to_host ? "? " ARROW " ADAM" : "ADAM " ARROW " ?", p[0]);
        return;
    }
    e->device = dev;
    if (to_host)
        off = addf(e->text, 0, sz, "%s " ARROW " ADAM", dev_names[dev]);
    else
        off = addf(e->text, 0, sz, "ADAM " ARROW " %s", dev_names[dev]);
    off = addf(e->text, off, sz, " %02X %s", p[0], type_name(to_host, p[0]));
    if (kind == KIND_CUT) {
        addf(e->text, off, sz, " (%d B, cut off)", n);
        return;
    }

    if ((!to_host && type == 0x6) || (to_host && type == 0xB)) {
        len = (p[1] << 8) | p[2];
        ok = xor_ck(p + 3, len) == p[3 + len];
        e->length = len;
        if (!to_host && is_named_dev(dev) && len > 0) {
            /* A command to FujiNet or a network device: its first byte. */
            e->command = p[3];
            t->pending_cmd[dev] = p[3];
        } else if (to_host && t->pending_cmd[dev] >= 0) {
            /* The answer to the last command sent there. */
            e->command = t->pending_cmd[dev];
            t->pending_cmd[dev] = -1;
        }
        if (e->command >= 0) {
            const char *name = fuji_name(e->command);
            off = addf(e->text, off, sz, " %02X%s%s", e->command,
                       *name ? " " : "", name);
        } else if (!to_host && dev >= 4 && dev <= 8 && len == 5) {
            off = addf(e->text, off, sz, " block %lu",
                       (unsigned long)p[3] | ((unsigned long)p[4] << 8) |
                           ((unsigned long)p[5] << 16) |
                           ((unsigned long)p[6] << 24));
        }
        addf(e->text, off, sz, " (%d B, ck %s)", len, ok ? "ok" : "BAD");
        if (!ok) t->bad_ck++;
    } else if (to_host && type == 0x8) {
        adamfuji_device *d = &t->dev[dev];
        ok = xor_ck(p + 1, 4) == p[5];
        d->has_status = 1;
        d->max_len = (uint16_t)(p[1] | (p[2] << 8));
        d->dev_type = p[3];
        d->status = p[4];
        addf(e->text, off, sz, " %s, max %u, status %02X, ck %s",
             p[3] == 0 ? "char" : p[3] == 1 ? "block" : "type ?",
             d->max_len, p[4], ok ? "ok" : "BAD");
        if (!ok) t->bad_ck++;
    }
}

/* ---- the ring and coalescing ------------------------------------------------ */

static adam_fuji_packet *entry(adamfuji *t, uint64_t k)
{
    return &t->ring[k % ADAMFUJI_RING];
}

static void push(adamfuji *t, const adam_fuji_packet *e)
{
    adam_fuji_packet *s = entry(t, t->pushed++);
    *s = *e;
    s->serial = ++t->serial;
}

/* Rewrites the "×N" on the last exchange's entries, with new serials so a
 * view sees the change. */
static void set_repeat(adamfuji *t, uint32_t repeat)
{
    int i;
    for (i = 0; i < t->ex_n; i++) {
        adam_fuji_packet *e = entry(t, t->ex_first + (uint64_t)i);
        char *x = strstr(e->text, " " TIMES);
        if (x) *x = '\0';
        e->repeat = repeat;
        if (repeat > 1) {
            int off = (int)strlen(e->text);
            addf(e->text, off, (int)sizeof(e->text), " " TIMES "%u", repeat);
        }
        e->serial = ++t->serial;
    }
}

static void bump(adamfuji *t, int by)
{
    set_repeat(t, entry(t, t->ex_first)->repeat + (uint32_t)by);
    t->bumped = by > 0;
}

static void add_to_exchange(adamfuji *t, const adam_fuji_packet *e,
                            uint64_t h)
{
    if (t->ex_n == 0) {
        t->ex_first = t->pushed;
        t->ex_ok = 1;
    }
    if (t->ex_n < ADAMFUJI_EX_MAX) {
        t->ex_hash[t->ex_n] = h;
        t->ex_to_host[t->ex_n] = e->to_host;
    } else {
        t->ex_ok = 0;
    }
    push(t, e);
    t->ex_n++;
}

static void start_exchange(adamfuji *t, const adam_fuji_packet *e, uint64_t h)
{
    t->ex_n = 0;
    add_to_exchange(t, e, h);
}

/* A would-be repeat turned out different: it is an exchange of its own. */
static void flush_held(adamfuji *t)
{
    int i, n = t->held_n;
    if (t->bumped) bump(t, -1);
    t->matching = 0;
    t->held_n = 0;
    for (i = 0; i < n; i++) {
        if (i == 0)
            start_exchange(t, &t->held[0], t->held_hash[0]);
        else
            add_to_exchange(t, &t->held[i], t->held_hash[i]);
    }
}

/* An exchange is a request and the answers that follow it. One that repeats
 * the last exchange byte for byte only adds to its count; its packets are
 * held while they still match, and counted as soon as they all have. */
static void emit(adamfuji *t, const adam_fuji_packet *e, uint64_t h)
{
    if (!e->to_host) {
        if (t->matching) {
            if (t->held_n == t->ex_n) { /* a whole repeat, already counted */
                t->matching = 0;
                t->held_n = 0;
                t->bumped = 0;
            } else {
                flush_held(t);
            }
        }
        if (t->ex_n > 0 && t->ex_ok && !t->ex_to_host[0] &&
            t->ex_hash[0] == h) {
            t->matching = 1;
            t->bumped = 0;
            t->held[0] = *e;
            t->held_hash[0] = h;
            t->held_n = 1;
            if (t->ex_n == 1) bump(t, 1);
            return;
        }
        start_exchange(t, e, h);
        return;
    }
    if (t->matching) {
        if (t->held_n < t->ex_n && t->ex_to_host[t->held_n] &&
            t->ex_hash[t->held_n] == h) {
            t->held[t->held_n] = *e;
            t->held_hash[t->held_n++] = h;
            if (t->held_n == t->ex_n) bump(t, 1);
            return;
        }
        flush_held(t);
    }
    add_to_exchange(t, e, h);
}

static void emit_bytes(adamfuji *t, int to_host, const uint8_t *p, int n,
                       int kind)
{
    adam_fuji_packet e;
    t->packets[to_host]++;
    t->bytes[to_host] += (uint64_t)n;
    if (kind == KIND_JUNK) t->junk++;
    if (kind == KIND_CUT) t->cut++;
    if (kind != KIND_JUNK) {
        if (to_host) t->dev[p[0] & 0x0F].answered++;
        else t->dev[p[0] & 0x0F].sent++;
    }
    describe(t, to_host, p, n, kind, &e);
    emit(t, &e, hash_bytes(to_host, p, n));
}

/* ---- public ------------------------------------------------------------------ */

void adamfuji_init(adamfuji *t)
{
    int i;
    memset(t, 0, sizeof(*t));
    for (i = 0; i < 16; i++)
        t->pending_cmd[i] = -1;
}

void adamfuji_feed(adamfuji *t, int to_host, const uint8_t *buf, int len)
{
    to_host = to_host ? 1 : 0;
    if (!to_host && t->acc_len[1] > 0) {
        /* The master asks again only once it has given up on what it was
         * reading, and throws the partial answer away. */
        emit_bytes(t, 1, t->acc[1], t->acc_len[1], KIND_CUT);
        t->acc_len[1] = 0;
    }
    while (len > 0) {
        uint8_t *acc = t->acc[to_host];
        int *have = &t->acc_len[to_host];
        int take = ADAMFUJI_PKT_MAX - *have, n;

        if (take > len) take = len;
        memcpy(acc + *have, buf, (size_t)take);
        *have += take;
        buf += take;
        len -= take;
        while (*have > 0 && (n = frame_len(to_host, acc, *have)) != 0) {
            if (n < 0) {
                emit_bytes(t, to_host, acc, 1, KIND_JUNK);
                n = 1;
            } else {
                emit_bytes(t, to_host, acc, n, KIND_PACKET);
            }
            *have -= n;
            memmove(acc, acc + n, (size_t)*have);
        }
    }
}

int adamfuji_read(const adamfuji *t, adam_fuji_packet *out, int max)
{
    uint64_t avail = t->pushed - t->cleared_at, k;
    int n = 0;
    if (avail > ADAMFUJI_RING) avail = ADAMFUJI_RING;
    if (max <= 0) return 0;
    if (avail > (uint64_t)max) avail = (uint64_t)max;
    for (k = t->pushed - avail; k < t->pushed; k++)
        out[n++] = t->ring[k % ADAMFUJI_RING];
    return n;
}

void adamfuji_clear(adamfuji *t)
{
    t->cleared_at = t->pushed;
    t->ex_n = 0;
    t->held_n = 0;
    t->matching = 0;
    t->bumped = 0;
}

int adamfuji_format_state(const adamfuji *t, char *dst, int dstsz)
{
    int off = 0, i, any = 0;
    if (dstsz <= 0) return 0;
    dst[0] = '\0';
    off = addf(dst, off, dstsz, "Sent     %lu packets, %llu B\n",
               (unsigned long)t->packets[0], (unsigned long long)t->bytes[0]);
    off = addf(dst, off, dstsz, "Received %lu packets, %llu B\n",
               (unsigned long)t->packets[1], (unsigned long long)t->bytes[1]);
    off = addf(dst, off, dstsz, "Bad checksums %lu\n",
               (unsigned long)t->bad_ck);
    if (t->junk)
        off = addf(dst, off, dstsz, "Not packets   %lu\n",
                   (unsigned long)t->junk);
    if (t->cut)
        off = addf(dst, off, dstsz, "Cut off       %lu\n",
                   (unsigned long)t->cut);
    off = addf(dst, off, dstsz, "\nAddress           Sent  Answered\n");
    for (i = 0; i < 16; i++) {
        const adamfuji_device *d = &t->dev[i];
        if (!d->sent && !d->answered) continue;
        any = 1;
        off = addf(dst, off, dstsz, " %X %-10s %8lu  %8lu\n", i, dev_names[i],
                   (unsigned long)d->sent, (unsigned long)d->answered);
        if (d->has_status)
            off = addf(dst, off, dstsz, "   %s, max %u, status %02X\n",
                       d->dev_type == 0   ? "char"
                       : d->dev_type == 1 ? "block"
                                          : "type ?",
                       d->max_len, d->status);
    }
    if (!any)
        off = addf(dst, off, dstsz, " (none yet)\n");
    return off;
}
