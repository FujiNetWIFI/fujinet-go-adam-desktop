/*
 * iostate.c -- the machine as the Cart & I/O and Sound tabs show it: the
 * memory configuration, the controllers, the VDP's host side, the CPU's
 * interrupt state, the Super Game Module, the cartridge window, FujiNet and
 * AdamNet, and the sound chips. Everything comes from adamcore's
 * emulator-thread accessors (adamcore_debug.h) -- never from the live sound
 * chips, which the audio thread owns -- and is read under the engine lock, so
 * a session restart cannot free the core underneath.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/session_internal.h"
#include "debugger_internal.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void emit(outbuf *buf, const char *fmt, ...)
{
    va_list args;
    int wrote;
    if (buf->len >= buf->cap - 1) return;
    va_start(args, fmt);
    wrote = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, args);
    va_end(args);
    if (wrote > 0)
        buf->len += wrote < buf->cap - buf->len ? wrote : buf->cap - buf->len - 1;
}

/* ---- the controllers ------------------------------------------------------- */

/* The keypad wire codes adamcore'sess controller_read returns, by key: the
 * inverse of its table, kept here because the table is private to it. */
static const uint8_t k_keypad_code[12] = {
    0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05, 0x01, 0x0B, 0x09, 0x06
};

/* The word holds the key INDEX (0-9, 10 '*', 11 '#', 12+ none), exactly what
 * controller_read feeds its table with. */
static int keypad_key(uint8_t low_byte)
{
    const int key = low_byte & 0x0F;
    return key < 12 ? key : -1;
}

static void keypad_name(int key, char *dst)
{
    if (key < 0) {
        dst[0] = '\0';
    } else if (key < 10) {
        dst[0] = (char)('0' + key);
        dst[1] = '\0';
    } else {
        dst[0] = key == 10 ? '*' : '#';
        dst[1] = '\0';
    }
}

/* ---- Cart & I/O -------------------------------------------------------------- */

int adamdebug_io_get(adamdebug *dbg, adamdebug_io *out)
{
    adamcore *core;
    adamdebug_memmap map;
    adamcore_vdp_latch latch;
    adamcore_cpu_aux_state aux;
    adamcore_sgm_info sgm;
    adamsession *sess;
    uint16_t joy[2];
    uint8_t joy_mode = 0;
    int port, line = 0, swap;
    uint64_t frame = 0;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!dbg) return 0;
    sess = adamdebug_session_of(dbg);
    swap = sess->opts.swap_buttons;
    adamdebug_memmap_get(dbg, &map);

    adamdebug_lock(dbg);
    core = adamdebug_core_locked(dbg);
    if (!core) {
        adamdebug_unlock(dbg);
        return 0;
    }
    out->valid = 1;

    adamcore_controller_state(core, joy, &joy_mode);
    out->joy_mode = joy_mode;
    for (port = 0; port < 2; port++) {
        uint8_t high = (uint8_t)(joy[port] >> 8);
        uint8_t low_byte = (uint8_t)(joy[port] & 0xFF);
        const int key = keypad_key(low_byte);

        out->pad[port].raw = joy[port];
        if (swap) {
            const uint8_t high_fire = (uint8_t)(high & 0x40), low_fire = (uint8_t)(low_byte & 0x40);
            high = (uint8_t)((high & ~0x40) | low_fire);
            low_byte = (uint8_t)((low_byte & ~0x40) | high_fire);
        }
        out->pad[port].joystick_byte = (uint8_t)(0xB0 | (high & 0x4F));
        out->pad[port].keypad_byte =
            (uint8_t)(0xB0 | (low_byte & 0x40) | (key < 0 ? 0x0F : k_keypad_code[key]));
        out->pad[port].up = !(high & 0x01);
        out->pad[port].right = !(high & 0x02);
        out->pad[port].down = !(high & 0x04);
        out->pad[port].left = !(high & 0x08);
        out->pad[port].fire_left = !(high & 0x40);
        out->pad[port].fire_right = !(low_byte & 0x40);
        out->pad[port].keypad = key;
        keypad_name(key, out->pad[port].keypad_name);
    }

    adamcore_vdp_state(core, out->vdp_regs, &out->vdp_status, &out->vdp_addr);
    adamcore_vdp_latch_state(core, &latch);
    out->vdp_latch = latch.latch;
    out->vdp_latched = latch.latched;
    out->vdp_read_ahead = latch.read_ahead;
    out->vdp_int_line = latch.int_line;

    adamcore_cpu_aux(core, &aux);
    out->cpu_q = aux.q;
    out->ei_pending = aux.ei_pending;
    out->irq_pending = aux.irq_pending;
    out->nmi_pending = aux.nmi_pending;
    out->nmi_count = aux.nmi_count;

    adamcore_debug_beam(core, &line, &frame);
    out->beam_line = line;
    out->beam_tstate = adamcore_debug_beam_tstate(core);
    out->frames = frame;

    adamcore_sgm_state(core, &sgm);
    out->sgm = sgm.configured;
    out->sgm_ram_en = sgm.ram_en;
    out->sgm_bios_off = sgm.bios_off;
    out->ay_addr = sgm.ay_addr;
    memcpy(out->ay_regs, sgm.ay_regs, sizeof out->ay_regs);
    adamdebug_unlock(dbg);

    out->mem_ctrl = map.mem_ctrl;
    out->net_ctrl = map.net_ctrl;
    out->game_mode = map.game_mode;
    out->lower = map.lower;
    out->upper = map.upper;
    out->lower_name = map.lower_name;
    out->upper_name = map.upper_name;
    out->eos_rom = map.eos_rom;
    out->net_reset = map.net_reset;
    out->cv_console = map.cv_console;

    out->fujinet_runtime = adamsession_fujinet_running(sess) || adamsession_fujinet_external(sess);
    out->fujinet_link = out->fujinet_runtime && adamsession_is_running(sess) &&
                        adamsession_fujinet_link(sess);
    adamdebug_fuji_packets(dbg, out->fujinet_packets);
    return 1;
}

static const char *pad_text(const adamdebug_io *state, int port, char *dst, size_t cap)
{
    size_t len = 0;
    dst[0] = '\0';
#define ADD(flag, word)                                                                      \
    do {                                                                                     \
        if (state->pad[port].flag)                                                           \
            len += (size_t)snprintf(dst + len, cap - len, "%s%s", len ? " " : "", word);    \
    } while (0)
    ADD(up, "up");
    ADD(right, "right");
    ADD(down, "down");
    ADD(left, "left");
    ADD(fire_left, "fire-L");
    ADD(fire_right, "fire-R");
#undef ADD
    if (state->pad[port].keypad >= 0)
        len += (size_t)snprintf(dst + len, cap - len, "%skey %s", len ? " " : "",
                                state->pad[port].keypad_name);
    if (!len)
        snprintf(dst, cap, "idle");
    return dst;
}

/* The Z80'sess I/O decode (adamcore machine.c io_read/io_write): the top three
 * address bits pick the device. */
int adamdebug_io_state(adamdebug *dbg, char *dst, int dstsz)
{
    adamdebug_io state;
    outbuf buf = { dst, dstsz, 0 };
    char pad[96];
    int port, idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!dbg) return 0;
    if (!adamdebug_io_get(dbg, &state)) {
        emit(&buf, "The machine is not running.\n");
        return buf.len;
    }

    emit(&buf, "Console      %s, last reset %s\n", state.cv_console ? "ColecoVision" : "ADAM",
         state.game_mode ? "game" : "computer");
    emit(&buf, "Memory map   $7F=%02X  lower bank %d (%s)  upper bank %d (%s)\n", state.mem_ctrl,
         state.lower, state.lower_name, state.upper, state.upper_name);
    switch (state.lower) {
    case 0:
        if (state.eos_rom) {
            emit(&buf, "  $0000-$5FFF  SmartWriter ROM\n");
            emit(&buf, "  $6000-$7FFF  EOS ROM\n");
        } else {
            emit(&buf, "  $0000-$7FFF  SmartWriter ROM\n");
        }
        break;
    case 1: emit(&buf, "  $0000-$7FFF  RAM (intrinsic 64K)\n"); break;
    case 2: emit(&buf, "  $0000-$7FFF  expansion RAM\n"); break;
    default:
        emit(&buf, "  $0000-$1FFF  OS7 ROM\n");
        if (state.cv_console) {
            emit(&buf, "  $2000-$5FFF  open bus\n");
            emit(&buf, "  $6000-$7FFF  1K RAM, mirrored every $400\n");
        } else {
            emit(&buf, "  $2000-$7FFF  RAM\n");
        }
        break;
    }
    switch (state.upper) {
    case 0: emit(&buf, "  $8000-$FFFF  RAM (intrinsic 64K)\n"); break;
    case 1: emit(&buf, "  $8000-$FFFF  expansion ROM socket (empty, reads $FF)\n"); break;
    case 2: emit(&buf, "  $8000-$FFFF  expansion RAM\n"); break;
    default: emit(&buf, "  $8000-$FFFF  cartridge ROM\n"); break;
    }
    emit(&buf, "Net control  $3F=%02X  net reset %s, EOS ROM %s\n", state.net_ctrl,
         state.net_reset ? "high" : "low", state.eos_rom ? "on" : "off");
    emit(&buf, "\nPorts (A7-A5 pick the device)\n");
    emit(&buf, "  $20-$3F  net control ($3F): D0 net reset, D1 EOS ROM at $6000\n");
    emit(&buf, "  $40-$5F  Super Game Module $50-$53 (not on a stock console)\n");
    emit(&buf, "  $60-$7F  memory control ($7F): D1-D0 lower bank, D3-D2 upper%s\n",
         state.cv_console ? " (ADAM only)" : "");
    emit(&buf, "  $80-$9F  strobe: keypad     $C0-$DF  strobe: joystick\n");
    emit(&buf, "  $A0-$BF  VDP: even data, odd control (write) / status (read)\n");
    emit(&buf, "  $E0-$FF  read: controllers (bit 1 picks the port); write: PSG\n");

    emit(&buf, "\nControllers  strobe %s half ($%s)\n", state.joy_mode ? "joystick" : "keypad",
         state.joy_mode ? "C0" : "80");
    for (port = 0; port < 2; port++)
        emit(&buf, "  port %d     word %04X  reads %02X / %02X  %s\n", port + 1,
             state.pad[port].raw, state.pad[port].joystick_byte, state.pad[port].keypad_byte,
             pad_text(&state, port, pad, sizeof pad));
    emit(&buf, "VDP          ");
    for (idx = 0; idx < 8; idx++)
        emit(&buf, "R%d=%02X ", idx, state.vdp_regs[idx]);
    emit(&buf, "\n             status %02X, address %04X\n", state.vdp_status, state.vdp_addr);
    emit(&buf, "  control port  latch %02X %s  read-ahead %02X  INT %s\n", state.vdp_latch,
         state.vdp_latched ? "(waiting for the second byte)" : "(idle)", state.vdp_read_ahead,
         state.vdp_int_line ? "asserted" : "low");
    emit(&buf, "CPU          NMI %s  IRQ %s  EI shadow %s  Q %02X  VBlank NMIs %llu\n",
         state.nmi_pending ? "pending" : "none", state.irq_pending ? "pending" : "none",
         state.ei_pending ? "yes" : "no", state.cpu_q, (unsigned long long)state.nmi_count);
    emit(&buf, "Beam         line %d, T-state %d, %llu frames\n", state.beam_line, state.beam_tstate,
         (unsigned long long)state.frames);
    if (state.sgm) {
        emit(&buf, "Super Game Module   RAM %s, BIOS %s, AY register %X\n",
             state.sgm_ram_en ? "on" : "off", state.sgm_bios_off ? "off (RAM)" : "on", state.ay_addr);
        emit(&buf, "  AY");
        for (idx = 0; idx < 16; idx++)
            emit(&buf, " %02X", state.ay_regs[idx]);
        emit(&buf, "\n");
    }
    if (!state.fujinet_runtime) {
        emit(&buf, "FujiNet      not running (AdamNet devices: keyboard, printer, tape and disks"
                   " are the machine's own)\n");
    } else {
        emit(&buf, "FujiNet      %s over AdamNet BoIP; %u packets to FujiNet, %u from it\n",
             state.fujinet_link ? "connected" : "waiting", (unsigned)state.fujinet_packets[0],
             (unsigned)state.fujinet_packets[1]);
    }
    return buf.len;
}

/* ---- the cartridge ------------------------------------------------------------ */

void adamdebug_cart_info(adamdebug *dbg, adamdebug_cart *out)
{
    adamsession *sess;
    adamdebug_memmap map;
    adamcore *core;
    struct stat statbuf;
    uint8_t head[2] = { 0, 0 };
    int have_core;

    if (!out) return;
    memset(out, 0, sizeof *out);
    out->cart_size = -1;
    if (!dbg) return;
    sess = adamdebug_session_of(dbg);
    adamdebug_memmap_get(dbg, &map);

    adamdebug_lock(dbg);
    core = adamdebug_core_locked(dbg);
    have_core = core != NULL;
    if (core && ((map.mem_ctrl >> 2) & 3) == 3)
        adamcore_peek_block(core, 0x8000, head, 2);
    adamdebug_unlock(dbg);
    if (!have_core) return;

    out->valid = 1;
    out->cv_console = map.cv_console;
    snprintf(out->machine, sizeof out->machine, "%s", map.cv_console ? "ColecoVision" : "ADAM");
    out->mem_ctrl = map.mem_ctrl;
    out->net_ctrl = map.net_ctrl;
    snprintf(out->config, sizeof out->config, "lower %s, upper %s, EOS ROM %s", map.lower_name,
             map.upper_name, map.eos_rom ? "on" : "off");
    out->cart_visible = ((map.mem_ctrl >> 2) & 3) == 3;
    if (sess->cart_path[0]) {
        out->cartridge = 1;
        snprintf(out->cart_path, sizeof out->cart_path, "%s", sess->cart_path);
        if (stat(sess->cart_path, &statbuf) == 0)
            out->cart_size = (long)statbuf.st_size;
    }
    if (out->cart_visible && out->cartridge) {
        if (head[0] == 0xAA && head[1] == 0x55)
            out->cart_header = 1;
        else if (head[0] == 0x55 && head[1] == 0xAA)
            out->cart_header = 2;
        else
            out->cart_header = 3;
    }
    out->fujinet_runtime = adamsession_fujinet_running(sess) || adamsession_fujinet_external(sess);
    out->fujinet_link = out->fujinet_runtime && adamsession_is_running(sess) &&
                        adamsession_fujinet_link(sess);
    snprintf(out->boip, sizeof out->boip, "%s", adamsession_boip_endpoint(sess));
}

int adamdebug_cart_summary(adamdebug *dbg, char *dst, int dstsz)
{
    adamdebug_cart cart;
    outbuf buf = { dst, dstsz, 0 };
    const char *slash;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    adamdebug_cart_info(dbg, &cart);
    if (!cart.valid) {
        emit(&buf, "no machine");
        return buf.len;
    }
    emit(&buf, "%s, memory %02X (%s)", cart.machine, cart.mem_ctrl, cart.config);
    if (cart.cartridge) {
        slash = strrchr(cart.cart_path, '/');
#ifdef _WIN32
        {
            const char *back_slash = strrchr(cart.cart_path, '\\');
            if (back_slash && (!slash || back_slash > slash)) slash = back_slash;
        }
#endif
        emit(&buf, ", cartridge %s", slash ? slash + 1 : cart.cart_path);
        if (cart.cart_size >= 0) emit(&buf, " (%ld bytes)", cart.cart_size);
        if (!cart.cart_visible) emit(&buf, ", not mapped");
    } else {
        emit(&buf, ", no cartridge");
    }
    if (cart.fujinet_runtime)
        emit(&buf, ", FujiNet %s", cart.fujinet_link ? "connected" : "waiting");
    else
        emit(&buf, ", no FujiNet");
    return buf.len;
}

/* ---- the memory map ------------------------------------------------------------- */

typedef struct {
    adamdebug_region *out;
    int max, count;
} map_builder;

/* Adds a region, joining it to the previous one when the name and the access
 * are the same. */
static void map_add(map_builder *map, uint16_t low, uint16_t high, int readable, int writable,
                    const char *name)
{
    if (map->count > 0) {
        adamdebug_region *last = &map->out[map->count - 1];
        if (last->readable == readable && last->writable == writable &&
            !strcmp(last->name, name) && (unsigned)last->high + 1 == low) {
            last->high = high;
            return;
        }
    }
    if (map->count >= map->max) return;
    map->out[map->count].low = low;
    map->out[map->count].high = high;
    map->out[map->count].readable = readable;
    map->out[map->count].writable = writable;
    snprintf(map->out[map->count].name, sizeof map->out[map->count].name, "%s", name);
    map->count++;
}

int adamdebug_memory_map(adamdebug *dbg, adamdebug_region *out, int max)
{
    map_builder map = { out, max, 0 };
    adamdebug_memmap memcfg;
    adamcore *core;
    adamcore_sgm_info sgm;
    adamsession *sess;
    struct stat statbuf;
    char text[64];
    long cart_size = -1;
    int have_core;

    if (!out || max <= 0 || !dbg) return 0;
    sess = adamdebug_session_of(dbg);
    adamdebug_memmap_get(dbg, &memcfg);
    adamdebug_lock(dbg);
    core = adamdebug_core_locked(dbg);
    have_core = core != NULL;
    memset(&sgm, 0, sizeof sgm);
    if (core)
        adamcore_sgm_state(core, &sgm);
    adamdebug_unlock(dbg);
    if (!have_core) return 0;
    if (sess->cart_path[0] && stat(sess->cart_path, &statbuf) == 0)
        cart_size = (long)statbuf.st_size;

    switch (memcfg.lower) {
    case 0:
        if (memcfg.eos_rom) {
            map_add(&map, 0x0000, 0x5FFF, 1, 0, "SmartWriter ROM (lower bank 0)");
            map_add(&map, 0x6000, 0x7FFF, 1, 0, "EOS ROM (net control $3F bit 1)");
        } else {
            map_add(&map, 0x0000, 0x7FFF, 1, 0, "SmartWriter ROM (lower bank 0)");
        }
        break;
    case 1:
        map_add(&map, 0x0000, 0x7FFF, 1, 1, "RAM (lower bank 1, intrinsic 64K)");
        break;
    case 2:
        map_add(&map, 0x0000, 0x7FFF, 1, 1, "Expansion RAM (lower bank 2)");
        break;
    default:
        map_add(&map, 0x0000, 0x1FFF, 1, sgm.bios_off,
                sgm.bios_off ? "SGM RAM (over the BIOS)" : "OS7 ROM (lower bank 3)");
        if (!memcfg.cv_console || sgm.ram_en) {
            map_add(&map, 0x2000, 0x7FFF, 1, 1,
                    memcfg.cv_console ? "SGM RAM" : "RAM (lower bank 3, 24K above OS7)");
        } else {
            map_add(&map, 0x2000, 0x5FFF, 0, 0, "open bus (unpopulated)");
            map_add(&map, 0x6000, 0x63FF, 1, 1, "Console RAM (1K)");
            map_add(&map, 0x6400, 0x7FFF, 1, 1, "Console RAM (mirror of $6000-$63FF)");
        }
        break;
    }

    switch (memcfg.upper) {
    case 0:
        map_add(&map, 0x8000, 0xFFFF, 1, 1, "RAM (upper bank 0, intrinsic 64K)");
        break;
    case 1:
        map_add(&map, 0x8000, 0xFFFF, 0, 0, "open bus (expansion ROM socket empty)");
        break;
    case 2:
        map_add(&map, 0x8000, 0xFFFF, 1, 1, "Expansion RAM (upper bank 2)");
        break;
    default:
        if (cart_size < 0 && !sess->cart_path[0]) {
            map_add(&map, 0x8000, 0xFFFF, 0, 0, "Cartridge (none, upper bank 3)");
        } else if (cart_size >= 0) {
            snprintf(text, sizeof text, "Cartridge (%ld bytes, upper bank 3)", cart_size);
            map_add(&map, 0x8000, 0xFFFF, 1, 0, text);
        } else {
            map_add(&map, 0x8000, 0xFFFF, 1, 0, "Cartridge (upper bank 3)");
        }
        break;
    }
    return map.count;
}

/* ---- Sound --------------------------------------------------------------------------- */

int adamdebug_sound_get(adamdebug *dbg, adamdebug_sound *out)
{
    adamcore *core;
    adamcore_sn_state sn_state;
    adamcore_sgm_info sgm;
    int idx;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!dbg) return 0;
    adamdebug_lock(dbg);
    core = adamdebug_core_locked(dbg);
    if (!core) {
        adamdebug_unlock(dbg);
        return 0;
    }
    adamcore_psg_sn_state(core, &sn_state);
    adamcore_sgm_state(core, &sgm);
    adamdebug_unlock(dbg);

    out->sn.present = 1;
    out->sn.latched_reg = sn_state.latched_reg;
    for (idx = 0; idx < 3; idx++) {
        out->sn.period[idx] = sn_state.period[idx] ? sn_state.period[idx] : 1024;
        out->sn.freq[idx] = 3579545.0 / (32.0 * out->sn.period[idx]);
    }
    for (idx = 0; idx < 4; idx++) {
        out->sn.atten[idx] = sn_state.atten[idx] & 0x0F;
        out->sn.level[idx] = 15 - out->sn.atten[idx];
    }
    out->sn.noise_ctrl = sn_state.noise_ctrl;
    out->sn.noise_white = (sn_state.noise_ctrl & 4) != 0;
    out->sn.noise_rate = sn_state.noise_ctrl & 3;
    out->sn.noise_hz = out->sn.noise_rate == 3
                           ? out->sn.freq[2]
                           : 3579545.0 / (512.0 * (double)(1 << out->sn.noise_rate));

    if (sgm.configured) {
        adamdebug_ay *ay_chip = &out->ay;
        ay_chip->present = 1;
        ay_chip->addr = sgm.ay_addr;
        memcpy(ay_chip->reg, sgm.ay_regs, sizeof ay_chip->reg);
        for (idx = 0; idx < 3; idx++) {
            const int period = ay_chip->reg[idx * 2] | ((ay_chip->reg[idx * 2 + 1] & 0x0F) << 8);
            ay_chip->period[idx] = period;
            ay_chip->freq[idx] = 111860.8 / (period ? period : 1);
            ay_chip->tone_on[idx] = !((ay_chip->reg[7] >> idx) & 1);
            ay_chip->noise_on[idx] = !((ay_chip->reg[7] >> (3 + idx)) & 1);
            ay_chip->level[idx] = ay_chip->reg[8 + idx] & 0x0F;
            ay_chip->envelope[idx] = (ay_chip->reg[8 + idx] & 0x10) != 0;
        }
        ay_chip->noise_period = ay_chip->reg[6] & 0x1F;
        ay_chip->env_period = ay_chip->reg[11] | (ay_chip->reg[12] << 8);
        ay_chip->env_shape = ay_chip->reg[13] & 0x0F;
    }
    return 1;
}
