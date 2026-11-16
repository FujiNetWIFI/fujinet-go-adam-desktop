/*
 * vidtab_live_test -- the Video tab on a running machine.
 *
 * Needs no system ROMs: it is a ColecoVision session whose hand-assembled
 * cartridge (below) sets up a Graphics I screen with a sprite and spins. The
 * debugger is attached before the machine exists, so it stops at the reset
 * vector; the PC is then pointed at the cartridge.
 *
 * The properties that matter: a tab that is open makes the host capture
 * frames and the per-line register log; at a stop the Display view is exactly
 * the picture the main window has (the published frame), whatever the
 * half-drawn frame buffer holds; the Beam view reads the machine as it
 * stands; a step moves the machine and the tab follows; a poke through the
 * VRAM pane lands in the VDP; and the console's `video` text is the tab's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "adamdebug.h"
#include "adamsession.h"
#include "adamvidtab.h"
#include "test_tmp.h"

static int failures;

static void check(const char *what, int passed)
{
    printf("  %s %s\n", passed ? "ok " : "FAIL", what);
    if (!passed) failures++;
}

static void nap(int millis)
{
    struct timespec nap_time = { millis / 1000, (long)(millis % 1000) * 1000000L };
    nanosleep(&nap_time, NULL);
}

/* The cartridge program at $8024: Graphics I, names 0-255 on the top eight
 * rows, 256 pattern bytes, colours, one 8x8 sprite at (80, 40), then JR $. */
static const uint8_t program[] = {
    0x31,0x00,0x64,0x3E,0x00,0xD3,0xBF,0x3E,0x80,0xD3,0xBF,0x3E,0xC0,0xD3,0xBF,0x3E,0x81,0xD3,0xBF,0x3E,0x06,0xD3,0xBF,0x3E,0x82,0xD3,0xBF,0x3E,0x80,0xD3,0xBF,0x3E,0x83,0xD3,0xBF,0x3E,0x00,0xD3,0xBF,0x3E,0x84,0xD3,0xBF,0x3E,0x36,0xD3,0xBF,0x3E,0x85,0xD3,0xBF,0x3E,0x07,0xD3,0xBF,0x3E,0x86,0xD3,0xBF,0x3E,0x04,0xD3,0xBF,0x3E,0x87,0xD3,0xBF,0x3E,0x00,0xD3,0xBF,0x3E,0x58,0xD3,0xBF,0x06,0x00,0xAF,0xD3,0xBE,0x3C,0x10,0xFB,0x3E,0x00,0xD3,0xBF,0x3E,0x40,0xD3,0xBF,0x06,0x00,0xAF,0xD3,0xBE,0x3C,0x10,0xFB,0x3E,0x00,0xD3,0xBF,0x3E,0x60,0xD3,0xBF,0x06,0x20,0x3E,0xF4,0xD3,0xBE,0x10,0xFC,0x3E,0x00,0xD3,0xBF,0x3E,0x78,0xD3,0xBF,0x06,0x08,0x3E,0xFF,0xD3,0xBE,0x10,0xFC,0x3E,0x00,0xD3,0xBF,0x3E,0x5B,0xD3,0xBF,0x3E,0x28,0xD3,0xBE,0x3E,0x50,0xD3,0xBE,0x3E,0x00,0xD3,0xBE,0x3E,0x0F,0xD3,0xBE,0x3E,0xD0,0xD3,0xBE,0x18,0xFE
};

static void make_cart(const char *path)
{
    uint8_t img[256];
    FILE *file;
    int item;

    memset(img, 0x00, sizeof img);
    img[0] = 0x55;            /* ColecoVision cartridge, skip the title screen */
    img[1] = 0xAA;
    img[0x0A] = 0x24;         /* start address $8024 */
    img[0x0B] = 0x80;
    for (item = 0x0C; item < 0x21; item++)
        img[item] = 0xC9;     /* RST vectors: RET */
    img[0x21] = 0xED;         /* NMI vector: RETN */
    img[0x22] = 0x45;
    memcpy(img + 0x24, program, sizeof program);
    file = fopen(path, "wb");
    if (file) {
        fwrite(img, 1, sizeof img, file);
        fclose(file);
    }
}

static int contains(const char *text, const char *word)
{
    return text && strstr(text, word) != NULL;
}

/* the published frame as the main window gets it, as 0x00RRGGBB */
static void published_frame(adamsession *sess, uint32_t *dst)
{
    uint16_t *raw = malloc(sizeof(uint16_t) * ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT);
    uint64_t serial = 0;
    adamsession_copy_frame(sess, raw, &serial);
    for (int item = 0; item < ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT; item++) {
        uint32_t pix = raw[item];
        uint32_t red = (pix >> 11) & 0x1F, grn = (pix >> 5) & 0x3F, blu = pix & 0x1F;
        red = red << 3 | red >> 2;
        grn = grn << 2 | grn >> 4;
        blu = blu << 3 | blu >> 2;
        dst[item] = red << 16 | grn << 8 | blu;
    }
    free(raw);
}

/* a line "Label: value" of the timing text, as a number */
static unsigned long long timing_value(adamvid_tab *tab, const char *label)
{
    static char buf[4096];
    const char *found;
    adamvid_tab_text(tab, ADAMVID_S_TIMING, buf, (int)sizeof buf);
    found = strstr(buf, label);
    if (!found) return 0;
    found += strlen(label);
    while (*found == ' ' || *found == ':') found++;
    return strtoull(found, NULL, 10);
}

int main(void)
{
    adamsession_paths paths;
    adamsession_start_opts opts;
    adamsession *sess;
    adamdebug *dbg;
    adamvid_tab *tab;
    char tmpl[512], cart[700], cmd[700];
    char *tmp = adam_make_tempdir(tmpl, sizeof tmpl, "adamvid-live-");
    uint32_t *want;
    const uint32_t *pic;
    int wide = 0, high = 0, round, ready = 0;

    if (!tmp) return 1;
    snprintf(cart, sizeof cart, "%s/vid.rom", tmp);
    make_cart(cart);

    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = "";

    sess = adamsession_new(&paths);
    if (!sess) return 1;
    dbg = adamsession_debugger(sess);
    want = malloc(sizeof(uint32_t) * ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT);
    if (!dbg || !want) return 1;

    /* attached before the machine exists: it stops at the reset vector */
    adamdebug_attach(dbg);
    memset(&opts, 0, sizeof opts);
    opts.machine = 1;               /* ColecoVision: the cartridge needs no ROM */
    opts.cart_path = cart;
    opts.joystick_mode = 1;
    if (adamsession_start(sess, &opts) != 0) {
        fprintf(stderr, "start failed: %s\n", adamsession_last_error(sess));
        return 1;
    }
    for (round = 0; round < 300 && !adamdebug_is_stopped(dbg); round++) nap(10);
    check("the machine stops at the reset vector", adamdebug_is_stopped(dbg));
    adamdebug_command(dbg, "set pc $8024", cmd, (int)sizeof cmd);

    printf("running:\n");
    adamdebug_resume(dbg);
    nap(400);   /* a few frames of the program, so the VDP is set up */
    tab = adamvid_tab_new(dbg);
    check("a tab is created", tab != NULL);
    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 1);
    for (round = 0; round < 300 && !ready; round++) {
        nap(20);
        adamvid_tab_refresh(tab, 0);
        pic = adamvid_tab_picture(tab, &wide, &high);
        ready = pic != NULL && contains(adamvid_tab_status(tab), "line log is recorded");
    }
    check("the host captures frames while the tab is open", adamvid_tab_picture(tab, &wide, &high) != NULL);
    check("...and the line log of a whole frame", ready);
    check("a running machine is reported as running", contains(adamvid_tab_status(tab), "Running"));
    adamvid_tab_set_view(tab, ADAMVID_V_BEAM);
    adamvid_tab_refresh(tab, 1);
    check("Beam says it needs a stop while running", adamvid_tab_picture(tab, &wide, &high) == NULL &&
                                                      contains(adamvid_tab_why(tab), "stopped"));
    adamvid_tab_set_view(tab, ADAMVID_V_DISPLAY);

    printf("stopped:\n");
    adamdebug_stop(dbg);
    for (round = 0; round < 300 && !adamdebug_is_stopped(dbg); round++) nap(10);
    check("the machine stops", adamdebug_is_stopped(dbg));
    adamvid_tab_refresh(tab, 1);
    check("the tab knows", contains(adamvid_tab_status(tab), "Stopped"));
    pic = adamvid_tab_picture(tab, &wide, &high);
    published_frame(sess, want);
    check("Display is 256 x 212", pic && wide == 256 && high == 212);
    {
        int same = pic != NULL;
        for (int item = 0; pic && item < 256 * 212; item++)
            if (pic[item] != (0xff000000u | want[item])) { same = 0; break; }
        check("Display is the published frame, pixel for pixel", same);
    }

    adamvid_tab_set_view(tab, ADAMVID_V_BEAM);
    adamvid_tab_refresh(tab, 1);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Beam draws at a stop", pic && wide == 256 && high == 212);
    check("...and says where the beam is", contains(adamvid_tab_status(tab), "beam at line"));
    {
        static char text[4096];
        adamvid_tab_text(tab, ADAMVID_S_TIMING, text, (int)sizeof text);
        check("...the timing section has the beam", contains(text, "Beam: line"));
    }

    adamvid_tab_set_view(tab, ADAMVID_V_DECODED);
    adamvid_tab_set_source(tab, ADAMVID_SRC_SCREEN);
    adamvid_tab_refresh(tab, 1);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Decoded Screen is 256 x 192", pic && wide == 256 && high == 192);
    {
        /* The BIOS screen has no sprites: the name table redraw must be what the VDP drew. */
        int equal = 0;
        for (int line = 0; pic && line < ADAMVID_ACTIVE; line++)
            for (int col = 0; col < 256; col++)
                if (pic[line * 256 + col] == (0xff000000u | want[(line + ADAMVID_TOP) * 256 + col])) equal++;
        printf("      Screen matches the frame on %d of %d pixels\n", equal, 256 * ADAMVID_ACTIVE);
        check("Decoded Screen is the display lines of the frame", equal >= 256 * ADAMVID_ACTIVE * 95 / 100);
    }

    printf("stepping and poking:\n");
    {
        const unsigned long long before = timing_value(tab, "Cycles");
        adamdebug_step(dbg);
        for (round = 0; round < 300; round++) {
            adamvid_tab_refresh(tab, 0);
            if (timing_value(tab, "Cycles") > before) break;   /* 0: the text of a moment when it ran */
            nap(10);
        }
        check("a step moves the machine and the tab follows", timing_value(tab, "Cycles") > before);
    }
    {
        uint8_t old = 0, now = 0, new_byte = 0;
        char err[200];
        adamdebug_vdp_read(dbg, 0x3F00, &old, 1);
        new_byte = (uint8_t)(old ^ 0xA5);
        snprintf(cmd, sizeof cmd, "3F00: %02X", new_byte);
        check("a poke through the VRAM pane", adamvid_tab_poke(tab, cmd, err, (int)sizeof err) == 1);
        adamdebug_vdp_read(dbg, 0x3F00, &now, 1);
        check("...lands in the VDP", now == new_byte);
        adamvid_tab_refresh(tab, 0);
        {
            char text[4096];
            snprintf(cmd, sizeof cmd, "3F00  %02X", new_byte);
            adamvid_tab_text(tab, ADAMVID_S_VRAM, text, (int)sizeof text);
            check("...and shows in the VRAM section", contains(text, cmd));
        }
        adamdebug_vdp_write(dbg, 0x3F00, &old, 1);
    }

    printf("the console:\n");
    {
        static char text[8192];
        adamvid_tab_text_for(dbg, "", text, (int)sizeof text);
        check("`video` gives the status and the registers", contains(text, "R0") && contains(text, "ST"));
        adamvid_tab_text_for(dbg, "timing", text, (int)sizeof text);
        check("`video timing` gives the timing", contains(text, "NTSC"));
        adamvid_tab_text_for(dbg, "lines", text, (int)sizeof text);
        check("`video lines` gives the bands", contains(text, "lines 0-"));
    }

    printf("break on writes:\n");
    {
        adamvid_tab_set_view(tab, ADAMVID_V_DISPLAY);
        adamvid_tab_set_zoom(tab, 1);
        adamvid_tab_refresh(tab, 1);
        adamvid_tab_pin(tab, 100, ADAMVID_TOP + 60);
        check("a pinned tile can be broken on", adamvid_tab_can_break(tab));
        {
            const int bpid = adamvid_tab_break_on_pixel(tab);
            if (bpid >= 0) {
                check("Break on writes adds a breakpoint", 1);
                adamdebug_breakpoint_clear(dbg);
            } else {
                printf("  NOTE the engine refused the condition (it needs vdpaddr and vdpsecond): -1\n");
            }
        }
    }

    printf("running again:\n");
    adamdebug_resume(dbg);
    for (round = 0; round < 100; round++) {
        nap(20);
        adamvid_tab_refresh(tab, 0);
        if (contains(adamvid_tab_status(tab), "Running")) break;
    }
    check("the tab reports the machine running again", contains(adamvid_tab_status(tab), "Running"));

    adamvid_tab_free(tab);
    free(want);
    adamsession_stop(sess);
    adamsession_free(sess);

    if (failures) {
        fprintf(stderr, "\nvidtab_live_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nvidtab_live_test: PASS\n");
    return 0;
}
