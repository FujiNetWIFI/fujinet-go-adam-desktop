/*
 * AdamNet packet decoder for the debugger's FujiNet page: re-frames the
 * bytes adamcore's BoIP trace hands over, names each packet, and keeps the
 * newest ADAMFUJI_RING of them, with byte-identical exchanges coalesced.
 * No emulator or locking in here, so a test can drive it with plain bytes;
 * the engine (debugger.c) owns one under its own mutex.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_FUJITRACE_H
#define ADAM_FUJITRACE_H

#include <stdint.h>

#include "adamdebug.h"

#define ADAMFUJI_RING   512
#define ADAMFUJI_EX_MAX 4    /* longest exchange that can coalesce */
#define ADAMFUJI_PKT_MAX 1028 /* DATA: header, length, 1024, checksum */

typedef struct {
    uint32_t sent, answered;  /* packets to and from this address */
    int has_status;           /* its last STATUS reply: */
    uint16_t max_len;
    uint8_t dev_type, status;
} adamfuji_device;

typedef struct {
    /* newest ADAMFUJI_RING entries; entry k (0 = first ever) is at k % RING */
    adam_fuji_packet ring[ADAMFUJI_RING];
    uint64_t pushed;     /* entries ever pushed */
    uint64_t cleared_at; /* pushed when last cleared */
    uint64_t serial;

    /* re-framing, indexed by to_host */
    uint8_t acc[2][ADAMFUJI_PKT_MAX];
    int acc_len[2];

    /* the Fuji/network command a device's next DATA answers, or -1 */
    int pending_cmd[16];

    /* coalescing: the last exchange (a request and its answers) in the
     * ring, and the packets of a new one that match it so far */
    uint64_t ex_first;   /* ring index of its first entry */
    int ex_n;            /* its entries; 0 = none to match */
    int ex_ok;           /* short enough to coalesce */
    uint64_t ex_hash[ADAMFUJI_EX_MAX];
    int ex_to_host[ADAMFUJI_EX_MAX];
    adam_fuji_packet held[ADAMFUJI_EX_MAX];
    uint64_t held_hash[ADAMFUJI_EX_MAX];
    int held_n;
    int matching, bumped;

    /* counters for the state pane; never cleared with the log */
    uint32_t packets[2];
    uint64_t bytes[2];
    uint32_t bad_ck, junk, cut;
    adamfuji_device dev[16];
} adamfuji;

void adamfuji_init(adamfuji *t);

/* Feeds bytes off the wire. to_host 1: FujiNet -> ADAM, any chunking;
 * 0: ADAM -> FujiNet. A request throws away a partial answer, as the
 * master does. */
void adamfuji_feed(adamfuji *t, int to_host, const uint8_t *buf, int len);

/* The newest max entries, oldest first. Returns the count. */
int  adamfuji_read(const adamfuji *t, adam_fuji_packet *out, int max);
void adamfuji_clear(adamfuji *t);

/* Packet counters and the addresses seen, as text lines. Returns the
 * length written. */
int  adamfuji_format_state(const adamfuji *t, char *dst, int dstsz);

/* "FujiNet", "D1", ... for an AdamNet address. */
const char *adamfuji_device_name(int dev);

#endif /* ADAM_FUJITRACE_H */
