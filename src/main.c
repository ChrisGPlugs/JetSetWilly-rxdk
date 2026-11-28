/*
 * main.c - RXDK + SDL3 front end for the Jet Set Willy port (JetSetWillyX).
 *
 * Ported from the nxdk front end (ChrisGPlugs/JetSet_Willy_NXDK, src/main.c,
 * JSW_COMBINED build): both engines are linked in. With both tapes present it
 * starts on a chooser drawn over the 48K game's own title screen; with one,
 * it runs that game.
 *
 * The game (the translated Z80 code in jsw_code.c / jsw128_code.c) runs as
 * straight-line code; every emulated Spectrum frame (50 Hz) zx.c calls
 * frame_hook() below, which turns that frame's beeper/AY activity into PCM,
 * reads the controller, draws the Spectrum screen and paces everything.
 *
 * What changed from nxdk (as in manicminer-rxdk): audio goes through an SDL3
 * audio stream (RXDK's DirectSound on the MCPX APU) instead of the AC97 fed
 * directly, and the stream's queue paces the game. Video is the 320x240
 * frame drawn by Direct3D (platform_rxdk.cpp). Input is SDL3's gamepad API.
 * Added: 4:3 / 16:9 on the right stick (U:\ASPECT.TXT), the video mode label,
 * JSW READY / SCREEN / STATS lines on the debug output.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "platform.h"
#include "zx.h"
#include "zx_font.h"
#include "jsw.h"
#include "ay.h"
#include "jsw128.h"
#include "z80c.h"

static int v128;
#define JSW_ROOMS (v128 ? 130 : 61)     /* HL9: rooms 0-129; 48K: 0-60 */
#define ROM_FONT 0x3D00
#define AUDIO_RATE 48000

/* Chooser: the 48K title screen with a menu in rows 20-23. Back pressed
 * twice within 1.5 s on either game's title screen returns to it. */
static int      chooser_available, in_chooser, chooser_sel;
static int      ui_up, ui_down, ui_select, ui_stick_y;
static int      back_to_chooser;
static uint64_t last_back_frame;
static jmp_buf  chooser_jmp, chosen_jmp;
#define BACK_DOUBLE_FRAMES 75

static SDL_Gamepad *pad;
static uint32_t frame_pixels[ZX_SCREEN_W * ZX_SCREEN_H];
static uint64_t frame_count;
static uint64_t perf_next;

#define AUDIO_QUEUE_TARGET_MS  80
#define AUDIO_QUEUE_LOW_MS     40

static int      diag_on;
static unsigned diag_underruns, diag_skipped, diag_draw_us;

/* Code-entry screen (original mode only). */
static int in_codes;

/* Back during play asks END GAME? first (1 = asking, 2 = yes, 3 = no); the
 * game is frozen meanwhile. On yes, SHIFT+SPACE is held for quit_frames. */
static int confirm_quit, quit_frames;

/* JSW READY / JSW SCREEN lines for tools/rxdk-run.ps1 -Until. */
static const char *pending_screen;
static int ready_logged;
static void screen_shown(const char *name) { pending_screen = name; }

/* ------------------------------------------------------------------------ */
/* Saves: picture shape and the dashboard's title metadata                   */
/* ------------------------------------------------------------------------ */
static int wide_on, aspect_msg_frames;
static char msg_text[24];             /* shown at the top while aspect_msg_frames > 0 */

/* Volume per screen, balanced by ear on hardware (1x = the nxdk level):
 *   launcher, 48K title   beeper 0.60x
 *   48K in game           beeper 0.85x
 *   128 title             beeper 1.00x, AY music 3x
 *   128 in game           beeper 1.00x, AY music 4x
 * The levels glide to a new setting over ~10 ms so a change never clicks.
 * (The beeper is kept near the 48K level on the 128 game: scaled by 4x, the
 * click when a room change switches the speaker off was an audible pop.) */
#define AY_BASE 1.6f                     /* the nxdk AY mix level */
static float gain_beep = -1.0f, gain_ay; /* current levels */
static unsigned vol_peak, vol_clipped, vol_samples;
#define ASPECT_MSG_FRAMES 100
#define MODE_LABEL_FRAMES 250

static void aspect_load(void)
{
    char path[64], buf[8] = { 0 };
    snprintf(path, sizeof path, "%sASPECT.TXT", plat_save_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    wide_on = !strncmp(buf, "16:9", 4);
}

/* TitleMeta.xbx: UTF-16LE with a BOM, "TitleName=...\r\n". */
static void write_title_meta(void)
{
    char path[64];
    snprintf(path, sizeof path, "%sTitleMeta.xbx", plat_save_dir());
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) return;
    static const char line[] = "TitleName=JetSetWillyX\r\n";
    uint8_t buf[2 + 2 * sizeof line];
    size_t n = 0;
    buf[n++] = 0xFF; buf[n++] = 0xFE;
    for (size_t i = 0; i + 1 < sizeof line; i++) { buf[n++] = (uint8_t)line[i]; buf[n++] = 0; }
    fwrite(buf, 1, n, f);
    if (fclose(f) == 0) plat_commit_file(path);
}

static void aspect_toggle(void)
{
    wide_on = !wide_on;
    plat_set_wide(wide_on);
    write_title_meta();
    char path[64];
    snprintf(path, sizeof path, "%sASPECT.TXT", plat_save_dir());
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(wide_on ? "16:9" : "4:3", f);
        if (fclose(f) == 0) plat_commit_file(path);
    }
    snprintf(msg_text, sizeof msg_text, " %s ", wide_on ? "16:9" : "4:3");
    aspect_msg_frames = ASPECT_MSG_FRAMES;
}

/* ------------------------------------------------------------------------ */
/* Audio output                                                             */
/* ------------------------------------------------------------------------ */
static SDL_AudioStream *audio_stream;
static int audio_ok;
static uint64_t stats_t0_us, stats_samples, stats_frames, stats_t0_z, stats_t0_contended;
static unsigned stats_rate_hz, stats_fps_x100, stats_contention_pct10;

static void audio_open(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!audio_stream) { plat_log("JSW: no audio: %s\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(audio_stream);
    audio_ok = 1;
}

static unsigned audio_queued_ms(void)
{
    if (!audio_ok) return 0;
    int q = SDL_GetAudioStreamQueued(audio_stream);
    return q > 0 ? (unsigned)q / (AUDIO_RATE / 1000 * 4) : 0;
}

static void audio_submit(const int16_t *buf, int frames)
{
    if (!audio_ok) return;
    if (frame_count > 50 && SDL_GetAudioStreamQueued(audio_stream) == 0) diag_underruns++;
    SDL_PutAudioStreamData(audio_stream, buf, frames * 2 * (int)sizeof(int16_t));
}

static void vol_report(const char *why);

static void stats_frame(int samples)
{
    uint64_t now = plat_now_us();
    if (!stats_t0_us) {
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
        return;
    }
    stats_samples += (uint64_t)samples;
    stats_frames++;
    uint64_t dt = now - stats_t0_us;
    if (dt >= 2000000) {
        stats_rate_hz = (unsigned)(stats_samples * 1000000 / dt);
        stats_fps_x100 = (unsigned)(stats_frames * 100000000 / dt);
        uint64_t dz = zx_t - stats_t0_z, dc = zx_contended_t - stats_t0_contended;
        stats_contention_pct10 = dz ? (unsigned)(dc * 1000 / dz) : 0;
        static unsigned windows;
        if (++windows % 5 == 0) {
            plat_log("JSW STATS fps %u.%02u audio %u Hz queued %u ms gaps %u skipped %u contention %u.%u%% room %d\n",
                     stats_fps_x100 / 100, stats_fps_x100 % 100, stats_rate_hz, audio_queued_ms(),
                     diag_underruns, diag_skipped, stats_contention_pct10 / 10, stats_contention_pct10 % 10,
                     mem[JSW_ROOM]);
            vol_report("stats");
        }
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
    }
}

/* ------------------------------------------------------------------------ */
/* Beeper + AY -> PCM                                                       */
/* ------------------------------------------------------------------------ */
static int16_t audio_buf[2 * 2048];
static double  samples_carry;
static uint8_t frame_start_level;
static float   hp_prev_in, hp_prev_out;
static ay_t    ay;

static float level_value(uint8_t level)
{
    return ((level & 0x10) ? 1.0f : 0.0f) + ((level & 0x08) ? 0.06f : 0.0f);
}

static void render_audio(void)
{
    double exact = (double)AUDIO_RATE * ZX_T_PER_FRAME / ZX_CPU_HZ + samples_carry;
    int n = (int)exact;
    samples_carry = exact - n;
    if (n > 2048) n = 2048;

    double t_per_sample = (double)ZX_T_PER_FRAME / n;
    float tb, ta;                            /* target levels for this screen */
    if (in_chooser || (!v128 && jsw_in_title)) tb = 0.60f;
    else if (!v128)                            tb = 0.85f;
    else                                       tb = 1.00f;
    ta = v128 ? AY_BASE * (jsw_in_title ? 3.0f : 4.0f) : 0.0f;
    if (gain_beep < 0.0f) { gain_beep = tb; gain_ay = ta; }
    int ev = 0, ay_ev = 0;
    uint8_t level = frame_start_level;
    for (int i = 0; i < n; i++) {
        double t0 = i * t_per_sample, t1 = t0 + t_per_sample, t = t0;
        double acc = 0.0;
        while (ev < zx_beep_count && zx_beeps[ev].t < t1) {
            double te = zx_beeps[ev].t > t ? zx_beeps[ev].t : t;
            acc += level_value(level) * (te - t);
            t = te;
            level = zx_beeps[ev].level;
            ev++;
        }
        acc += level_value(level) * (t1 - t);
        float x = (float)(acc / t_per_sample);
        gain_beep += (tb - gain_beep) * 0.002f;
        gain_ay   += (ta - gain_ay) * 0.002f;
        x *= gain_beep;
        if (v128) {
            while (ay_ev < zx_ay_count && zx_ay_log[ay_ev].t < t1) {
                ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val);
                ay_ev++;
            }
            x += ay_sample(&ay) * gain_ay;
        }
        float y = x - hp_prev_in + 0.995f * hp_prev_out;
        hp_prev_in = x;
        hp_prev_out = y;
        int s = (int)(y * 9000.0f);
        unsigned a = (unsigned)(s < 0 ? -s : s);
        if (a > vol_peak) vol_peak = a;
        vol_samples++;
        if (s > 32767) { s = 32767; vol_clipped++; }
        if (s < -32768) { s = -32768; vol_clipped++; }
        audio_buf[2 * i] = audio_buf[2 * i + 1] = (int16_t)s;
    }
    frame_start_level = zx_beep_level;
    if (v128)
        while (ay_ev < zx_ay_count) { ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val); ay_ev++; }
    audio_submit(audio_buf, n);
    stats_frame(n);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */
static uint8_t pad_keys[8], macro_keys[8];
static int triggers_held;

static void set_bit(uint8_t *rows, int row, int bit, int down)
{
    if (down) rows[row] |= (uint8_t)(1u << bit);
    else      rows[row] &= (uint8_t)~(1u << bit);
}

/* WRITETYPER macro (works where the original's does: First Landing, at the
 * foot of the stairs). */
typedef struct { uint8_t row, bit; } zx_keypos_t;
static const zx_keypos_t cheat_keys[] = {
    { ZX_ROW_Q_T, 1 }, { ZX_ROW_Q_T, 3 }, { ZX_ROW_P_Y, 2 }, { ZX_ROW_Q_T, 4 }, { ZX_ROW_Q_T, 2 },
    { ZX_ROW_Q_T, 4 }, { ZX_ROW_P_Y, 4 }, { ZX_ROW_P_Y, 0 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 3 },
};
#define CHEAT_LEN  10
#define CHEAT_DONE 10
static int macro_step = -1, macro_timer;
static int in_play;                     /* between STARTGAME and the title screen */

/* Where the game accepts WRITETYPER: First Landing (room 28 in both
 * versions), Willy on the floor at the foot of the stairs (y 208).
 * Anywhere else the letters are only movement keys. */
static int cheat_spot(void)
{
    return in_play && mem[JSW_ROOM] == 28 && mem[JSW_WILLY_Y] == 208;
}

/* Teleport: the two port reads after the game's teleport check are answered
 * with the target room (see the nxdk main.c). */
static int teleport_pending, teleport_target, teleport_armed, teleport_frames;

static uint8_t teleport_in_filter(uint16_t port, uint8_t v)
{
    if (!teleport_armed) return v;
    if (teleport_armed == 1 && port == 0xEFFE) {
        teleport_armed = 2;
        v &= (uint8_t)~0x02;                                   /* '9' */
        if (v128) {
            if (teleport_target & 32)  v &= (uint8_t)~0x04;    /* '8' */
            if (teleport_target & 64)  v &= (uint8_t)~0x08;    /* '7' */
            if (teleport_target & 128) v &= (uint8_t)~0x10;    /* '6' */
        } else if (teleport_target & 32) {
            v &= (uint8_t)~0x10;                               /* '6' */
        }
        return v;
    }
    if (teleport_armed == 2 && port == 0xF7FE) {
        teleport_armed = 0;
        teleport_pending = 0;
        return (uint8_t)((v & 0xE0) | (0x1F & ~teleport_target));
    }
    return v;
}

/* ------------------------------------------------------------------------ */
/* Cheat menu (Black) and room menu (White, with the cheat on)              */
/* ------------------------------------------------------------------------ */
/* The well-known POKEs. Those that change an opcode or a jump target work
 * because tools/Regen-Code.ps1 builds both decodings into the translated
 * code (--variant); the others change data or an operand, which the
 * translated code reads from memory anyway. Same addresses in 48K and 128.
 * A POKE is on while the byte holds its POKEd value. */
static const struct { const char *name; uint16_t addr; uint8_t on, off; } cheats[] = {
    { "Gentle guardians",  37392,   0,   1 },
    { "Nice nasties",      38447, 201,  58 },
    { "Agreeable arrows",  37505,  24,  32 },
    { "Harmless heights",  36578,  16,  12 },
    { "No conveyors",      36601,   0,  95 },
    { "Cheat mode",        34275,  10,   0 },     /* = WRITETYPER typed */
    { "Infinite lives",    35899,   0,  53 },
    { "Infinite time",     35408,  24,  32 },
    { "Superjump",         36404,  44, 188 },
};
#define N_CHEATS (int)(sizeof cheats / sizeof cheats[0])
#define MENU_ROWS 11                    /* room menu: rooms shown at once */

enum { MENU_ROOMS = 1, MENU_CHEATS = 2 };
static int menu;                        /* 0 none, MENU_ROOMS, MENU_CHEATS */
static int menu_sel, menu_close_button, menu_dirty, menu_teleport = -1, cheat_sel;

static void draw_text(uint32_t *dst, int pitch, int x, int y, const char *s, uint32_t fg, uint32_t bg);

static int cheat_on(int i) { return mem[cheats[i].addr] == cheats[i].on; }

/* Room names come from the game's own room data: 48K rooms at 49152, 128
 * rooms in banks 1 (0-63), 3 (64-127) and 4 (128-129); the name is bytes
 * 128-159 of each 256-byte room. */
static void room_name(int r, char *out)
{
    const uint8_t *p;
    if (!v128) p = &mem[49152 + r * 256 + 128];
    else p = zx_bank_ptr(r < 64 ? 1 : r < 128 ? 3 : 4) + (r % 64) * 256 + 128;
    int a = 0, n = 32;                  /* names are centred: trim both ends */
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) n--;
    while (a < n && p[a] == ' ') a++;
    for (int i = a; i < n; i++) out[i - a] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    out[n - a] = 0;
}

static void menu_open(int which, int button)
{
    menu = which;
    menu_close_button = button;
    menu_sel = which == MENU_ROOMS ? mem[JSW_ROOM] % JSW_ROOMS : cheat_sel;
    menu_dirty = 1;
    memset(pad_keys, 0, sizeof pad_keys);
    macro_step = -1;
    plat_log("JSW SCREEN %s\n", which == MENU_ROOMS ? "rooms" : "cheats");
}

static void menu_button(int button)
{
    int n = menu == MENU_ROOMS ? JSW_ROOMS : N_CHEATS;
    int page = menu == MENU_ROOMS ? 10 : n;
    menu_dirty = 1;
    if (menu == MENU_CHEATS) cheat_sel = menu_sel;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    menu_sel = (menu_sel + n - 1) % n; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  menu_sel = (menu_sel + 1) % n; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  menu_sel = menu_sel >= page ? menu_sel - page : 0; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: menu_sel = menu_sel + page < n ? menu_sel + page : n - 1; break;
    case SDL_GAMEPAD_BUTTON_SOUTH:                                    /* A */
        if (menu == MENU_ROOMS) {
            menu_teleport = menu_sel;
            menu = 0;
        } else {
            int on = !cheat_on(menu_sel);
            mem[cheats[menu_sel].addr] = on ? cheats[menu_sel].on : cheats[menu_sel].off;
            plat_log("JSW: cheat %s %s\n", cheats[menu_sel].name, on ? "on" : "off");
        }
        break;
    case SDL_GAMEPAD_BUTTON_EAST:                                     /* B */
    case SDL_GAMEPAD_BUTTON_BACK:
        menu = 0;
        break;
    default:
        if (button == menu_close_button) menu = 0;                    /* same shoulder */
        break;
    }
    if (menu == MENU_CHEATS) cheat_sel = menu_sel;
}

static void menu_draw(uint32_t *dst)
{
    char line[64];
    const uint32_t fg = 0xFFFFFFFF, bg = 0xFF0000A0, hi_fg = 0xFF000000, hi_bg = 0xFFFFFF00;
    const int w = 37, x = (ZX_SCREEN_W - 8 * w) / 2;   /* room names are up to 32 */
    int rows = menu == MENU_ROOMS ? MENU_ROWS : N_CHEATS;
    int y = (ZX_SCREEN_H - 8 * (rows + 4)) / 2;
    snprintf(line, sizeof line, " %-35s ", menu == MENU_ROOMS ? "TELEPORT TO ROOM" : "CHEATS (POKES)");
    draw_text(dst, ZX_SCREEN_W, x, y, line, fg, bg);
    draw_text(dst, ZX_SCREEN_W, x, y + 8, "                                     ", fg, bg);
    if (menu == MENU_ROOMS) {
        int first = menu_sel - MENU_ROWS / 2;
        if (first > JSW_ROOMS - MENU_ROWS) first = JSW_ROOMS - MENU_ROWS;
        if (first < 0) first = 0;
        for (int i = 0; i < MENU_ROWS; i++) {
            int r = first + i;
            char name[33];
            room_name(r, name);
            snprintf(line, sizeof line, "%3d %-32.32s%c", r, name, r == mem[JSW_ROOM] ? '*' : ' ');
            draw_text(dst, ZX_SCREEN_W, x, y + 16 + i * 8, line,
                      r == menu_sel ? hi_fg : fg, r == menu_sel ? hi_bg : bg);
        }
    } else {
        for (int i = 0; i < N_CHEATS; i++) {
            snprintf(line, sizeof line, " %d %-28s %-4s ", i + 1, cheats[i].name, cheat_on(i) ? "ON" : "off");
            draw_text(dst, ZX_SCREEN_W, x, y + 16 + i * 8, line,
                      i == menu_sel ? hi_fg : fg, i == menu_sel ? hi_bg : bg);
        }
    }
    draw_text(dst, ZX_SCREEN_W, x, y + 16 + rows * 8, "                                     ", fg, bg);
    draw_text(dst, ZX_SCREEN_W, x, y + 24 + rows * 8,
              menu == MENU_ROOMS ? " A go  B back  D-pad <> 10 rooms     " : " A on/off  B back                    ", fg, bg);
}

static void game_hook(uint16_t addr)
{
    switch (addr) {
    case JSW_TITLESCREEN:
        in_codes = 0;
        in_play = 0;
        if (v128) jsw_in_title = 1;
        screen_shown(v128 ? "title128" : "title48");
        break;
    case JSW_STARTGAME:
        if (v128) jsw_in_title = 0;
        in_play = 1;
        screen_shown(v128 ? "play128" : "play48");
        break;
    case JSW_ENTERCODES:  in_play = 0; if (!v128) { in_codes = 1; screen_shown("codes"); } break;
    case JSW_TELECHECK:   if (teleport_pending) teleport_armed = 1; break;
    case JSW_MAINLOOP:
        teleport_armed = 0;
        {
            static int was_on;
            int on = mem[JSW_TELEPORT] == CHEAT_DONE;
            if (on && !was_on) plat_log("JSW: cheat on\n");
            was_on = on;
        }
        break;
    default: break;
    }
}

static void run_macros(void)
{
    memset(macro_keys, 0, sizeof macro_keys);
    if (macro_step >= 0) {
        if (macro_timer < 10) set_bit(macro_keys, cheat_keys[macro_step].row, cheat_keys[macro_step].bit, 1);
        if (++macro_timer >= 20) {
            macro_timer = 0;
            if (++macro_step >= CHEAT_LEN) macro_step = -1;
        }
    }
    if (quit_frames > 0) {
        quit_frames--;
        set_bit(macro_keys, ZX_ROW_SHIFT_V, 0, 1);         /* SHIFT+SPACE: quit */
        set_bit(macro_keys, ZX_ROW_SPACE_B, 0, 1);
    }
    if (teleport_pending && ++teleport_frames > 25) { teleport_pending = 0; teleport_armed = 0; }
}

static void vol_report(const char *why)
{
    plat_log("JSW VOL %s %s%s beeper %d.%02dx ay %d.%02dx peak %u clipped %u of %u samples\n", why, in_chooser ? "launcher" : v128 ? "128" : "48K", jsw_in_title ? " title" : "",
             (int)(gain_beep * 100 + 0.5f) / 100, (int)(gain_beep * 100 + 0.5f) % 100,
             (int)(gain_ay / AY_BASE * 100 + 0.5f) / 100, (int)(gain_ay / AY_BASE * 100 + 0.5f) % 100, vol_peak, vol_clipped, vol_samples);
    vol_peak = vol_clipped = vol_samples = 0;
}

/* Pad layout as in the nxdk port, plus right-stick click = 4:3 / 16:9. */
static void pad_button(int button, int down)
{
    if (down && triggers_held && (button == SDL_GAMEPAD_BUTTON_START || button == SDL_GAMEPAD_BUTTON_BACK ||
                                  button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
        return;                                  /* IGR combo in progress */
    if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) { if (down) aspect_toggle(); return; }
    if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK)  { if (down) diag_on = !diag_on; return; }
    if (in_chooser) {
        if (down) {
            if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) ui_up = 1;
            if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) ui_down = 1;
            if (button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_START) ui_select = 1;
        }
        return;
    }
    if (confirm_quit) {
        if (down) {
            if (button == SDL_GAMEPAD_BUTTON_SOUTH) confirm_quit = 2;
            else if (button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_BACK) confirm_quit = 3;
        }
        return;
    }
    if (menu) {
        if (down) menu_button(button);
        return;
    }
    if (button == SDL_GAMEPAD_BUTTON_BACK && down && chooser_available && jsw_in_title) {
        if (last_back_frame && frame_count - last_back_frame <= BACK_DOUBLE_FRAMES)
            back_to_chooser = 1;
        last_back_frame = frame_count;
    }
    if (in_codes) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_WEST:  set_bit(pad_keys, ZX_ROW_1_5, 0, down); return;   /* X: 1 */
        case SDL_GAMEPAD_BUTTON_EAST:  set_bit(pad_keys, ZX_ROW_1_5, 1, down); return;   /* B: 2 */
        case SDL_GAMEPAD_BUTTON_NORTH: set_bit(pad_keys, ZX_ROW_1_5, 2, down); return;   /* Y: 3 */
        case SDL_GAMEPAD_BUTTON_SOUTH: set_bit(pad_keys, ZX_ROW_1_5, 3, down); return;   /* A: 4 */
        case SDL_GAMEPAD_BUTTON_START: set_bit(pad_keys, ZX_ROW_ENTER_H, 0, down); return;
        default: break;
        }
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
        set_bit(pad_keys, ZX_ROW_SPACE_B, 2, down);        /* M: jump / resume */
        break;
    case SDL_GAMEPAD_BUTTON_START:
        if (down) set_bit(pad_keys, jsw_in_title ? ZX_ROW_ENTER_H : ZX_ROW_A_G, 0, 1);
        else { set_bit(pad_keys, ZX_ROW_ENTER_H, 0, 0); set_bit(pad_keys, ZX_ROW_A_G, 0, 0); }
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        set_bit(pad_keys, ZX_ROW_ENTER_H, 4, down);        /* H: tune on/off */
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        set_bit(pad_keys, ZX_ROW_A_G, 0, down);            /* A: pause */
        break;
    case SDL_GAMEPAD_BUTTON_BACK:
        if (!jsw_in_title && !in_codes) {             /* in play: ask first */
            if (down) { confirm_quit = 1; memset(pad_keys, 0, sizeof pad_keys); }
            break;
        }
        set_bit(pad_keys, ZX_ROW_SHIFT_V, 0, down);        /* SHIFT+SPACE: quit */
        set_bit(pad_keys, ZX_ROW_SPACE_B, 0, down);
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:                 /* White */
        if (!down || !in_play) break;
        if (mem[JSW_TELEPORT] == CHEAT_DONE) menu_open(MENU_ROOMS, button);
        else if (macro_step < 0 && cheat_spot()) { macro_step = 0; macro_timer = 0; }
        else if (macro_step < 0) plat_log("JSW: cheat only works in First Landing at the foot of the stairs (room %d, y %d)\n", mem[JSW_ROOM], mem[JSW_WILLY_Y]);
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:                /* Black */
        if (down && in_play) menu_open(MENU_CHEATS, button);
        break;
    default:
        break;
    }
}

static void read_pad_axes(void)
{
    uint8_t joy = 0;
    int left = 0, right = 0, fire = 0;
    plat_check_igr(pad);                      /* doesn't return if the combo is held */
    triggers_held = pad && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000 &&
                    SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    if (pad) {
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        left  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -12000;
        right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 12000;
        fire  = !in_codes && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                              SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
        /* LT / RT: jump left / right (direction + jump together). Both held
         * is the start of the IGR combo, so that does nothing. */
        int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
        int rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
        if (!in_codes && lt != rt) {
            if (lt) left = 1; else right = 1;
            fire = 1;
        }
        int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        int dir = y < -16000 ? -1 : (y > 16000 ? 1 : 0);
        if (dir != ui_stick_y) { if (dir < 0) ui_up = 1; if (dir > 0) ui_down = 1; }
        ui_stick_y = dir;
    }
    if (in_chooser) { zx_kempston = 0; return; }
    if (right) joy |= 0x01;
    if (left)  joy |= 0x02;
    if (fire)  joy |= 0x10;
    zx_kempston = joy;
    /* O / P resume a paused game. Not while WRITETYPER is being typed:
     * a stray O / P on the P-Y row resets the game's key counter. */
    int typing = macro_step >= 0;
    set_bit(pad_keys, ZX_ROW_P_Y, 1, left && !typing);
    set_bit(pad_keys, ZX_ROW_P_Y, 0, right && !typing);
}

static void poll_input(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad);
                pad = NULL;
                memset(pad_keys, 0, sizeof pad_keys);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (!pad) pad = SDL_GetGamepadFromID(e.gbutton.which);
            pad_button(e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            break;
        default:
            break;
        }
    }
    read_pad_axes();
    run_macros();
    for (int row = 0; row < 8; row++)
        zx_keys[row] = (uint8_t)(0x1F & ~(pad_keys[row] | macro_keys[row]));
    if (in_chooser) zx_keys_clear();       /* the title would start on ENTER/0 */
}

/* ------------------------------------------------------------------------ */
/* Video and pacing                                                         */
/* ------------------------------------------------------------------------ */
static int screen_changed(int flash_phase)
{
    static uint8_t last[6912];
    static int last_border = -1, last_flash = -1;
    int has_flash = 0;
    for (int i = 0; i < 768; i++)
        if (mem[0x5800 + i] & 0x80) { has_flash = 1; break; }
    int flash_key = has_flash ? flash_phase : -1;
    if (last_border == zx_border && last_flash == flash_key && !memcmp(last, mem + 0x4000, sizeof last))
        return 0;
    memcpy(last, mem + 0x4000, sizeof last);
    last_border = zx_border;
    last_flash = flash_key;
    return 1;
}

static void draw_text(uint32_t *dst, int pitch, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    for (; *s; s++, x += 8) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 32 || ch > 127) ch = '?';
        for (int py = 0; py < 8; py++) {
            uint8_t bits = mem[ROM_FONT + (ch - 32) * 8 + py];
            uint32_t *row = dst + (y + py) * pitch + x;
            for (int px = 0; px < 8; px++)
                row[px] = (bits & (0x80 >> px)) ? fg : bg;
        }
    }
}

static void draw_diag(uint32_t *dst, int pitch, int x0, int y0)
{
    char lines[6][48];
    unsigned exp_fps_x100 = (unsigned)((uint64_t)ZX_CPU_HZ * 100 / ZX_T_PER_FRAME);
    snprintf(lines[0], 48, " SPEED %3u.%02u FPS (REAL %u.%02u) ", stats_fps_x100 / 100, stats_fps_x100 % 100,
             exp_fps_x100 / 100, exp_fps_x100 % 100);
    snprintf(lines[1], 48, " AUDIO RATE %5u HZ (48000)    ", stats_rate_hz);
    snprintf(lines[2], 48, " AUDIO QUEUED %3u MS  GAPS %-5u", audio_queued_ms(), diag_underruns);
    snprintf(lines[3], 48, " DRAW %2u.%u MS  SKIPPED %-8u", diag_draw_us / 1000, (diag_draw_us / 100) % 10, diag_skipped);
    snprintf(lines[4], 48, " ULA CONTENTION %2u.%u%%          ", stats_contention_pct10 / 10, stats_contention_pct10 % 10);
    snprintf(lines[5], 48, "                               ");
    for (int i = 0; i < 6; i++)
        draw_text(dst, pitch, x0, y0 + i * 10, lines[i], 0xFFFFFFFF, 0xFF0000A0);
}

static void present(void)
{
    int flash = (int)((frame_count >> 4) & 1);
    int msg = aspect_msg_frames > 0;
    if (msg) aspect_msg_frames--;
    int clear_msg = msg && aspect_msg_frames == 0;
    static int last_mode_label;
    int mode_label = in_chooser || frame_count < MODE_LABEL_FRAMES;
    int label_changed = mode_label != last_mode_label;
    last_mode_label = mode_label;
    static int last_confirm;
    int confirm_changed = (confirm_quit == 1) != last_confirm;
    last_confirm = confirm_quit == 1;
    static int last_menu;
    int menu_changed = menu_dirty || menu != last_menu;
    menu_dirty = 0;
    last_menu = menu;
    if (!screen_changed(flash) && !diag_on && !msg && !clear_msg && !label_changed && !confirm_changed &&
        !menu_changed) return;
    uint64_t t0 = plat_now_us();
    zx_render(frame_pixels, ZX_SCREEN_W, flash);
    if (diag_on) draw_diag(frame_pixels, ZX_SCREEN_W, 16, 16);
    if (menu) menu_draw(frame_pixels);
    if (confirm_quit == 1) {
        static const char *const box[3] = {
            "                        ",
            " END GAME?  A=YES  B=NO ",
            "                        ",
        };
        for (int i = 0; i < 3; i++)
            draw_text(frame_pixels, ZX_SCREEN_W, (ZX_SCREEN_W - 8 * 24) / 2, ZX_SCREEN_H / 2 - 12 + i * 8,
                      box[i], 0xFFFFFFFF, 0xFFA00000);
    }
    if (mode_label) {
        char label[48];
        snprintf(label, sizeof label, " %s ", plat_video_mode_name());
        int x = (ZX_SCREEN_W - 8 * (int)strlen(label)) / 2;
        draw_text(frame_pixels, ZX_SCREEN_W, x < 0 ? 0 : x, 8, label, 0xFFFFFFFF, 0xFF0000A0);
    } else if (msg && !clear_msg) {
        const char *label = msg_text;
        int x = (ZX_SCREEN_W - 8 * (int)strlen(label)) / 2;
        draw_text(frame_pixels, ZX_SCREEN_W, x, 8, label, 0xFFFFFFFF, 0xFF0000A0);
    }
    plat_present(frame_pixels);
    diag_draw_us = (unsigned)((diag_draw_us * 9 + (unsigned)(plat_now_us() - t0)) / 10);
    if (pending_screen) {
        plat_log("JSW SCREEN %s\n", pending_screen);
        pending_screen = NULL;
        if (!ready_logged) { ready_logged = 1; plat_log("JSW READY\n"); }
    }
}

static void pace(void)
{
    if (audio_ok) {
        while (audio_queued_ms() > AUDIO_QUEUE_TARGET_MS)
            SDL_Delay(1);
        return;
    }
    uint64_t frame = (uint64_t)1000000 * ZX_T_PER_FRAME / ZX_CPU_HZ;
    uint64_t now = plat_now_us();
    if (!perf_next || now > perf_next + 5 * frame) perf_next = now;
    perf_next += frame;
    while (plat_now_us() < perf_next) {
        uint64_t left_ms = (perf_next - plat_now_us()) / 1000;
        if (left_ms > 1) SDL_Delay((Uint32)(left_ms - 1));
    }
}

/* ------------------------------------------------------------------------ */
/* Chooser                                                                  */
/* ------------------------------------------------------------------------ */
static void ui_print(int row, int col, const char *t, uint8_t attr)
{
    for (; *t && col < 32; t++, col++) {
        uint8_t ch = (uint8_t)*t;
        if (ch < 32 || ch > 127) ch = '?';
        for (int py = 0; py < 8; py++) {
            uint16_t addr = (uint16_t)(0x4000 | ((row & 0x18) << 8) | (py << 8) | ((row & 7) << 5) | col);
            mem[addr] = mem[ROM_FONT + (ch - 32) * 8 + py];
        }
        mem[0x5800 + row * 32 + col] = attr;
    }
}

static void chooser_draw(void)
{
    static const char *const items[2] = {
        "    JET SET WILLY  (1984)       ",
        "    JET SET WILLY 128  (HL9)    ",
    };
    ui_print(20, 0, "                                ", 0x00);
    for (int i = 0; i < 2; i++)
        ui_print(21 + i, 0, items[i], chooser_sel == i ? 0x30 : 0x07);
    ui_print(23, 0, " D-pad choose     A / Start play", 0x05);
}

static void chooser_frame(void)
{
    if (ui_up)   { chooser_sel = 0; ui_up = 0; }
    if (ui_down) { chooser_sel = 1; ui_down = 0; }
    chooser_draw();
    if (ui_select) {
        ui_select = 0;
        longjmp(chosen_jmp, 1 + chooser_sel);
    }
}

/* END GAME? asked: hold the game still (sound silent, picture kept) until
 * A (yes) or B / Back (no), then until those buttons are let go, so the
 * answer never reaches the game as a jump. */
static void confirm_loop(void)
{
    static const int16_t silence[2 * 960];
    plat_log("JSW SCREEN endgame?\n");
    while (confirm_quit == 1) { audio_submit(silence, 960); poll_input(); present(); pace(); }
    int yes = confirm_quit == 2;
    while (pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))) {
        audio_submit(silence, 960); poll_input(); present(); pace();
    }
    confirm_quit = 0;
    if (yes) quit_frames = 10;
    plat_log("JSW: end game? %s\n", yes ? "yes" : "no");
}

/* A menu is open: hold the game still as for END GAME?, then wait until the
 * buttons are let go so A never reaches the game as a jump. A room chosen
 * is reached through the game's own teleport (as typing 9 and the room
 * number does on a Spectrum), so Willy keeps his place on the screen. */
static void menu_loop(void)
{
    static const int16_t silence[2 * 960];
    while (menu) { audio_submit(silence, 960); poll_input(); present(); pace(); }
    while (pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))) {
        audio_submit(silence, 960); poll_input(); present(); pace();
    }
    menu_dirty = 1;
    if (menu_teleport >= 0 && mem[JSW_TELEPORT] == CHEAT_DONE) {
        plat_log("JSW: teleport to room %d\n", menu_teleport);
        teleport_target = menu_teleport;
        teleport_pending = 1;
        teleport_frames = 0;
    }
    menu_teleport = -1;
    plat_log("JSW SCREEN play%s\n", v128 ? "128" : "48");
}

static void frame_hook(void)
{
    render_audio();
    poll_input();
    if (in_chooser) chooser_frame();
    if (back_to_chooser) {
        back_to_chooser = 0;
        longjmp(chooser_jmp, 1);
    }
    if (confirm_quit) confirm_loop();
    if (menu) menu_loop();
    if (audio_ok && frame_count > 50 && audio_queued_ms() < AUDIO_QUEUE_LOW_MS)
        diag_skipped++;
    else
        present();
    pace();
    frame_count++;
}

/* ------------------------------------------------------------------------ */
/* Start-up                                                                 */
/* ------------------------------------------------------------------------ */
/* Data files are looked for next to default.xbe (D:\), then in
 * D:\datafiles\, where the build puts the repo's datafiles/ folder. */
static long load_data(const char *name, uint8_t *dst, long max)
{
    static const char *const sub[] = { "", "datafiles\\" };
    for (int i = 0; i < 2; i++) {
        char path[160];
        snprintf(path, sizeof path, "%s%s%s", plat_data_dir(), sub[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        long n = (long)fread(dst, 1, (size_t)max, f);
        fclose(f);
        plat_log("JSW: %s: %ld bytes\n", path, n);
        return n;
    }
    return -1;
}

static void fatal_screen(const char *line1, const char *line2)
{
    plat_log("JSW: %s %s\n", line1, line2);
    memset(mem + 0x4000, 0, 0x1800);
    memset(mem + 0x5800, 0x47, 0x300);
    zx_border = 2;
    ui_print(10, 0, line1, 0x47);
    ui_print(12, 0, line2, 0x47);
    screen_shown("error");
    for (int i = 0; i < 50 * 15; i++) {
        poll_input();
        present();
        SDL_Delay(20);
    }
    plat_exit();
}

static const char *const files48[] = {
    "JetSetWilly.tzx", "JetSetWilly.tap", "JetSetWilly.bin",
    "Jet Set Willy.tzx", "Jet Set Willy.tap",
};
static const char *const files128[] = {
    "JetSetWilly128.tzx", "JetSetWilly128.tap",
    "Jet Set Willy 128k v. 0.04 HL 9.tzx", "Jet Set Willy 128k v. 0.04 HL 9.tap",
};
static uint8_t image48[JSW_IMAGE_SIZE];
static jsw128_image_t image128;
static uint8_t tape[1 << 20];
static uint8_t rom[0x4000];
static int     have_rom;

/* 1 = loaded, 0 = no file, -1 = a file that isn't the right tape. */
static int load48(void)
{
    int found = 0;
    for (unsigned i = 0; i < sizeof files48 / sizeof files48[0]; i++) {
        long n = load_data(files48[i], tape, sizeof tape);
        if (n < 0) continue;
        found = 1;
        if (jsw_load(tape, n, image48) == 0) return 1;
    }
    return found ? -1 : 0;
}

static int load128(void)
{
    int found = 0;
    for (unsigned i = 0; i < sizeof files128 / sizeof files128[0]; i++) {
        long n = load_data(files128[i], tape, sizeof tape);
        if (n < 0) continue;
        found = 1;
        if (jsw128_load(tape, n, &image128) == 0) return 1;
    }
    return found ? -1 : 0;
}

static void run48(int original)
{
    for (;;) {
        jsw_prepare(image48, original ? JSW_OPT_CODECARD : JSW_OPT_FIXES);
        zx_install_font(have_rom ? rom : NULL);
        uint32_t seed = (uint32_t)SDL_GetPerformanceCounter();
        mem[23672] = (uint8_t)(seed ^ (seed >> 8) ^ (seed >> 16));   /* FRAMES: code-card square */
        zx_in_filter = teleport_in_filter;
        jsw_hook = game_hook;
        in_codes = 0;
        plat_log("JSW: Jet Set Willy (%s)\n", original ? "original 1984" : "bug fixes");
        jsw_run();          /* returns only when the game resets the machine */
    }
}

static void run128(void)
{
    jsw128_prepare(&image128);
    zx_install_font(have_rom ? rom : NULL);
    ay_init(&ay, AY_CLOCK_128K, AUDIO_RATE);
    frame_start_level = 0;
    zx_in_filter = teleport_in_filter;
    z80c_hook = game_hook;
    jsw_in_title = 0;
    plat_log("JSW: Jet Set Willy 128\n");
    jsw128_z80_run(JSW128_BEGIN);
    char where[40];
    snprintf(where, sizeof where, "Stopped at %04X (bank %d).", z80c_bad_addr, zx_bank_c000);
    fatal_screen(where, "Please report this.");
}

static void release_all_keys(void)
{
    memset(pad_keys, 0, sizeof pad_keys);
    zx_keys_clear();
    ui_up = ui_down = ui_select = 0;
}

static int run_chooser(void)
{
    int r = setjmp(chosen_jmp);
    if (r) {
        in_chooser = 0;
        release_all_keys();
        plat_log("JSW: chose %s\n", r == 2 ? "128" : "48K");
        return r;
    }
    release_all_keys();
    in_chooser = 1;
    v128 = 0;
    jsw_prepare(image48, JSW_OPT_FIXES);
    zx_install_font(have_rom ? rom : NULL);
    zx_in_filter = NULL;
    jsw_hook = NULL;
    screen_shown("chooser");
    jsw_run();
    in_chooser = 0;
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    plat_log("JSW: JetSetWillyX (RXDK) starting\n");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        plat_log("JSW: SDL_Init: %s\n", SDL_GetError());
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) plat_exit();
    }
    if (!plat_video_init(ZX_SCREEN_W, ZX_SCREEN_H)) plat_exit();
    aspect_load();
    if (wide_on) plat_set_wide(1);
    audio_open();
    {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    have_rom = load_data("48.rom", rom, sizeof rom) == (long)sizeof rom;
    zx_reset();
    memset(mem, 0, sizeof mem);
    zx_install_font(have_rom ? rom : NULL);
    zx_frame_hook = frame_hook;

    int ok48 = load48(), ok128 = load128();
    plat_log("JSW: Jet Set Willy %s, Jet Set Willy 128 %s, 48.rom %s\n",
             ok48 > 0 ? "found" : ok48 < 0 ? "WRONG TAPE" : "missing",
             ok128 > 0 ? "found" : ok128 < 0 ? "WRONG TAPE" : "missing",
             have_rom ? "found" : "missing (built-in font)");
    if (ok48 <= 0 && ok128 <= 0) {
        if (ok48 < 0 || ok128 < 0) fatal_screen("A data file isn't the right", "tape (see README).");
        fatal_screen("Put JetSetWilly.tzx and/or", "JetSetWilly128.tzx by the XBE.");
    }

    /* Hold X or Y at start-up for the original 1984 release (48K). */
    int original = 0;
    SDL_PumpEvents();
    if (pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH)))
        original = 1;

    volatile int choice = ok48 > 0 ? 1 : 2;
    chooser_available = ok48 > 0 && ok128 > 0;
    if (chooser_available) {
        if (setjmp(chooser_jmp)) {
            last_back_frame = 0;
            macro_step = -1;
            teleport_pending = teleport_armed = 0;
            in_codes = 0;
            jsw_in_title = 0;
            frame_start_level = 0;
            stats_t0_us = 0;
            plat_log("JSW: back to the chooser\n");
        }
        choice = run_chooser();
    }
    v128 = choice == 2;
    stats_t0_us = 0;
    if (v128) run128();
    else      run48(original);
    plat_exit();
    return 0;
}
