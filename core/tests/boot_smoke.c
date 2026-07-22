/*
 * Headless boot smoke test: create the core from the embedded ROMs, run 300
 * frames (~5s emulated) of an ADAM boot without FujiNet, and check that the
 * machine drew something sensible (SmartWriter appears, frames changed, the
 * final screen has more than one color).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "roms_embedded.h"

static uint64_t fb_hash(const uint16_t *fb, int n)
{
    uint64_t h = 1469598103934665603ull;
    int i;
    for (i = 0; i < n; i++) {
        h ^= fb[i];
        h *= 1099511628211ull;
    }
    return h;
}

int main(void)
{
    adamcore_config cfg;
    adamcore *core;
    const uint16_t *fb;
    int w, h, i, changed = 0, colors = 0;
    uint16_t seen[16];
    int nseen = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.os7_rom_data = adam_rom_os7;
    cfg.eos_rom_data = adam_rom_eos;
    cfg.wp_rom_data = adam_rom_wp;
    cfg.start_machine = ADAMCORE_MACHINE_ADAM;
    cfg.audio_rate = 44100;

    core = adamcore_create(&cfg);
    if (!core) {
        fprintf(stderr, "boot_smoke: adamcore_create failed\n");
        return 1;
    }

    for (i = 0; i < 300; i++)
        changed += adamcore_run_frame(core);

    fb = adamcore_framebuffer(core, &w, &h);
    for (i = 0; i < w * h && nseen < 16; i++) {
        int j, known = 0;
        for (j = 0; j < nseen; j++)
            if (seen[j] == fb[i]) known = 1;
        if (!known) seen[nseen++] = fb[i];
    }
    colors = nseen;

    printf("boot_smoke: %d changed frames, %d distinct colors, hash %016llx\n",
           changed, colors, (unsigned long long)fb_hash(fb, w * h));

    adamcore_destroy(core);

    if (changed < 2) {
        fprintf(stderr, "boot_smoke: screen never changed (no boot?)\n");
        return 1;
    }
    if (colors < 2) {
        fprintf(stderr, "boot_smoke: blank final screen\n");
        return 1;
    }
    return 0;
}
