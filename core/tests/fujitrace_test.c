/*
 * The FujiNet page's AdamNet decoder, driven with synthetic wire bytes:
 * framing in both directions, packets split across reads and several in one,
 * naming (packet types, Fuji commands, the DATA that answers one, block
 * numbers, STATUS), checksums, a partial answer cut off by the next request,
 * coalescing of repeated exchanges, the ring limit, clearing, and the empty
 * state. Needs no ROMs, no core and no network.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujitrace.h"

static int failures;

static void check(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void check_text(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s:\n  got  \"%s\"\n  want \"%s\"\n", name, got,
                want);
        failures++;
    }
}

static uint8_t xck(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}

#define FEED(t, to_host, ...)                                                \
    do {                                                                     \
        static const uint8_t b_[] = {__VA_ARGS__};                           \
        adamfuji_feed((t), (to_host), b_, (int)sizeof(b_));                  \
    } while (0)

static adam_fuji_packet out[ADAMFUJI_RING];

static int read_all(adamfuji *t)
{
    return adamfuji_read(t, out, ADAMFUJI_RING);
}

int main(void)
{
    static adamfuji tr;
    adamfuji *t = &tr;
    static char state[4096];
    uint8_t data[4 + 140];
    uint64_t serial;
    int n, i;

    /* empty */
    adamfuji_init(t);
    check("empty: no packets", read_all(t) == 0);
    adamfuji_format_state(t, state, (int)sizeof(state));
    check("empty: state says so", strstr(state, "(none yet)") != NULL);
    check("empty: no packets counted",
          strstr(state, "Sent     0 packets, 0 B") != NULL);

    /* ADAM -> FujiNet: a Fuji command, named from its first byte */
    FEED(t, 0, 0x6F, 0x00, 0x01, 0xE8, 0xE8);
    n = read_all(t);
    check("send: one packet", n == 1);
    check_text("send: text", out[0].text,
               "ADAM \xe2\x86\x92 FujiNet 6F SEND E8 GET ADAPTERCONFIG "
               "(1 B, ck ok)");
    check("send: fields", out[0].to_host == 0 && out[0].device == 15 &&
                              out[0].command == 0xE8 && out[0].length == 1 &&
                              out[0].nbytes == 5 && out[0].repeat == 1);

    /* FujiNet -> ADAM: an ACK and a STATUS in one read */
    FEED(t, 1, 0x9F, 0x8F, 0x00, 0x04, 0x00, 0x40, 0x44);
    n = read_all(t);
    check("ack+status: two packets", n == 3);
    check_text("ack: text", out[1].text,
               "FujiNet \xe2\x86\x92 ADAM 9F ACK");
    check_text("status: text", out[2].text,
               "FujiNet \xe2\x86\x92 ADAM 8F STATUS char, max 1024, "
               "status 40, ck ok");

    /* a 140-byte DATA split over three reads, answering the E8 */
    data[0] = 0xBF;
    data[1] = 0x00;
    data[2] = 140;
    for (i = 0; i < 140; i++)
        data[3 + i] = (uint8_t)(i * 7);
    data[3 + 140] = xck(&data[3], 140);
    FEED(t, 0, 0x3F);
    adamfuji_feed(t, 1, data, 1);
    check("split: nothing until whole", read_all(t) == 4);
    adamfuji_feed(t, 1, data + 1, 50);
    check("split: still waiting", read_all(t) == 4);
    adamfuji_feed(t, 1, data + 51, (int)sizeof(data) - 51);
    n = read_all(t);
    check("split: one DATA", n == 5);
    check_text("data: inherits the command", out[4].text,
               "FujiNet \xe2\x86\x92 ADAM BF DATA E8 GET ADAPTERCONFIG "
               "(140 B, ck ok)");
    check("data: fields", out[4].to_host == 1 && out[4].command == 0xE8 &&
                              out[4].length == 140 && out[4].nbytes == 144 &&
                              memcmp(out[4].data, data, 64) == 0);

    /* the inherited name is used once */
    data[3 + 140] ^= 0xFF;
    FEED(t, 0, 0x3F);
    adamfuji_feed(t, 1, data, (int)sizeof(data));
    n = read_all(t);
    check_text("data: bad checksum, no stale name", out[n - 1].text,
               "FujiNet \xe2\x86\x92 ADAM BF DATA (140 B, ck BAD)");

    /* a block number SEND */
    adamfuji_clear(t);
    check("clear: empty", read_all(t) == 0);
    FEED(t, 0, 0x64, 0x00, 0x05, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x1D);
    n = read_all(t);
    check("block: one packet", n == 1);
    check_text("block: text", out[0].text,
               "ADAM \xe2\x86\x92 D1 64 SEND block 29 (5 B, ck ok)");

    /* a partial answer the master gives up on, then junk */
    FEED(t, 1, 0xB4, 0x04, 0x00, 0x11, 0x22);
    FEED(t, 0, 0x44);
    FEED(t, 1, 0x5A);
    n = read_all(t);
    check("cut: three more", n == 4);
    check_text("cut: text", out[1].text,
               "D1 \xe2\x86\x92 ADAM B4 DATA (5 B, cut off)");
    check_text("cut: then the request", out[2].text,
               "ADAM \xe2\x86\x92 D1 44 RECEIVE");
    check_text("junk: text", out[3].text,
               "? \xe2\x86\x92 ADAM 5A, not a packet");
    check("junk: no device", out[3].device == -1);

    /* the keepalive coalesces, and each repeat is a new serial */
    adamfuji_clear(t);
    FEED(t, 0, 0xD3);
    serial = 0;
    for (i = 0; i < 4; i++) {
        FEED(t, 0, 0xD3);
        n = read_all(t);
        check("keepalive: serial moves", out[n - 1].serial > serial);
        serial = out[n - 1].serial;
    }
    n = read_all(t);
    check("keepalive: one entry", n == 1 && out[0].repeat == 5);
    check_text("keepalive: text", out[0].text,
               "ADAM \xe2\x86\x92 CLK D3 READY \xc3\x97" "5");

    /* a poll and its answer coalesce as a pair */
    for (i = 0; i < 3; i++) {
        FEED(t, 0, 0x12);
        FEED(t, 1, 0x82, 0x01, 0x00, 0x00, 0x00, 0x01);
    }
    n = read_all(t);
    check("poll: two entries", n == 3 && out[1].repeat == 3 &&
                                   out[2].repeat == 3);
    check_text("poll: request text", out[1].text,
               "ADAM \xe2\x86\x92 PRN 12 STATUS \xc3\x97" "3");

    /* a half-matching repeat is held, then shown once it differs */
    FEED(t, 0, 0x12);
    check("held: not shown yet", read_all(t) == 3);
    FEED(t, 1, 0xC2);
    n = read_all(t);
    check("diverged: new exchange", n == 5 && out[1].repeat == 3 &&
                                        out[3].repeat == 1);
    check_text("diverged: request", out[3].text,
               "ADAM \xe2\x86\x92 PRN 12 STATUS");
    check_text("diverged: answer", out[4].text,
               "PRN \xe2\x86\x92 ADAM C2 NAK");

    /* a repeat that grows an extra answer is not a repeat */
    FEED(t, 0, 0x12);
    FEED(t, 1, 0xC2);
    check("regrow: counted", read_all(t) == 5 && out[4].repeat == 2);
    FEED(t, 1, 0x92);
    n = read_all(t);
    check("regrow: uncounted and shown", n == 8 && out[3].repeat == 1 &&
                                             out[4].repeat == 1 &&
                                             out[7].device == 2);
    check_text("regrow: text has no count", out[4].text,
               "PRN \xe2\x86\x92 ADAM C2 NAK");

    /* the ring keeps the newest ADAMFUJI_RING */
    adamfuji_clear(t);
    for (i = 0; i < 600; i++) {
        uint8_t p[5] = {0x6F, 0x00, 0x01, 0x00, 0x00};
        p[3] = p[4] = (uint8_t)i; /* each differs from the one before */
        adamfuji_feed(t, 0, p, 5);
    }
    n = read_all(t);
    check("ring: capped", n == ADAMFUJI_RING);
    check("ring: oldest kept", out[0].data[3] == (uint8_t)(600 - 512));
    for (i = 1; i < n; i++)
        if (out[i].serial <= out[i - 1].serial) break;
    check("ring: oldest first", i == n);
    n = adamfuji_read(t, out, 3);
    check("ring: newest three", n == 3 && out[2].data[3] == (uint8_t)599);

    /* the state pane */
    adamfuji_format_state(t, state, (int)sizeof(state));
    check("state: FujiNet listed", strstr(state, " F FujiNet") != NULL);
    check("state: its STATUS", strstr(state, "char, max 1024, status 40") !=
                                   NULL);
    check("state: D1 listed", strstr(state, " 4 D1") != NULL);
    check("state: bad checksum counted", strstr(state, "Bad checksums 1") !=
                                             NULL);
    check("state: cut counted", strstr(state, "Cut off       1") != NULL);
    {
        char tiny[8];
        int len = adamfuji_format_state(t, tiny, (int)sizeof(tiny));
        check("state: truncates", len == 7 && strlen(tiny) == 7);
    }

    if (failures) {
        fprintf(stderr, "fujitrace_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("fujitrace_test: all ok\n");
    return 0;
}
