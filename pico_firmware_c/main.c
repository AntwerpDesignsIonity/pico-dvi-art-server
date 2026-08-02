/*
 * pico-dvi-art client - local-render C firmware for a Pico 2 W (RP2350) in a
 * Waveshare PICO-DVI-LCD carrier.
 *
 * This build turns the panel into an endless 8-bit arcade attract mode. It
 * cycles through 25 original retro-inspired "cartridges" with scene lengths
 * between 6 and 21 seconds: block stackers, platformers, run-and-gun stages,
 * chess boards, snakes, racers, space battles and dungeon crawls.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"

// Diagnostic build switch: skip the DVI overclock and scan-out entirely, to
// separate toolchain problems from panel or timing problems.
#ifndef DIAG_SKIP_OVERCLOCK
#define DIAG_SKIP_OVERCLOCK 0
#endif

// ---------------------------------------------------------------- display
// The panel is 1024x600 and scales whatever standard DVI mode we feed it. The
// framebuffer is always half the mode in each axis because libdvi pixel-doubles
// it (DVI_SYMBOLS_PER_WORD=2 horizontally, DVI_VERTICAL_REPEAT=2 vertically).
#ifndef DVI_MODE_800X480
#define DVI_MODE_800X480 0
#endif

#if DVI_MODE_800X480
#define FRAME_WIDTH   400
#define DVI_TIMING    dvi_timing_800x480p_60hz
#define MODE_LABEL    "800X480P60"
#else
#define FRAME_WIDTH   320
#define DVI_TIMING    dvi_timing_640x480p_60hz
#define MODE_LABEL    "640X480P60"
#endif
#define FRAME_HEIGHT        240
#define FRAME_PIXELS        (FRAME_WIDTH * FRAME_HEIGHT)
#define VREG_VSEL           VREG_VOLTAGE_1_20
#define WATCHDOG_TIMEOUT_MS 8000
#define FRAME_INTERVAL_MS   33
#define FPS_APPROX          30u
#define TITLE_CARD_FRAMES   54u

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

struct dvi_inst dvi0;

static char device_line[48] = "BOOTING";

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} DateTime;

static DateTime build_clock = {2026, 1, 1, 0, 0, 0};

static void draw_text(uint16_t *buf, int x, int y, const char *s, int scale, uint16_t rgb);

// Two framebuffers: core0 scans one out while rendering into the other.
static uint16_t framebuf[2][FRAME_PIXELS];
static volatile uint8_t front_idx = 0;
static volatile uint8_t scanout_idx = 0;

enum {
    MODE_BLOCKS = 0,
    MODE_PLATFORM = 1,
    MODE_SHOOTER = 2,
    MODE_CHESS = 3,
    MODE_SNAKE = 4,
    MODE_RACER = 5,
    MODE_SPACE = 6,
    MODE_DUNGEON = 7,
    MODE_FIGHTER = 8,
    MODE_SCORE = 9,
};

typedef struct {
    const char *title;
    uint8_t mode;
    uint8_t duration_s;
    uint8_t variant;
    uint16_t year;
} SceneDef;

static const SceneDef SCENES[] = {
    {"BRICKFALL",       MODE_BLOCKS,   12,  0, 1989},
    {"PLUMBER QUEST",   MODE_PLATFORM, 12,  1, 1990},
    {"JUNGLE BURST",    MODE_SHOOTER,   9,  2, 1988},
    {"BYTE CHESS",      MODE_CHESS,    14,  3, 1987},
    {"NEON SNAKE",      MODE_SNAKE,    11,  4, 1986},
    {"PIXEL RACER",     MODE_RACER,     8,  5, 1990},
    {"STAR LANCER",     MODE_SPACE,    13,  6, 1991},
    {"DUNGEON SHIFT",   MODE_DUNGEON,  15,  7, 1989},
    {"FINAL KOMBAT",    MODE_FIGHTER,  16,  8, 1992},
    {"MOON MINER",      MODE_BLOCKS,   14,  9, 1991},
    {"BLUE BLUR DASH",  MODE_PLATFORM, 10, 10, 1991},
    {"CASTLE CIRCUIT",  MODE_RACER,    14, 11, 1992},
    {"DINO PARK 16",    MODE_DUNGEON,  12, 12, 1993},
    {"GLITCH GARDEN",   MODE_SPACE,    14, 13, 1992},
    {"OMEGA GRID",      MODE_CHESS,    12, 14, 1988},
    {"RAGE STREETS",    MODE_FIGHTER,  14, 15, 1991},
    {"CRYSTAL DROP",    MODE_BLOCKS,   13, 16, 1990},
    {"NIGHT TACTICS",   MODE_CHESS,     8, 17, 1993},
    {"SOLAR SERPENT",   MODE_SNAKE,    14, 18, 1992},
    {"TEMPLE BURST",    MODE_SHOOTER,  13, 19, 1989},
    {"CLOUD RAIDER",    MODE_PLATFORM,  8, 20, 1990},
    {"ASTRO FORGE",     MODE_SPACE,    13, 21, 1993},
    {"SKULL MAZE",      MODE_DUNGEON,  14, 22, 1987},
    {"CIRCUIT SPRINT",  MODE_RACER,     7, 23, 1991},
    {"AI SCORECARD",    MODE_SCORE,    10, 24, 1992},
    // ---- class of 1991-1993 ----
    {"STREET BRAWL II", MODE_FIGHTER,  14, 25, 1992},
    {"KOMBAT KINGS",    MODE_FIGHTER,  12, 26, 1992},
    {"TURBO HEDGEROW",  MODE_PLATFORM, 11, 27, 1992},
    {"LEGEND QUEST",    MODE_DUNGEON,  14, 28, 1991},
    {"PUFFBALL DREAM",  MODE_PLATFORM,  8, 29, 1992},
    {"BUNKER STORM 3D", MODE_SHOOTER,  13, 30, 1992},
    {"DESERT EMPIRE II",MODE_CHESS,    12, 31, 1992},
    {"STAR COMMANDER",  MODE_SPACE,    10, 32, 1990},
    {"SUPER KART 92",   MODE_RACER,    14, 33, 1992},
    {"COBRA TRIAD III", MODE_SHOOTER,   9, 34, 1992},
    {"DOLPHIN ECHO",    MODE_SNAKE,    13, 35, 1992},
    {"LEMMING DROP",    MODE_BLOCKS,   11, 36, 1991},
    {"PALACE RUNNER",   MODE_PLATFORM, 10, 37, 1989},
    {"FANTASY SAGA V",  MODE_CHESS,    15, 38, 1992},
    {"SHADOW SHINOBI",  MODE_PLATFORM, 12, 39, 1993},
    {"TOAD RUMBLE",     MODE_FIGHTER,  12, 40, 1991},
    {"RAPTOR ESCAPE",   MODE_SHOOTER,  12, 41, 1993},
    {"HEDGE SPIN ZONE", MODE_PLATFORM, 10, 42, 1992},
    {"CAGE OF FURY",    MODE_FIGHTER,  13, 43, 1993},
    {"WING SQUADRON",   MODE_SPACE,    11, 44, 1993},
    {"ECCO DEPTHS",     MODE_SNAKE,    12, 45, 1992},
    {"AXE LEGENDS",     MODE_DUNGEON,  12, 46, 1989},
    {"ROAD CLASH 2",    MODE_RACER,    10, 47, 1992},
    {"GEM MACHINE",     MODE_BLOCKS,   12, 48, 1994},
    {"AI SCORECARD",    MODE_SCORE,    10, 49, 1993},
};

#define SCENE_COUNT ((int)(sizeof(SCENES) / sizeof(SCENES[0])))

// ---------------------------------------------------------------- helpers
static int iabs(int value) {
    return value < 0 ? -value : value;
}

static uint8_t clamp_u8(int value) {
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    return (uint8_t)value;
}

static int approx_dist(int dx, int dy) {
    dx = iabs(dx);
    dy = iabs(dy);
    return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}

static uint8_t wave8(uint16_t phase) {
    uint8_t p = (uint8_t)phase;
    uint8_t tri = p < 128 ? (uint8_t)(p << 1) : (uint8_t)((255 - p) << 1);
    return (uint8_t)(((uint16_t)tri * tri) >> 8);
}

static uint8_t hash8(uint16_t x, uint16_t y, uint16_t z) {
    uint32_t h = (uint32_t)x * 17u + (uint32_t)y * 131u + (uint32_t)z * 73u;
    h ^= h >> 7;
    h ^= h << 9;
    h ^= h >> 13;
    return (uint8_t)h;
}

static float chip_temperature(void) {
    adc_select_input(4);
    // 12-bit conversion over the 3.3V reference, per the RP2350 datasheet.
    const float conversion = 3.3f / (1 << 12);
    float volts = adc_read() * conversion;
    return 27.0f - (volts - 0.706f) / 0.001721f;
}

static void update_device_line(void) {
    snprintf(
        device_line,
        sizeof(device_line),
        "%s  MCU %.1FC",
        MODE_LABEL,
        (double)chip_temperature()
    );
}

static int parse_month(const char *mon) {
    if (mon[0] == 'J' && mon[1] == 'a') return 1;
    if (mon[0] == 'F') return 2;
    if (mon[0] == 'M' && mon[2] == 'r') return 3;
    if (mon[0] == 'A' && mon[1] == 'p') return 4;
    if (mon[0] == 'M' && mon[2] == 'y') return 5;
    if (mon[0] == 'J' && mon[2] == 'n') return 6;
    if (mon[0] == 'J' && mon[2] == 'l') return 7;
    if (mon[0] == 'A' && mon[1] == 'u') return 8;
    if (mon[0] == 'S') return 9;
    if (mon[0] == 'O') return 10;
    if (mon[0] == 'N') return 11;
    return 12;
}

static int parse_int2(const char *text) {
    return (text[0] - '0') * 10 + (text[1] - '0');
}

static int is_leap_year(int year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

static int days_in_month(int year, int month) {
    static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return DAYS[month - 1];
}

static void init_build_clock(void) {
    const char *date = __DATE__;
    const char *time = __TIME__;
    build_clock.month = parse_month(date);
    build_clock.day = (date[4] == ' ' ? 0 : (date[4] - '0')) * 10 + (date[5] - '0');
    build_clock.year =
        (date[7] - '0') * 1000 +
        (date[8] - '0') * 100 +
        (date[9] - '0') * 10 +
        (date[10] - '0');
    build_clock.hour = parse_int2(time);
    build_clock.minute = parse_int2(time + 3);
    build_clock.second = parse_int2(time + 6);
}

static DateTime current_clock(void) {
    DateTime now = build_clock;
    uint64_t carry = time_us_64() / 1000000u;

    now.second += (int)(carry % 60u);
    carry /= 60u;
    if (now.second >= 60) {
        now.minute += now.second / 60;
        now.second %= 60;
    }

    now.minute += (int)(carry % 60u);
    carry /= 60u;
    if (now.minute >= 60) {
        now.hour += now.minute / 60;
        now.minute %= 60;
    }

    now.hour += (int)(carry % 24u);
    carry /= 24u;
    if (now.hour >= 24) {
        carry += (uint64_t)(now.hour / 24);
        now.hour %= 24;
    }

    while (carry > 0u) {
        int dim = days_in_month(now.year, now.month);
        now.day += 1;
        if (now.day > dim) {
            now.day = 1;
            now.month += 1;
            if (now.month > 12) {
                now.month = 1;
                now.year += 1;
            }
        }
        carry -= 1u;
    }

    return now;
}

static void fill_rect(uint16_t *buf, int x, int y, int w, int h, uint16_t color) {
    int x0;
    int y0;
    int x1;
    int y1;
    if (w <= 0 || h <= 0) return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w;
    y1 = y + h;
    if (x1 > FRAME_WIDTH) x1 = FRAME_WIDTH;
    if (y1 > FRAME_HEIGHT) y1 = FRAME_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; ++py) {
        uint16_t *row = buf + py * FRAME_WIDTH;
        for (int px = x0; px < x1; ++px) {
            row[px] = color;
        }
    }
}

static void stroke_rect(uint16_t *buf, int x, int y, int w, int h, int t, uint16_t color) {
    fill_rect(buf, x, y, w, t, color);
    fill_rect(buf, x, y + h - t, w, t, color);
    fill_rect(buf, x, y, t, h, color);
    fill_rect(buf, x + w - t, y, t, h, color);
}

static void fill_stripes(uint16_t *buf, int y0, int y1, int stripe_h,
                         uint16_t a, uint16_t b) {
    for (int y = y0; y < y1; y += stripe_h) {
        fill_rect(buf, 0, y, FRAME_WIDTH, stripe_h, ((y / stripe_h) & 1) ? b : a);
    }
}

// ---------------------------------------------------------------- ai core
// Integer sine (-127..127) over a 256-step circle, quarter-table interpolated.
static int isin8(uint8_t a) {
    static const int8_t Q[17] = {
        0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 118, 122, 125, 127, 127
    };
    int idx = a & 63;
    int quad = a >> 6;
    int v;
    if (quad & 1) idx = 64 - idx;
    if (idx >= 64) {
        v = 127;
    } else {
        int base = Q[idx >> 2];
        int next = Q[(idx >> 2) + 1];
        v = base + ((next - base) * (idx & 3)) / 4;
    }
    return (quad & 2) ? -v : v;
}

static int icos8(uint8_t a) {
    return isin8((uint8_t)(a + 64u));
}

// Smoothstep easing: returns 0..256 for t in 0..total.
static int ease256(int t, int total) {
    int u;
    if (total <= 0 || t >= total) return 256;
    if (t <= 0) return 0;
    u = (t * 256) / total;
    return ((u * u >> 8) * (768 - 2 * u)) >> 8;
}

// Linear approach: move value toward target by at most step.
static int approach(int value, int target, int step) {
    if (value < target) {
        value += step;
        if (value > target) value = target;
    } else if (value > target) {
        value -= step;
        if (value < target) value = target;
    }
    return value;
}

// Particle engine (positions and velocities in 8.8 fixed point).
typedef struct {
    int x, y, vx, vy;
    int life;
    uint16_t color;
} Particle;

#define MAX_PARTICLES 96
static Particle particles[MAX_PARTICLES];

static void spawn_burst(int x, int y, int count, uint16_t color, uint16_t seed) {
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < count; ++i) {
        Particle *p = &particles[i];
        uint8_t angle;
        int speed;
        if (p->life > 0) continue;
        angle = hash8((uint16_t)i, seed, (uint16_t)(x + y));
        speed = 96 + (hash8(seed, (uint16_t)i, 77u) & 127);
        p->x = x << 8;
        p->y = y << 8;
        p->vx = (isin8(angle) * speed) >> 7;
        p->vy = (icos8(angle) * speed) >> 7;
        p->life = 14 + (hash8((uint16_t)i, 31u, seed) & 15);
        p->color = color;
        ++spawned;
    }
}

static void particles_step_draw(uint16_t *buf) {
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        Particle *p = &particles[i];
        int px, py, size;
        if (p->life <= 0) continue;
        p->x += p->vx;
        p->y += p->vy;
        p->vy += 10; // gentle gravity
        p->life -= 1;
        px = p->x >> 8;
        py = p->y >> 8;
        size = p->life > 8 ? 2 : 1;
        fill_rect(buf, px, py, size, size, p->life > 10 ? 0xFFFF : p->color);
    }
}

// ---------------------------------------------------------------- ai player
// One AI player competes across every game. run_seed comes from ADC noise at
// boot and is re-stirred on every scene entry, so no two runs ever match.
static uint16_t run_seed;

static const char *const MODE_NAMES[9] = {
    "BLOCKS", "PLATFORM", "SHOOTER", "CHESS", "SNAKE",
    "RACER", "SPACE", "DUNGEON", "FIGHTER",
};
static uint32_t ai_mode_score[9];

static void ai_award(uint8_t mode, uint32_t points) {
    if (mode < 9) ai_mode_score[mode] += points;
}

static uint32_t ai_total_score(void) {
    uint32_t total = 0;
    for (int i = 0; i < 9; ++i) total += ai_mode_score[i];
    return total;
}

static void draw_pyramid(uint16_t *buf, int cx, int base_y, int half_width, int height,
                         uint16_t color) {
    for (int row = 0; row < height; ++row) {
        int width = ((height - row) * half_width) / height;
        if (width < 1) width = 1;
        fill_rect(buf, cx - width, base_y - row, width * 2 + 1, 1, color);
    }
}

static void draw_orb(uint16_t *buf, int cx, int cy, int radius,
                     uint16_t outer, uint16_t inner, uint16_t shine) {
    for (int y = -radius; y <= radius; ++y) {
        int py = cy + y;
        if (py < 0 || py >= FRAME_HEIGHT) continue;
        for (int x = -radius; x <= radius; ++x) {
            int px = cx + x;
            int d;
            if (px < 0 || px >= FRAME_WIDTH) continue;
            d = approx_dist(x, y);
            if (d > radius) continue;
            if (d < (radius >> 1)) {
                buf[py * FRAME_WIDTH + px] = inner;
            } else if (((y + radius + (x >> 1)) & 7) == 0) {
                buf[py * FRAME_WIDTH + px] = shine;
            } else {
                buf[py * FRAME_WIDTH + px] = outer;
            }
        }
    }
}

static void draw_star(uint16_t *buf, int x, int y, int size, uint16_t color) {
    fill_rect(buf, x, y, size, size, color);
    if (size > 1) {
        fill_rect(buf, x - 1, y, size + 2, 1, color);
        fill_rect(buf, x, y - 1, 1, size + 2, color);
    }
}

static void draw_runner(uint16_t *buf, int x, int y, int s,
                        uint16_t body, uint16_t skin, uint16_t accent) {
    fill_rect(buf, x + 2 * s, y, 2 * s, 2 * s, skin);
    fill_rect(buf, x + 1 * s, y, 4 * s, 1 * s, accent);
    fill_rect(buf, x + 2 * s, y + 2 * s, 3 * s, 3 * s, body);
    fill_rect(buf, x + 1 * s, y + 2 * s, 1 * s, 2 * s, accent);
    fill_rect(buf, x + 5 * s, y + 3 * s, 2 * s, 1 * s, accent);
    fill_rect(buf, x + 2 * s, y + 5 * s, 1 * s, 3 * s, body);
    fill_rect(buf, x + 4 * s, y + 5 * s, 1 * s, 3 * s, body);
}

static void draw_soldier(uint16_t *buf, int x, int y, int s,
                         uint16_t suit, uint16_t skin, uint16_t accent) {
    fill_rect(buf, x + 2 * s, y, 2 * s, 2 * s, skin);
    fill_rect(buf, x + 1 * s, y, 4 * s, 1 * s, accent);
    fill_rect(buf, x + 2 * s, y + 2 * s, 4 * s, 3 * s, suit);
    fill_rect(buf, x + 5 * s, y + 3 * s, 3 * s, 1 * s, accent);
    fill_rect(buf, x + 1 * s, y + 3 * s, 1 * s, 1 * s, accent);
    fill_rect(buf, x + 2 * s, y + 5 * s, 1 * s, 3 * s, suit);
    fill_rect(buf, x + 4 * s, y + 5 * s, 1 * s, 3 * s, suit);
}

static void draw_car(uint16_t *buf, int x, int y, int s,
                     uint16_t body, uint16_t glass, uint16_t wheel) {
    fill_rect(buf, x + 1 * s, y + 3 * s, 6 * s, 3 * s, body);
    fill_rect(buf, x + 2 * s, y + 1 * s, 4 * s, 3 * s, body);
    fill_rect(buf, x + 2 * s, y + 2 * s, 4 * s, 1 * s, glass);
    fill_rect(buf, x + 1 * s, y + 6 * s, 2 * s, 2 * s, wheel);
    fill_rect(buf, x + 5 * s, y + 6 * s, 2 * s, 2 * s, wheel);
}

static void draw_ship(uint16_t *buf, int x, int y, int s,
                      uint16_t hull, uint16_t canopy, uint16_t thrust) {
    fill_rect(buf, x + 1 * s, y + 2 * s, 5 * s, 2 * s, hull);
    fill_rect(buf, x + 2 * s, y + 1 * s, 2 * s, 1 * s, canopy);
    fill_rect(buf, x + 6 * s, y + 3 * s, 2 * s, 1 * s, hull);
    fill_rect(buf, x, y + 3 * s, 1 * s, 1 * s, thrust);
    fill_rect(buf, x + 1 * s, y + 4 * s, 1 * s, 1 * s, thrust);
    fill_rect(buf, x + 1 * s, y + 1 * s, 1 * s, 1 * s, hull);
    fill_rect(buf, x + 1 * s, y + 4 * s, 1 * s, 1 * s, hull);
}

static void draw_drone(uint16_t *buf, int x, int y, int s,
                       uint16_t body, uint16_t eye) {
    fill_rect(buf, x + 1 * s, y + 1 * s, 4 * s, 3 * s, body);
    fill_rect(buf, x, y + 2 * s, 1 * s, 1 * s, body);
    fill_rect(buf, x + 5 * s, y + 2 * s, 1 * s, 1 * s, body);
    fill_rect(buf, x + 2 * s, y + 2 * s, 1 * s, 1 * s, eye);
    fill_rect(buf, x + 3 * s, y + 2 * s, 1 * s, 1 * s, eye);
}

static void draw_apple(uint16_t *buf, int x, int y, int s,
                       uint16_t fruit, uint16_t leaf) {
    fill_rect(buf, x + s, y + s, 3 * s, 3 * s, fruit);
    fill_rect(buf, x + 2 * s, y, 1 * s, 1 * s, leaf);
    fill_rect(buf, x + 3 * s, y + s, 1 * s, 1 * s, leaf);
}

static void draw_chest(uint16_t *buf, int x, int y, int s,
                       uint16_t wood, uint16_t metal) {
    fill_rect(buf, x, y + s, 6 * s, 3 * s, wood);
    fill_rect(buf, x + s, y, 4 * s, s, metal);
    fill_rect(buf, x + 2 * s, y + s, 2 * s, 2 * s, metal);
}

static void draw_torch(uint16_t *buf, int x, int y, int s,
                       uint16_t stick, uint16_t ember, uint16_t flame) {
    fill_rect(buf, x + s, y + 2 * s, s, 4 * s, stick);
    fill_rect(buf, x, y + s, 3 * s, s, ember);
    fill_rect(buf, x + s, y, s, s, flame);
}

static void draw_ui_bar(uint16_t *buf, int scene_index, unsigned seconds_left) {
    char info[20];
    fill_rect(buf, 0, 0, FRAME_WIDTH, 16, 0x0000);
    stroke_rect(buf, 0, 0, FRAME_WIDTH, 16, 1, RGB565(48, 82, 120));
    draw_text(buf, 6, 4, "8 BIT INFINITE ARCADE", 1, RGB565(220, 240, 255));
    snprintf(
        info,
        sizeof(info),
        "%02u/%02u %02uS",
        (unsigned)(scene_index + 1),
        (unsigned)SCENE_COUNT,
        seconds_left
    );
    draw_text(
        buf,
        FRAME_WIDTH - 6 - (int)strlen(info) * 6,
        4,
        info,
        1,
        RGB565(255, 214, 90)
    );
}

static void draw_clock_box(uint16_t *buf) {
    DateTime now = current_clock();
    char time_text[12];
    char date_text[16];
    int x = FRAME_WIDTH - 76;
    int y = 20;

    snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d", now.hour, now.minute, now.second);
    snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d", now.year, now.month, now.day);

    fill_rect(buf, x, y, 68, 22, 0x0000);
    stroke_rect(buf, x, y, 68, 22, 1, RGB565(84, 150, 228));
    draw_text(buf, x + 8, y + 3, time_text, 1, 0xFFFF);
    draw_text(buf, x + 4, y + 13, date_text, 1, RGB565(255, 214, 90));
}

// ---------------------------------------------------------------- daily verse
// KJV (public domain). One verse per calendar day, rotating through the list.
static const char *const VERSES[] = {
    "THE LORD IS MY SHEPHERD; I SHALL NOT WANT. - PSALM 23:1",
    "I CAN DO ALL THINGS THROUGH CHRIST WHICH STRENGTHENETH ME. - PHILIPPIANS 4:13",
    "FOR GOD SO LOVED THE WORLD, THAT HE GAVE HIS ONLY BEGOTTEN SON. - JOHN 3:16",
    "TRUST IN THE LORD WITH ALL THINE HEART; AND LEAN NOT UNTO THINE OWN UNDERSTANDING. - PROVERBS 3:5",
    "BE STRONG AND OF A GOOD COURAGE; BE NOT AFRAID. - JOSHUA 1:9",
    "THE LORD IS MY LIGHT AND MY SALVATION; WHOM SHALL I FEAR? - PSALM 27:1",
    "IN THE BEGINNING GOD CREATED THE HEAVEN AND THE EARTH. - GENESIS 1:1",
    "CAST THY BURDEN UPON THE LORD, AND HE SHALL SUSTAIN THEE. - PSALM 55:22",
    "THY WORD IS A LAMP UNTO MY FEET, AND A LIGHT UNTO MY PATH. - PSALM 119:105",
    "AND WE KNOW THAT ALL THINGS WORK TOGETHER FOR GOOD TO THEM THAT LOVE GOD. - ROMANS 8:28",
    "BE STILL, AND KNOW THAT I AM GOD. - PSALM 46:10",
    "THE JOY OF THE LORD IS YOUR STRENGTH. - NEHEMIAH 8:10",
    "COME UNTO ME, ALL YE THAT LABOUR AND ARE HEAVY LADEN, AND I WILL GIVE YOU REST. - MATTHEW 11:28",
    "BUT THEY THAT WAIT UPON THE LORD SHALL RENEW THEIR STRENGTH. - ISAIAH 40:31",
    "GOD IS OUR REFUGE AND STRENGTH, A VERY PRESENT HELP IN TROUBLE. - PSALM 46:1",
    "LET YOUR LIGHT SO SHINE BEFORE MEN. - MATTHEW 5:16",
    "THIS IS THE DAY WHICH THE LORD HATH MADE; WE WILL REJOICE AND BE GLAD IN IT. - PSALM 118:24",
    "FOR WHERE TWO OR THREE ARE GATHERED TOGETHER IN MY NAME, THERE AM I. - MATTHEW 18:20",
    "A SOFT ANSWER TURNETH AWAY WRATH. - PROVERBS 15:1",
    "GREATER LOVE HATH NO MAN THAN THIS, THAT A MAN LAY DOWN HIS LIFE FOR HIS FRIENDS. - JOHN 15:13",
    "THE FEAR OF THE LORD IS THE BEGINNING OF WISDOM. - PROVERBS 9:10",
    "CREATE IN ME A CLEAN HEART, O GOD; AND RENEW A RIGHT SPIRIT WITHIN ME. - PSALM 51:10",
    "I WILL LIFT UP MINE EYES UNTO THE HILLS, FROM WHENCE COMETH MY HELP. - PSALM 121:1",
    "LOVE THY NEIGHBOUR AS THYSELF. - MARK 12:31",
    "FOR BY GRACE ARE YE SAVED THROUGH FAITH. - EPHESIANS 2:8",
    "REJOICE IN THE LORD ALWAY: AND AGAIN I SAY, REJOICE. - PHILIPPIANS 4:4",
    "THE LORD BLESS THEE, AND KEEP THEE. - NUMBERS 6:24",
    "SEEK YE FIRST THE KINGDOM OF GOD, AND HIS RIGHTEOUSNESS. - MATTHEW 6:33",
    "IN MY FATHER'S HOUSE ARE MANY MANSIONS. - JOHN 14:2",
    "BLESSED ARE THE PEACEMAKERS: FOR THEY SHALL BE CALLED THE CHILDREN OF GOD. - MATTHEW 5:9",
    "I AM THE WAY, THE TRUTH, AND THE LIFE. - JOHN 14:6",
};

#define VERSE_COUNT ((int)(sizeof(VERSES) / sizeof(VERSES[0])))

static int day_of_year(const DateTime *dt) {
    int total = dt->day;
    for (int m = 1; m < dt->month; ++m) {
        total += days_in_month(dt->year, m);
    }
    return total;
}

// Bottom cross-screen ticker: one verse per day, scrolling right to left.
static void draw_verse_ticker(uint16_t *buf, uint32_t global_frame) {
    DateTime now = current_clock();
    int verse_index = (day_of_year(&now) + now.year) % VERSE_COUNT;
    const char *verse = VERSES[verse_index];
    int text_w = (int)strlen(verse) * 6;
    int span = text_w + FRAME_WIDTH;
    int scroll = (int)((global_frame * 2u) % (uint32_t)span);
    int x = FRAME_WIDTH - scroll;
    int y = FRAME_HEIGHT - 13;

    fill_rect(buf, 0, FRAME_HEIGHT - 16, FRAME_WIDTH, 16, 0x0000);
    fill_rect(buf, 0, FRAME_HEIGHT - 16, FRAME_WIDTH, 1, RGB565(120, 96, 32));
    draw_text(buf, x, y, verse, 1, RGB565(255, 222, 120));
    draw_text(buf, x + span, y, verse, 1, RGB565(255, 222, 120));
}

static void draw_scene_card(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    int text_w;
    int x;
    int y;
    char year_text[8];
    if (scene_frame >= TITLE_CARD_FRAMES) return;
    text_w = (int)strlen(scene->title) * 12;
    x = (FRAME_WIDTH - text_w) / 2;
    y = FRAME_HEIGHT - 44;
    fill_rect(buf, x - 10, y - 4, text_w + 20, 40, 0x0000);
    stroke_rect(buf, x - 10, y - 4, text_w + 20, 40, 1, RGB565(84, 150, 228));
    draw_text(buf, x, y, scene->title, 2, 0xFFFF);
    snprintf(year_text, sizeof(year_text), "(%u)", (unsigned)scene->year);
    draw_text(buf, (FRAME_WIDTH - (int)strlen(year_text) * 6) / 2, y + 18, year_text, 1,
              RGB565(255, 214, 90));
}

// Small IONITY wordmark, pinned bottom-left above the verse ticker.
static void draw_ionity_logo(uint16_t *buf) {
    const uint16_t blue = RGB565(36, 90, 200);
    int x = 4;
    int y = FRAME_HEIGHT - 30;
    fill_rect(buf, x, y, 48, 12, 0xFFFF);
    stroke_rect(buf, x, y, 48, 12, 1, blue);
    draw_text(buf, x + 4, y + 3, "IONITY", 1, blue);
}

static void draw_startup_note(uint16_t *buf, uint32_t frame) {
    if (frame >= TITLE_CARD_FRAMES) return;
    fill_rect(buf, 10, 18, 170, 16, 0x0000);
    stroke_rect(buf, 10, 18, 170, 16, 1, RGB565(48, 82, 120));
    draw_text(buf, 16, 22, device_line, 1, RGB565(196, 228, 255));
}

static unsigned art_break_secs(int scene_index);

static void scene_at(uint32_t global_frame, int *scene_index, uint32_t *scene_frame,
                     unsigned *seconds_left, int *in_break) {
    uint32_t total_frames = 0;
    uint32_t cycle;
    uint32_t cursor = 0;
    *in_break = 0;
    for (int i = 0; i < SCENE_COUNT; ++i) {
        total_frames += ((uint32_t)SCENES[i].duration_s + art_break_secs(i)) * FPS_APPROX;
    }
    cycle = global_frame % total_frames;
    for (int i = 0; i < SCENE_COUNT; ++i) {
        uint32_t len = (uint32_t)SCENES[i].duration_s * FPS_APPROX;
        uint32_t brk = art_break_secs(i) * FPS_APPROX;
        if (cycle < cursor + len) {
            *scene_index = i;
            *scene_frame = cycle - cursor;
            *seconds_left = (unsigned)SCENES[i].duration_s - (unsigned)((cycle - cursor) / FPS_APPROX);
            if (*seconds_left == 0) *seconds_left = 1;
            return;
        }
        cursor += len;
        if (cycle < cursor + brk) {
            *scene_index = i;
            *scene_frame = cycle - cursor;
            *seconds_left = (unsigned)art_break_secs(i) - (unsigned)((cycle - cursor) / FPS_APPROX);
            if (*seconds_left == 0) *seconds_left = 1;
            *in_break = 1;
            return;
        }
        cursor += brk;
    }
    *scene_index = 0;
    *scene_frame = 0;
    *seconds_left = SCENES[0].duration_s;
}

// ---------------------------------------------------------------- font
// 5x7 ASCII font, 0x20..0x5F (space through underscore). Five column bytes per
// glyph, bit 0 = top row.
static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, // sp !
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14}, // " #
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, // $ %
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, // & '
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, // ( )
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08}, // * +
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, // , -
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}, // . /
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, // 0 1
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, // 2 3
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, // 4 5
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, // 6 7
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, // 8 9
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00}, // : ;
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, // < =
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, // > ?
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, // @ A
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, // B C
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, // D E
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, // F G
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, // H I
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, // J K
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, // L M
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, // N O
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, // P Q
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, // R S
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, // T U
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, // V W
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, // X Y
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00}, // Z [
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, // \ ]
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, // ^ _
};

static void draw_char(uint16_t *buf, int x, int y, char c, int scale, uint16_t rgb) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 0x20 || c > 0x5F) c = '?';
    const uint8_t *glyph = FONT5X7[c - 0x20];
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (!(glyph[col] & (1u << row))) continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    int px = x + col * scale + dx;
                    int py = y + row * scale + dy;
                    if (px >= 0 && px < FRAME_WIDTH && py >= 0 && py < FRAME_HEIGHT) {
                        buf[py * FRAME_WIDTH + px] = rgb;
                    }
                }
            }
        }
    }
}

static void draw_text(uint16_t *buf, int x, int y, const char *s, int scale, uint16_t rgb) {
    for (; *s; ++s) {
        draw_char(buf, x, y, *s, scale, rgb);
        x += 6 * scale;
    }
}

// ---------------------------------------------------------------- scenes
static int blk_fits(const uint16_t *well, const int shape[4][2], int px, int py) {
    for (int i = 0; i < 4; ++i) {
        int cx = px + shape[i][0];
        int cy = py + shape[i][1];
        if (cx < 0 || cx >= 10 || cy < 0 || cy >= 14) return 0;
        if (well[cy] & (1u << cx)) return 0;
    }
    return 1;
}

static void render_blocks(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t bg0[3] = {
        RGB565(18, 24, 68), RGB565(18, 46, 76), RGB565(54, 16, 28),
    };
    const uint16_t bg1[3] = {
        RGB565(10, 14, 42), RGB565(10, 30, 54), RGB565(26, 6, 16),
    };
    const uint16_t block_a[3] = {
        RGB565(42, 236, 240), RGB565(120, 220, 255), RGB565(255, 170, 74),
    };
    const uint16_t block_b[3] = {
        RGB565(255, 92, 180), RGB565(168, 120, 255), RGB565(255, 78, 108),
    };
    const uint16_t block_c[3] = {
        RGB565(255, 218, 68), RGB565(96, 255, 188), RGB565(255, 255, 120),
    };
    const uint16_t grid = RGB565(32, 44, 82);
    const uint16_t dark = RGB565(5, 8, 18);
    const int theme = scene->variant % 3;
    const int cell = 12;
    const int cols = 10;
    const int rows = 14;
    const int well_x = (FRAME_WIDTH - cols * cell) / 2;
    const int well_y = 34;
    static const int piece_shapes[4][4][2] = {
        {{0,0}, {1,0}, {2,0}, {1,1}},
        {{0,0}, {1,0}, {1,1}, {2,1}},
        {{0,0}, {0,1}, {1,1}, {2,1}},
        {{0,0}, {1,0}, {0,1}, {1,1}},
    };
    // The AI actually plays: it owns the well, evaluates every column for the
    // falling piece, steers it there, locks it in and clears real lines.
    static uint16_t well[14];
    static int pc_kind, pc_x, pc_y, pc_target, pc_alive;
    static int clear_row, clear_timer, blk_score, blk_lines;
    char blk_text[16];

    if (scene_frame == 0u) {
        for (int r = 0; r < rows; ++r) well[r] = 0;
        pc_alive = 0;
        clear_timer = 0;
        blk_score = 0;
        blk_lines = 0;
    }

    if (clear_timer > 0) {
        clear_timer -= 1;
        if (clear_timer == 0) {
            for (int r = clear_row; r > 0; --r) well[r] = well[r - 1];
            well[0] = 0;
        }
    } else {
        if (!pc_alive) {
            // Spawn and plan: pick the column with the lowest, cleanest landing.
            int best = -30000;
            pc_kind = (int)(hash8((uint16_t)scene_frame, (uint16_t)(scene->variant + run_seed), 6u) % 4u);
            pc_target = 3;
            for (int tx = 0; tx <= cols - 3; ++tx) {
                int ty = 0, landed, holes = 0, filled_lines = 0, score;
                if (!blk_fits(well, piece_shapes[pc_kind], tx, 0)) continue;
                while (blk_fits(well, piece_shapes[pc_kind], tx, ty + 1)) ++ty;
                landed = ty;
                for (int i = 0; i < 4; ++i) {
                    int cx = tx + piece_shapes[pc_kind][i][0];
                    int cy = landed + piece_shapes[pc_kind][i][1] + 1;
                    if (cy < rows && !(well[cy] & (1u << cx))) {
                        int covered = 1;
                        for (int j = 0; j < 4; ++j) {
                            if (tx + piece_shapes[pc_kind][j][0] == cx &&
                                landed + piece_shapes[pc_kind][j][1] == cy) covered = 0;
                        }
                        if (covered) ++holes;
                    }
                }
                for (int r = 0; r < rows; ++r) {
                    uint16_t merged = well[r];
                    for (int i = 0; i < 4; ++i) {
                        if (landed + piece_shapes[pc_kind][i][1] == r) {
                            merged |= (uint16_t)(1u << (tx + piece_shapes[pc_kind][i][0]));
                        }
                    }
                    if (merged == (uint16_t)((1u << cols) - 1u)) ++filled_lines;
                }
                score = landed * 6 - holes * 25 + filled_lines * 300;
                if (score > best) { best = score; pc_target = tx; }
            }
            pc_x = cols / 2 - 1;
            pc_y = 0;
            pc_alive = blk_fits(well, piece_shapes[pc_kind], pc_x, pc_y);
            if (!pc_alive) { // topped out: fresh game
                for (int r = 0; r < rows; ++r) well[r] = 0;
                pc_alive = 1;
            }
        }
        // Steer sideways toward the plan, then gravity.
        if ((scene_frame % 3u) == 0u && pc_x != pc_target) {
            int step = pc_target > pc_x ? 1 : -1;
            if (blk_fits(well, piece_shapes[pc_kind], pc_x + step, pc_y)) pc_x += step;
        }
        if ((scene_frame % (pc_x == pc_target ? 2u : 4u)) == 0u) {
            if (blk_fits(well, piece_shapes[pc_kind], pc_x, pc_y + 1)) {
                pc_y += 1;
            } else {
                for (int i = 0; i < 4; ++i) {
                    well[pc_y + piece_shapes[pc_kind][i][1]] |=
                        (uint16_t)(1u << (pc_x + piece_shapes[pc_kind][i][0]));
                }
                blk_score += 4;
                ai_award(MODE_BLOCKS, 4);
                pc_alive = 0;
                for (int r = 0; r < rows; ++r) {
                    if (well[r] == (uint16_t)((1u << cols) - 1u)) {
                        clear_row = r;
                        clear_timer = 12;
                        blk_score += 100;
                        blk_lines += 1;
                        ai_award(MODE_BLOCKS, 100);
                        spawn_burst(well_x + cols * cell / 2, well_y + r * cell + 6,
                                    20, block_c[theme], (uint16_t)scene_frame);
                        break;
                    }
                }
            }
        }
    }

    fill_stripes(buf, 0, FRAME_HEIGHT, 8, bg0[theme], bg1[theme]);
    fill_rect(buf, well_x - 20, well_y - 6, cols * cell + 40, rows * cell + 12, dark);
    stroke_rect(buf, well_x - 20, well_y - 6, cols * cell + 40, rows * cell + 12, 2, block_a[theme]);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            fill_rect(buf, well_x + c * cell, well_y + r * cell, cell - 1, cell - 1, grid);
            if (well[r] & (1u << c)) {
                int pick = (r + c + scene->variant) % 3;
                uint16_t color = pick == 0 ? block_a[theme] : (pick == 1 ? block_b[theme] : block_c[theme]);
                fill_rect(buf, well_x + c * cell + 1, well_y + r * cell + 1, cell - 3, cell - 3, color);
                fill_rect(buf, well_x + c * cell + 2, well_y + r * cell + 2, cell - 7, 2, RGB565(255, 255, 255));
            }
        }
    }

    if (clear_timer > 0 && ((clear_timer / 3) & 1) == 0) {
        fill_rect(buf, well_x + 1, well_y + clear_row * cell + 1, cols * cell - 2, cell - 3, RGB565(255, 255, 255));
    }

    if (pc_alive) {
        for (int i = 0; i < 4; ++i) {
            int px = pc_x + piece_shapes[pc_kind][i][0];
            int py = pc_y + piece_shapes[pc_kind][i][1];
            fill_rect(buf, well_x + px * cell + 1, well_y + py * cell + 1, cell - 3, cell - 3, block_b[theme]);
            fill_rect(buf, well_x + px * cell + 2, well_y + py * cell + 2, cell - 7, 2, RGB565(255, 255, 255));
        }
        // Ghost marker at the planned column so you can see it thinking.
        fill_rect(buf, well_x + pc_target * cell + 2, well_y + rows * cell + 2, cell * 3 - 6, 3, block_c[theme]);
    }

    snprintf(blk_text, sizeof(blk_text), "%06d", blk_score);
    draw_text(buf, 22, 58, "CPU", 1, block_c[theme]);
    draw_text(buf, 22, 74, "PLAYS", 1, block_a[theme]);
    draw_text(buf, 22, 86, blk_text, 1, 0xFFFF);
    snprintf(blk_text, sizeof(blk_text), "%d", blk_lines);
    draw_text(buf, FRAME_WIDTH - 62, 58, "LINES", 1, block_b[theme]);
    draw_text(buf, FRAME_WIDTH - 62, 74, blk_text, 2, block_c[theme]);
}

static void render_platform(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t sky_a[3] = {
        RGB565(82, 180, 255), RGB565(252, 148, 98), RGB565(46, 64, 128),
    };
    const uint16_t sky_b[3] = {
        RGB565(166, 228, 255), RGB565(255, 198, 124), RGB565(24, 30, 70),
    };
    const uint16_t ground[3] = {
        RGB565(74, 168, 84), RGB565(106, 74, 40), RGB565(58, 92, 146),
    };
    const uint16_t brick[3] = {
        RGB565(202, 96, 58), RGB565(164, 82, 62), RGB565(124, 146, 208),
    };
    const uint16_t coin[3] = {
        RGB565(255, 226, 64), RGB565(255, 238, 122), RGB565(250, 220, 160),
    };
    const int theme = scene->variant % 3;
    const int ground_y = 196;
    // Real physics hero: velocity + gravity, hops timed at obstacle marks.
    static int hero_px, hero_py, hero_vy, buddy_px;
    int hero_x, hero_y_pos, jump_h;

    if (scene_frame == 0u) {
        hero_px = 18 << 8;
        hero_py = 0;
        hero_vy = 0;
        buddy_px = (FRAME_WIDTH / 2) << 8;
    }

    hero_px += 2 << 8;
    if (hero_px > (FRAME_WIDTH - 34) << 8) hero_px = 18 << 8;
    hero_x = hero_px >> 8;

    // Jump when approaching a platform column or the flag block.
    if (hero_py == 0 && hero_vy == 0) {
        int ahead = (hero_x + 14) % 62;
        if (ahead < 6 || iabs(hero_x - 232) < 6) {
            hero_vy = -(5 << 8);
        }
    }
    hero_py += hero_vy;
    hero_vy += 90; // gravity
    if (hero_py > 0) {
        if (hero_vy > 300) {
            spawn_burst(hero_x + 6, ground_y - 4, 6, RGB565(220, 200, 160),
                        (uint16_t)scene_frame); // landing dust
            ai_award(MODE_PLATFORM, 2);
        }
        hero_py = 0;
        hero_vy = 0;
    }
    jump_h = -(hero_py >> 8);

    // Buddy AI: pursues the hero, easing harder the further behind it falls.
    buddy_px += (hero_px - (40 << 8) - buddy_px) >> 5;
    hero_y_pos = ground_y - 24 - jump_h;

    fill_rect(buf, 0, 0, FRAME_WIDTH, ground_y, sky_a[theme]);
    for (int y = 16; y < ground_y; y += 6) {
        fill_rect(buf, 0, y, FRAME_WIDTH, 3, sky_b[theme]);
    }
    draw_orb(buf, FRAME_WIDTH - 56, 52, theme == 2 ? 16 : 22,
             coin[theme], sky_b[theme], 0xFFFF);

    for (int i = 0; i < 4; ++i) {
        int cx = 24 + ((i * 84 + (int)(scene_frame * (i + 1u))) % (FRAME_WIDTH + 40)) - 20;
        fill_rect(buf, cx, 34 + i * 18, 18, 6, 0xFFFF);
        fill_rect(buf, cx + 6, 28 + i * 18, 18, 6, 0xFFFF);
    }

    draw_pyramid(buf, 42, ground_y - 8, 34, 38, RGB565(86, 136, 118));
    draw_pyramid(buf, 102, ground_y - 8, 42, 52, RGB565(64, 116, 102));
    draw_pyramid(buf, FRAME_WIDTH - 52, ground_y - 8, 48, 48, RGB565(84, 118, 160));

    fill_rect(buf, 0, ground_y, FRAME_WIDTH, FRAME_HEIGHT - ground_y, ground[theme]);
    for (int x = 0; x < FRAME_WIDTH; x += 16) {
        fill_rect(buf, x, ground_y, 14, 8, RGB565(82, 212, 98));
        fill_rect(buf, x, ground_y + 8, 14, 12, brick[theme]);
        fill_rect(buf, x, ground_y + 20, 14, 12, brick[theme]);
    }

    for (int i = 0; i < 4; ++i) {
        int px = 40 + i * 62 + ((scene->variant & 1) ? 8 : 0);
        int py = 140 - (i & 1) * 22;
        fill_rect(buf, px, py, 36, 10, brick[theme]);
        stroke_rect(buf, px, py, 36, 10, 1, RGB565(48, 32, 24));
        fill_rect(buf, px + 10, py - 18 - (int)(wave8((uint16_t)(scene_frame * 4u + i * 80u)) >> 6), 8, 8, coin[theme]);
    }

    fill_rect(buf, 246, ground_y - 30, 18, 30, RGB565(54, 128, 72));
    fill_rect(buf, 244, ground_y - 34, 22, 4, RGB565(92, 176, 104));
    draw_runner(buf, hero_x, hero_y_pos, 2, RGB565(230, 52, 48), RGB565(255, 220, 180), RGB565(255, 236, 64));
    draw_runner(buf, buddy_px >> 8, ground_y - 24 - (jump_h > 4 ? jump_h / 2 : 0), 2,
                RGB565(64, 96, 224), RGB565(255, 220, 180), RGB565(255, 236, 64));
}

static void render_shooter(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t sky_a[3] = {
        RGB565(36, 88, 56), RGB565(80, 106, 128), RGB565(72, 46, 84),
    };
    const uint16_t sky_b[3] = {
        RGB565(18, 52, 32), RGB565(40, 52, 86), RGB565(32, 18, 42),
    };
    const uint16_t foliage[3] = {
        RGB565(62, 148, 74), RGB565(112, 112, 124), RGB565(168, 124, 48),
    };
    const uint16_t enemy[3] = {
        RGB565(242, 70, 82), RGB565(238, 126, 68), RGB565(224, 72, 132),
    };
    const int theme = scene->variant % 3;
    const int ground_y = 184;
    // Drones dodge incoming rounds; when cornered they take the hit.
    static int drone_y[2], drone_ty[2], drone_x[2], drone_down[2];
    int shot_x = 74 + (int)((scene_frame * 7u) % 220u);
    int shot_y = ground_y - 14;

    if (scene_frame == 0u) {
        drone_x[0] = 236; drone_y[0] = 94 << 8;  drone_ty[0] = 94;
        drone_x[1] = 214; drone_y[1] = 142 << 8; drone_ty[1] = 142;
        drone_down[0] = drone_down[1] = 0;
    }

    for (int i = 0; i < 2; ++i) {
        int dy;
        if (drone_down[i] > 0) {
            drone_down[i] -= 1;
            if (drone_down[i] == 0) {
                drone_x[i] = FRAME_WIDTH - 30 - i * 26;
                drone_ty[i] = 80 + (int)hash8((uint16_t)i, (uint16_t)scene_frame, 9u) % 80;
                drone_y[i] = drone_ty[i] << 8;
            }
            continue;
        }
        drone_x[i] -= (i == 0) ? 1 : 0;
        if (drone_x[i] < 120) drone_x[i] = 236;
        // Threat reaction: when the shot closes in, pick an escape altitude.
        if (shot_x > drone_x[i] - 70 && shot_x < drone_x[i] &&
            iabs((drone_y[i] >> 8) - shot_y) < 30) {
            drone_ty[i] = (drone_y[i] >> 8) < shot_y ? 74 : 160;
        } else if ((scene_frame % 50u) == (uint32_t)(i * 25)) {
            drone_ty[i] = 84 + (int)hash8((uint16_t)(scene_frame + run_seed), (uint16_t)i, 4u) % 76;
        }
        dy = (drone_ty[i] << 8) - drone_y[i];
        drone_y[i] += dy >> 3; // eased evasive climb/dive
        // Hit check when it failed to dodge.
        if (iabs(shot_x - drone_x[i]) < 8 && iabs((drone_y[i] >> 8) - shot_y) < 10) {
            spawn_burst(drone_x[i] + 6, drone_y[i] >> 8, 16,
                        RGB565(255, 190, 60), (uint16_t)(scene_frame + i * 51u));
            drone_down[i] = 36;
            ai_award(MODE_SHOOTER, 15);
        }
    }

    fill_rect(buf, 0, 0, FRAME_WIDTH, ground_y, sky_a[theme]);
    for (int y = 20; y < ground_y; y += 8) {
        fill_rect(buf, 0, y, FRAME_WIDTH, 3, sky_b[theme]);
    }

    for (int i = 0; i < 6; ++i) {
        int tx = 12 + i * 52 + ((theme == 0) ? 0 : 6);
        draw_pyramid(buf, tx, ground_y - 12, 12 + (i & 1) * 4, 28 + (i % 3) * 6, foliage[theme]);
        fill_rect(buf, tx - 2, ground_y - 8, 4, 10, RGB565(86, 54, 32));
    }

    if (theme == 1) {
        for (int i = 0; i < 3; ++i) {
            int tower_x = 40 + i * 90;
            fill_rect(buf, tower_x, 72 + i * 10, 18, 108 - i * 10, RGB565(86, 92, 120));
            fill_rect(buf, tower_x - 4, 66 + i * 10, 26, 8, RGB565(126, 132, 164));
        }
    } else if (theme == 2) {
        for (int i = 0; i < 3; ++i) {
            int shrine_x = 44 + i * 88;
            draw_pyramid(buf, shrine_x + 18, 116 + i * 12, 18, 22, RGB565(146, 72, 34));
            fill_rect(buf, shrine_x, 116 + i * 12, 36, 56, RGB565(104, 48, 26));
        }
    }

    fill_rect(buf, 0, ground_y, FRAME_WIDTH, FRAME_HEIGHT - ground_y, RGB565(58, 46, 24));
    for (int x = 0; x < FRAME_WIDTH; x += 12) {
        fill_rect(buf, x, ground_y + (x & 8 ? 8 : 0), 10, 6, foliage[theme]);
    }

    draw_soldier(buf, 28, ground_y - 24, 2, RGB565(56, 196, 208), RGB565(255, 214, 176), RGB565(242, 236, 88));
    for (int i = 0; i < 2; ++i) {
        if (drone_down[i] > 0) continue;
        draw_drone(buf, drone_x[i], drone_y[i] >> 8, 2, enemy[theme],
                   i == 0 ? 0xFFFF : RGB565(255, 248, 160));
    }
    fill_rect(buf, shot_x, ground_y - 14, 14, 2, RGB565(255, 242, 120));
    fill_rect(buf, shot_x + 14, ground_y - 15, 4, 4, RGB565(255, 255, 255));
}

static void render_chess(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t bg_a[3] = {
        RGB565(20, 24, 38), RGB565(44, 16, 28), RGB565(10, 24, 34),
    };
    const uint16_t light_sq[3] = {
        RGB565(224, 214, 188), RGB565(232, 204, 176), RGB565(166, 214, 226),
    };
    const uint16_t dark_sq[3] = {
        RGB565(82, 62, 46), RGB565(66, 22, 34), RGB565(44, 92, 118),
    };
    const uint16_t highlight[3] = {
        RGB565(250, 208, 64), RGB565(255, 96, 140), RGB565(84, 230, 226),
    };
    const int theme = scene->variant % 3;
    const int cell = 20;
    const int board_x = (FRAME_WIDTH - 8 * cell) / 2;
    const int board_y = 42;
    const int move_paths[3][4][4] = {
        {{1,7,2,5}, {6,0,5,2}, {3,6,4,4}, {1,0,2,2}},
        {{6,7,5,5}, {1,0,2,2}, {4,7,4,4}, {3,0,4,1}},
        {{2,7,3,5}, {5,0,4,2}, {0,7,1,5}, {7,0,6,2}},
    };
    const int *move = move_paths[theme][(scene_frame / 45u) % 4u];
    int t = (int)(scene_frame % 45u);
    // Smoothstep ease (slow-fast-slow) plus a parabolic knight hop.
    int e = ease256(t, 40);
    int moving_x = move[0] * cell + (((move[2] - move[0]) * cell * e) >> 8);
    int moving_y = move[1] * cell + (((move[3] - move[1]) * cell * e) >> 8);
    int hop = ((256 - iabs(2 * e - 256)) * 14) >> 8;
    moving_y -= hop;
    if (t == 41) {
        spawn_burst(board_x + move[2] * cell + cell / 2,
                    board_y + move[3] * cell + cell / 2,
                    10, highlight[theme], (uint16_t)scene_frame);
        ai_award(MODE_CHESS, 30);
    }

    fill_rect(buf, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, bg_a[theme]);
    for (int i = 0; i < 28; ++i) {
        int sx = (i * 37 + scene->variant * 17) % FRAME_WIDTH;
        int sy = (i * 19 + scene->variant * 11) % 38;
        draw_star(buf, sx, sy + 18, (i & 3) == 0 ? 2 : 1, highlight[theme]);
    }

    stroke_rect(buf, board_x - 4, board_y - 4, 8 * cell + 8, 8 * cell + 8, 2, highlight[theme]);
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            uint16_t color = ((r + c) & 1) ? dark_sq[theme] : light_sq[theme];
            fill_rect(buf, board_x + c * cell, board_y + r * cell, cell, cell, color);
        }
    }

    fill_rect(buf, board_x + move[0] * cell, board_y + move[1] * cell, cell, cell, highlight[theme]);
    fill_rect(buf, board_x + move[2] * cell, board_y + move[3] * cell, cell, cell, highlight[theme]);

    draw_text(buf, board_x + 8, board_y + 6, "R", 2, 0x0000);
    draw_text(buf, board_x + 46, board_y + 6, "Q", 2, 0x0000);
    draw_text(buf, board_x + 86, board_y + 6, "K", 2, 0x0000);
    draw_text(buf, board_x + 128, board_y + 6, "R", 2, 0x0000);
    draw_text(buf, board_x + 24, board_y + 126, "P", 2, 0xFFFF);
    draw_text(buf, board_x + 68, board_y + 126, "B", 2, 0xFFFF);
    draw_text(buf, board_x + 106, board_y + 126, "Q", 2, 0xFFFF);
    draw_text(buf, board_x + moving_x + 5, board_y + moving_y + 3, "N", 2, RGB565(255, 255, 255));

    fill_rect(buf, 18, 186, 110, 26, 0x0000);
    stroke_rect(buf, 18, 186, 110, 26, 1, highlight[theme]);
    draw_text(buf, 26, 194, ((theme == 1) ? "CHECK" : "TACTIC"), 1, 0xFFFF);
    draw_text(buf, FRAME_WIDTH - 92, 194, "TURN 08", 1, highlight[theme]);
}

#define SNK_COLS 24
#define SNK_ROWS 16
#define SNK_MAX  110
static uint8_t snk_x[SNK_MAX];
static uint8_t snk_y[SNK_MAX];
static int snk_len, snk_dir, snk_fruit_x, snk_fruit_y, snk_score;

static int snake_occupied(int x, int y, int skip_tail) {
    int limit = snk_len - (skip_tail ? 1 : 0);
    for (int i = 0; i < limit; ++i) {
        if (snk_x[i] == x && snk_y[i] == y) return 1;
    }
    return 0;
}

static void snake_place_fruit(uint16_t seed) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        int fx = hash8(seed, (uint16_t)attempt, 5u) % SNK_COLS;
        int fy = hash8((uint16_t)attempt, seed, 9u) % SNK_ROWS;
        if (!snake_occupied(fx, fy, 0)) {
            snk_fruit_x = fx;
            snk_fruit_y = fy;
            return;
        }
    }
    snk_fruit_x = 1;
    snk_fruit_y = 1;
}

static void snake_reset(uint16_t seed) {
    snk_len = 6;
    snk_dir = 0;
    snk_score = 0;
    for (int i = 0; i < snk_len; ++i) {
        snk_x[i] = (uint8_t)(SNK_COLS / 2 - i);
        snk_y[i] = (uint8_t)(SNK_ROWS / 2);
    }
    snake_place_fruit(seed);
}

// Greedy seek with survival lookahead: prefer moves toward the fruit, never
// reverse, never hit a wall or the body.
static void snake_think_step(uint16_t seed) {
    static const int DX[4] = {1, 0, -1, 0};
    static const int DY[4] = {0, 1, 0, -1};
    int head_x = snk_x[0];
    int head_y = snk_y[0];
    int best_dir = -1;
    int best_score = -10000;

    for (int d = 0; d < 4; ++d) {
        int nx, ny, score;
        if (d == ((snk_dir + 2) & 3)) continue; // no reversing
        nx = head_x + DX[d];
        ny = head_y + DY[d];
        if (nx < 0 || ny < 0 || nx >= SNK_COLS || ny >= SNK_ROWS) continue;
        if (snake_occupied(nx, ny, 1)) continue;
        score = 200 - iabs(nx - snk_fruit_x) * 4 - iabs(ny - snk_fruit_y) * 4;
        // Open-space bonus: count free neighbours so it avoids dead ends.
        for (int e = 0; e < 4; ++e) {
            int ex = nx + DX[e];
            int ey = ny + DY[e];
            if (ex >= 0 && ey >= 0 && ex < SNK_COLS && ey < SNK_ROWS &&
                !snake_occupied(ex, ey, 1)) {
                score += 3;
            }
        }
        if (d == snk_dir) score += 2; // mild momentum
        if (score > best_score) {
            best_score = score;
            best_dir = d;
        }
    }

    if (best_dir < 0) {
        snake_reset(seed); // boxed in: new round
        return;
    }

    snk_dir = best_dir;
    for (int i = snk_len - 1; i > 0; --i) {
        snk_x[i] = snk_x[i - 1];
        snk_y[i] = snk_y[i - 1];
    }
    snk_x[0] = (uint8_t)(head_x + DX[best_dir]);
    snk_y[0] = (uint8_t)(head_y + DY[best_dir]);
}

static void render_snake(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t bg[3] = {
        RGB565(8, 10, 20), RGB565(18, 10, 10), RGB565(6, 18, 18),
    };
    const uint16_t grid[3] = {
        RGB565(28, 42, 74), RGB565(62, 26, 26), RGB565(18, 58, 58),
    };
    const uint16_t snake_a[3] = {
        RGB565(84, 255, 118), RGB565(255, 210, 88), RGB565(92, 255, 248),
    };
    const uint16_t snake_b[3] = {
        RGB565(36, 166, 78), RGB565(226, 116, 54), RGB565(24, 162, 164),
    };
    const uint16_t fruit[3] = {
        RGB565(255, 78, 118), RGB565(255, 82, 72), RGB565(255, 166, 84),
    };
    const int theme = scene->variant % 3;
    const int cell = 10;
    const int field_x = (FRAME_WIDTH - SNK_COLS * cell) / 2;
    const int field_y = 42;
    char score_text[16];

    if (scene_frame == 0u) {
        snake_reset((uint16_t)(scene->variant * 41u + 7u + run_seed));
    }

    if ((scene_frame % 3u) == 0u && scene_frame > 0u) {
        snake_think_step((uint16_t)(scene_frame + scene->variant));
        if (snk_x[0] == snk_fruit_x && snk_y[0] == snk_fruit_y) {
            snk_score += 10;
            ai_award(MODE_SNAKE, 10);
            spawn_burst(field_x + snk_fruit_x * cell + 5,
                        field_y + snk_fruit_y * cell + 5,
                        14, fruit[theme], (uint16_t)scene_frame);
            if (snk_len + 2 <= SNK_MAX) {
                for (int g = 0; g < 2; ++g) {
                    snk_x[snk_len] = snk_x[snk_len - 1];
                    snk_y[snk_len] = snk_y[snk_len - 1];
                    ++snk_len;
                }
            }
            snake_place_fruit((uint16_t)(scene_frame * 13u + 3u + run_seed));
        }
    }

    fill_rect(buf, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, bg[theme]);
    fill_rect(buf, field_x - 4, field_y - 4, SNK_COLS * cell + 8, SNK_ROWS * cell + 8, 0x0000);
    stroke_rect(buf, field_x - 4, field_y - 4, SNK_COLS * cell + 8, SNK_ROWS * cell + 8, 2, snake_a[theme]);

    for (int y = 0; y < SNK_ROWS; ++y) {
        for (int x = 0; x < SNK_COLS; ++x) {
            fill_rect(buf, field_x + x * cell, field_y + y * cell, cell - 1, cell - 1, grid[theme]);
        }
    }

    {
        // Pulsing fruit so the target reads as alive.
        int pulse = (int)(wave8((uint16_t)(scene_frame * 6u)) >> 6);
        draw_apple(buf,
                   field_x + snk_fruit_x * cell + 2 - pulse / 2,
                   field_y + snk_fruit_y * cell + 2 - pulse / 2,
                   2, fruit[theme], snake_a[theme]);
    }

    for (int i = snk_len - 1; i >= 0; --i) {
        uint16_t color = i == 0 ? snake_a[theme] : snake_b[theme];
        fill_rect(buf, field_x + snk_x[i] * cell + 1, field_y + snk_y[i] * cell + 1,
                  cell - 3, cell - 3, color);
        if (i == 0) {
            fill_rect(buf, field_x + snk_x[i] * cell + 6, field_y + snk_y[i] * cell + 3, 1, 1, 0x0000);
        }
    }

    snprintf(score_text, sizeof(score_text), "SCORE %03d", snk_score);
    draw_text(buf, 20, 202, "BYTE", 1, snake_b[theme]);
    draw_text(buf, 20, 214, "BRAIN", 1, snake_a[theme]);
    draw_text(buf, FRAME_WIDTH - 86, 208, score_text, 1, 0xFFFF);
}

static void render_racer(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t sky_a[3] = {
        RGB565(116, 84, 196), RGB565(36, 78, 122), RGB565(180, 220, 255),
    };
    const uint16_t sky_b[3] = {
        RGB565(255, 156, 64), RGB565(18, 20, 60), RGB565(102, 164, 220),
    };
    const uint16_t road[3] = {
        RGB565(52, 52, 68), RGB565(40, 40, 54), RGB565(90, 102, 126),
    };
    const uint16_t side[3] = {
        RGB565(22, 18, 28), RGB565(16, 18, 34), RGB565(164, 198, 230),
    };
    const uint16_t lane[3] = {
        RGB565(255, 248, 120), RGB565(118, 228, 255), RGB565(255, 255, 255),
    };
    const int theme = scene->variant % 3;
    const int horizon = 86;
    const int center = FRAME_WIDTH / 2 + (((int)wave8((uint16_t)(scene_frame * 2u + scene->variant * 20u)) - 128) >> 2);

    fill_rect(buf, 0, 0, FRAME_WIDTH, horizon, sky_a[theme]);
    for (int y = 16; y < horizon; y += 8) {
        fill_rect(buf, 0, y, FRAME_WIDTH, 4, sky_b[theme]);
    }
    draw_orb(buf, FRAME_WIDTH - 64, 54, 18, sky_b[theme], RGB565(255, 236, 120), 0xFFFF);

    for (int i = 0; i < 5; ++i) {
        int cx = 20 + i * 70;
        draw_pyramid(buf, cx, horizon, 24 + (i & 1) * 8, 30 + (i % 3) * 10, side[theme]);
    }

    fill_rect(buf, 0, horizon, FRAME_WIDTH, FRAME_HEIGHT - horizon, RGB565(12, 12, 16));
    for (int y = horizon; y < FRAME_HEIGHT; ++y) {
        int depth = y - horizon;
        int half = 24 + depth * 90 / (FRAME_HEIGHT - horizon);
        int x0 = center - half;
        int width = half * 2;
        fill_rect(buf, 0, y, x0, 1, side[theme]);
        fill_rect(buf, x0, y, width, 1, road[theme]);
        fill_rect(buf, x0 + width, y, FRAME_WIDTH - (x0 + width), 1, side[theme]);
        if (((depth + (int)(scene_frame * 4u)) % 26) < 12) {
            fill_rect(buf, center - 2, y, 4, 1, lane[theme]);
        }
    }

    for (int i = 0; i < 3; ++i) {
        // Rival AI: each car eases toward a lane it re-picks periodically.
        static int rival_lane[3];
        static int rival_pos[3];
        int ry, half, lane_target;
        if (scene_frame == 0u && i == 0) {
            for (int k = 0; k < 3; ++k) {
                rival_lane[k] = k - 1;
                rival_pos[k] = (k - 1) << 8;
            }
        }
        if ((scene_frame % 70u) == (uint32_t)(i * 23)) {
            rival_lane[i] = (int)(hash8((uint16_t)(scene_frame + run_seed), (uint16_t)i, 21u) % 3u) - 1;
            ai_award(MODE_RACER, 2);
        }
        lane_target = rival_lane[i] << 8;
        rival_pos[i] += (lane_target - rival_pos[i]) >> 4; // smooth lane change
        ry = horizon + 20 + (int)((scene_frame * 3u + i * 46u) % 108u);
        half = 24 + (ry - horizon) * 90 / (FRAME_HEIGHT - horizon);
        draw_car(buf, center + ((rival_pos[i] * half) >> 9) - 8, ry, 2,
                 RGB565(78, 232, 248), RGB565(220, 246, 255), 0x0000);
    }

    if (theme != 2) {
        fill_rect(buf, 24, 108, 6, 40, RGB565(74, 52, 28));
        fill_rect(buf, 16, 98, 22, 6, RGB565(54, 204, 94));
        fill_rect(buf, 32, 118, 6, 40, RGB565(74, 52, 28));
        fill_rect(buf, 24, 108, 22, 6, RGB565(54, 204, 94));
    }

    {
        // Player drifts: chassis lags the road curve then counter-steers in.
        static int player_px;
        if (scene_frame == 0u) player_px = (FRAME_WIDTH / 2) << 8;
        player_px += (((center - 12) << 8) - player_px) >> 3;
        if ((scene_frame % 4u) == 0u) {
            spawn_burst((player_px >> 8) + 6, 202, 1, RGB565(140, 140, 150), (uint16_t)scene_frame);
        }
        draw_car(buf, player_px >> 8, 184, 3, RGB565(255, 64, 72), RGB565(236, 250, 255), 0x0000);
    }
}

static void render_space(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t bg[3] = {
        RGB565(6, 8, 18), RGB565(18, 6, 20), RGB565(8, 18, 34),
    };
    const uint16_t star[3] = {
        RGB565(255, 255, 255), RGB565(255, 204, 160), RGB565(122, 244, 255),
    };
    const uint16_t orb_outer[3] = {
        RGB565(62, 114, 240), RGB565(244, 106, 76), RGB565(84, 230, 230),
    };
    const uint16_t orb_inner[3] = {
        RGB565(154, 216, 255), RGB565(255, 210, 102), RGB565(204, 255, 255),
    };
    const uint16_t ship_color[3] = {
        RGB565(84, 255, 224), RGB565(255, 170, 96), RGB565(255, 76, 178),
    };
    const int theme = scene->variant % 3;
    // Ace fighter flies evasive figure-eights; wasps fly true pursuit curves.
    static int ace_x = 26 << 8, ace_y = 90 << 8;
    static int wasp_x[3], wasp_y[3], wasp_vx[3], wasp_vy[3];
    static int wasp_down[3];
    int target_x = (60 + (icos8((uint8_t)(scene_frame * 2u)) >> 2)) << 8;
    int target_y = (110 + ((isin8((uint8_t)(scene_frame * 3u)) * 60) >> 7)) << 8;
    int laser_x = (ace_x >> 8) + 20 + (int)((scene_frame * 9u) % 200u);

    if (scene_frame == 0u) {
        ace_x = 26 << 8;
        ace_y = 90 << 8;
        for (int i = 0; i < 3; ++i) {
            wasp_x[i] = (250 + i * 20) << 8;
            wasp_y[i] = (50 + i * 60) << 8;
            wasp_vx[i] = wasp_vy[i] = 0;
            wasp_down[i] = 0;
        }
    }

    // Ace steering: chase the moving evasion point with smooth acceleration.
    ace_x += (target_x - ace_x) >> 4;
    ace_y += (target_y - ace_y) >> 4;

    for (int i = 0; i < 3; ++i) {
        if (wasp_down[i] > 0) {
            wasp_down[i] -= 1;
            if (wasp_down[i] == 0) {
                wasp_x[i] = (FRAME_WIDTH + 20) << 8;
                wasp_y[i] = (40 + (int)hash8((uint16_t)i, (uint16_t)(scene_frame + run_seed), 3u) % 140) << 8;
                wasp_vx[i] = wasp_vy[i] = 0;
            }
            continue;
        }
        // Pursuit: accelerate toward the ace, capped speed, per-wasp offset
        // so the flight paths braid instead of stacking.
        {
            int ox = (isin8((uint8_t)(scene_frame * 3u + i * 85u)) * 24) >> 7;
            int oy = (icos8((uint8_t)(scene_frame * 3u + i * 85u)) * 24) >> 7;
            int dx = (ace_x >> 8) + 30 + ox - (wasp_x[i] >> 8);
            int dy = (ace_y >> 8) + oy - (wasp_y[i] >> 8);
            wasp_vx[i] = approach(wasp_vx[i], dx * 12, 22);
            wasp_vy[i] = approach(wasp_vy[i], dy * 12, 22);
            if (wasp_vx[i] > 420) wasp_vx[i] = 420;
            if (wasp_vx[i] < -420) wasp_vx[i] = -420;
            if (wasp_vy[i] > 420) wasp_vy[i] = 420;
            if (wasp_vy[i] < -420) wasp_vy[i] = -420;
            wasp_x[i] += wasp_vx[i];
            wasp_y[i] += wasp_vy[i];
        }
        // Laser kill check: burst + respawn timer.
        if (iabs(laser_x - (wasp_x[i] >> 8)) < 10 &&
            iabs(((ace_y >> 8) + 10) - ((wasp_y[i] >> 8) + 6)) < 12) {
            spawn_burst(wasp_x[i] >> 8, wasp_y[i] >> 8, 18,
                        RGB565(255, 170, 64), (uint16_t)(scene_frame + i * 37u));
            wasp_down[i] = 40 + i * 12;
            ai_award(MODE_SPACE, 25);
        }
    }

    fill_rect(buf, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, bg[theme]);
    for (int i = 0; i < 72; ++i) {
        int sx = (i * 43 + scene->variant * 17) % FRAME_WIDTH;
        int sy = (i * 29 + scene->variant * 31 + (int)(scene_frame * (1u + (uint32_t)(i & 3)))) % FRAME_HEIGHT;
        draw_star(buf, sx, sy, (i & 7) == 0 ? 2 : 1, star[theme]);
    }

    draw_orb(buf, FRAME_WIDTH - 68, 58, 24, orb_outer[theme], orb_inner[theme], 0xFFFF);
    draw_ship(buf, ace_x >> 8, ace_y >> 8, 3, ship_color[theme], RGB565(255, 255, 255), RGB565(255, 210, 84));
    for (int i = 0; i < 3; ++i) {
        if (wasp_down[i] > 0) continue;
        draw_ship(buf, wasp_x[i] >> 8, wasp_y[i] >> 8, 2,
                  RGB565(255, 78, 126), RGB565(255, 255, 255), RGB565(255, 214, 64));
        // Engine trail follows the velocity vector.
        fill_rect(buf, (wasp_x[i] >> 8) - (wasp_vx[i] >> 6), (wasp_y[i] >> 8) + 5 - (wasp_vy[i] >> 6),
                  3, 2, RGB565(255, 150, 70));
    }
    fill_rect(buf, laser_x, (ace_y >> 8) + 10, 18, 2, RGB565(255, 255, 255));
    fill_rect(buf, laser_x + 18, (ace_y >> 8) + 9, 6, 4, ship_color[theme]);

    if (scene->variant == 24) {
        fill_rect(buf, 218, 122, 70, 46, RGB565(44, 30, 72));
        stroke_rect(buf, 218, 122, 70, 46, 2, RGB565(255, 92, 170));
        fill_rect(buf, 232, 136, 12, 8, RGB565(255, 236, 120));
        fill_rect(buf, 262, 136, 12, 8, RGB565(255, 236, 120));
        fill_rect(buf, 244, 152, 16, 6, RGB565(255, 92, 170));
    }
}

static void render_dungeon(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t wall[3] = {
        RGB565(44, 46, 62), RGB565(32, 56, 38), RGB565(54, 36, 42),
    };
    const uint16_t accent[3] = {
        RGB565(255, 112, 54), RGB565(108, 255, 142), RGB565(224, 96, 164),
    };
    const uint16_t floor[3] = {
        RGB565(26, 22, 30), RGB565(14, 26, 18), RGB565(22, 12, 18),
    };
    const uint16_t glow[3] = {
        RGB565(255, 206, 82), RGB565(166, 255, 186), RGB565(255, 194, 220),
    };
    const int theme = scene->variant % 3;
    const int floor_y = 188;

    fill_rect(buf, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, floor[theme]);
    for (int y = 18; y < floor_y; y += 14) {
        for (int x = 0; x < FRAME_WIDTH; x += 18) {
            fill_rect(buf, x, y, 16, 12, wall[theme]);
        }
    }

    fill_rect(buf, 0, floor_y, FRAME_WIDTH, FRAME_HEIGHT - floor_y, RGB565(10, 10, 12));
    fill_rect(buf, 102, floor_y, 116, 18, accent[theme]);
    fill_rect(buf, 108, floor_y + 4, 104, 10, glow[theme]);
    fill_rect(buf, 34, 92, 30, 96, wall[theme]);
    fill_rect(buf, FRAME_WIDTH - 64, 92, 30, 96, wall[theme]);
    draw_torch(buf, 40, 116, 3, RGB565(100, 70, 34), accent[theme], glow[theme]);
    draw_torch(buf, FRAME_WIDTH - 58, 116, 3, RGB565(100, 70, 34), accent[theme], glow[theme]);
    // Hero AI loot loop: run to the chest, grab, retreat; drone pursues.
    static int hero_hx, drone_hx, hero_goal, loot_count;
    if (scene_frame == 0u) {
        hero_hx = 64 << 8;
        drone_hx = 214 << 8;
        hero_goal = 1;
        loot_count = 0;
    }
    {
        int chest_x = FRAME_WIDTH - 92;
        int home_x = 64;
        int target = hero_goal ? chest_x : home_x;
        int hx, dx;
        // Flee override: if the drone is close, sprint away from it first.
        dx = (drone_hx >> 8) - (hero_hx >> 8);
        if (iabs(dx) < 34) {
            hero_hx += (dx > 0 ? -3 : 3) << 8;
        } else {
            hero_hx = approach(hero_hx, target << 8, 2 << 8);
        }
        if (hero_hx < 40 << 8) hero_hx = 40 << 8;
        if (hero_hx > (FRAME_WIDTH - 78) << 8) hero_hx = (FRAME_WIDTH - 78) << 8;
        hx = hero_hx >> 8;
        if (hero_goal && iabs(hx - (chest_x - 14)) < 6) {
            hero_goal = 0;
            loot_count += 1;
            ai_award(MODE_DUNGEON, 20);
            spawn_burst(chest_x + 8, 164, 12, glow[theme], (uint16_t)scene_frame);
        } else if (!hero_goal && iabs(hx - home_x) < 6) {
            hero_goal = 1;
        }
        // Drone pursuit with lag, hovering bob.
        drone_hx += ((hero_hx - drone_hx) >> 5);
        draw_runner(buf, hx, 150, 2, RGB565(72, 164, 255), RGB565(255, 220, 184), glow[theme]);
        draw_drone(buf, drone_hx >> 8, 138 + (isin8((uint8_t)(scene_frame * 5u)) >> 5), 3,
                   RGB565(188, 58, 66), glow[theme]);
        draw_chest(buf, chest_x, 158, 3, RGB565(122, 74, 28), glow[theme]);
        if (loot_count > 0) {
            char loot_text[12];
            snprintf(loot_text, sizeof(loot_text), "LOOT %02d", loot_count);
            draw_text(buf, 24, 202, loot_text, 1, glow[theme]);
        }
        // Torch embers drifting up.
        if ((scene_frame % 9u) == 0u) {
            spawn_burst(40 + (int)(hash8((uint16_t)scene_frame, 2u, 8u) & 3), 112, 1,
                        accent[theme], (uint16_t)scene_frame);
        }
    }
}

// Two AI fighters duel: they close distance, feint, strike, block and take
// knockback. Health bars, rounds and KO bursts. All original pixel art.
static void render_fighter(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    const uint16_t sky[3] = {
        RGB565(60, 24, 60), RGB565(24, 40, 78), RGB565(80, 44, 20),
    };
    const uint16_t floor_c[3] = {
        RGB565(120, 82, 44), RGB565(70, 74, 92), RGB565(140, 110, 60),
    };
    const uint16_t p1_c = RGB565(255, 196, 40);
    const uint16_t p2_c = RGB565(60, 220, 255);
    const int theme = scene->variant % 3;
    const int floor_y = 190;
    static int fx[2], fvx[2], fhp[2], fstate[2], ftimer[2], rounds_won[2], ko_timer;
    char hud[20];

    if (scene_frame == 0u) {
        fx[0] = 70 << 8;
        fx[1] = (FRAME_WIDTH - 90) << 8;
        fhp[0] = fhp[1] = 96;
        fstate[0] = fstate[1] = 0;
        ftimer[0] = ftimer[1] = 0;
        rounds_won[0] = rounds_won[1] = 0;
        ko_timer = 0;
        fvx[0] = fvx[1] = 0;
    }

    if (ko_timer > 0) {
        ko_timer -= 1;
        if (ko_timer == 0) {
            fx[0] = 70 << 8;
            fx[1] = (FRAME_WIDTH - 90) << 8;
            fhp[0] = fhp[1] = 96;
            fstate[0] = fstate[1] = 0;
        }
    } else {
        for (int i = 0; i < 2; ++i) {
            int other = 1 - i;
            int gap = (fx[other] >> 8) - (fx[i] >> 8);
            int dir = gap > 0 ? 1 : -1;
            uint8_t roll = hash8((uint16_t)(scene_frame + i * 97u), run_seed,
                                 (uint16_t)(fx[i] >> 10));
            if (ftimer[i] > 0) {
                ftimer[i] -= 1;
                if (fstate[i] == 1 && ftimer[i] == 4) {
                    // Strike lands unless the other fighter is blocking.
                    if (iabs(gap) < 34) {
                        if (fstate[other] == 2) {
                            spawn_burst((fx[other] >> 8) + 8, floor_y - 30, 4,
                                        RGB565(200, 200, 220), (uint16_t)(scene_frame + i));
                        } else {
                            fhp[other] -= 8 + (roll & 7);
                            fvx[other] = dir * (3 << 8);
                            spawn_burst((fx[other] >> 8) + 8, floor_y - 34, 10,
                                        RGB565(255, 90, 60), (uint16_t)(scene_frame + i));
                            ai_award(MODE_FIGHTER, 5);
                        }
                    }
                }
                if (ftimer[i] == 0) fstate[i] = 0;
            } else if (iabs(gap) > 36) {
                fx[i] += dir * (roll & 1 ? 2 : 1) << 8; // close in, uneven gait
                if ((roll & 15) == 0) fx[i] -= dir << 8; // feint step back
            } else {
                if (roll < 100) { fstate[i] = 1; ftimer[i] = 10; }       // strike
                else if (roll < 150) { fstate[i] = 2; ftimer[i] = 12; }  // block
                else if (roll < 180) { fx[i] -= dir * (2 << 8); }        // retreat
            }
            fx[i] += fvx[i];
            fvx[i] = (fvx[i] * 3) / 4; // knockback friction
            if (fx[i] < 20 << 8) fx[i] = 20 << 8;
            if (fx[i] > (FRAME_WIDTH - 44) << 8) fx[i] = (FRAME_WIDTH - 44) << 8;
        }
        for (int i = 0; i < 2; ++i) {
            if (fhp[i] <= 0) {
                rounds_won[1 - i] += 1;
                ko_timer = 50;
                spawn_burst((fx[i] >> 8) + 8, floor_y - 28, 24,
                            i == 0 ? p1_c : p2_c, (uint16_t)scene_frame);
                ai_award(MODE_FIGHTER, 50);
            }
        }
    }

    fill_rect(buf, 0, 0, FRAME_WIDTH, floor_y, sky[theme]);
    for (int y = 24; y < floor_y; y += 9) {
        fill_rect(buf, 0, y, FRAME_WIDTH, 3, RGB565(20, 12, 30));
    }
    // Crowd silhouettes.
    for (int i = 0; i < 20; ++i) {
        int cx = 8 + i * 16;
        int bob = (isin8((uint8_t)(scene_frame * 4u + i * 40u)) >> 6);
        fill_rect(buf, cx, 120 + bob, 8, 14, RGB565(16, 10, 24));
        fill_rect(buf, cx + 2, 114 + bob, 4, 5, RGB565(16, 10, 24));
    }
    fill_rect(buf, 0, floor_y, FRAME_WIDTH, FRAME_HEIGHT - floor_y, floor_c[theme]);
    fill_rect(buf, 0, floor_y, FRAME_WIDTH, 3, RGB565(255, 255, 255));

    for (int i = 0; i < 2; ++i) {
        int x = fx[i] >> 8;
        int dir = ((fx[1 - i] >> 8) > x) ? 1 : -1;
        uint16_t body = i == 0 ? p1_c : p2_c;
        int crouch = fstate[i] == 2 ? 4 : 0;
        if (ko_timer > 0 && fhp[i] <= 0) {
            fill_rect(buf, x - 6, floor_y - 10, 26, 8, body); // down for the count
            continue;
        }
        fill_rect(buf, x + 4, floor_y - 40 + crouch, 10, 10, RGB565(255, 214, 176));
        fill_rect(buf, x + 2, floor_y - 30 + crouch, 14, 16, body);
        fill_rect(buf, x + 2, floor_y - 14 + crouch, 5, 14 - crouch, body);
        fill_rect(buf, x + 11, floor_y - 14 + crouch, 5, 14 - crouch, body);
        if (fstate[i] == 1 && ftimer[i] > 3) {
            fill_rect(buf, x + (dir > 0 ? 16 : -12), floor_y - 28, 14, 5, body); // punch
        } else if (fstate[i] == 2) {
            fill_rect(buf, x + (dir > 0 ? 15 : -5), floor_y - 32, 4, 14, RGB565(220, 220, 240));
        }
    }

    // Health bars + rounds.
    stroke_rect(buf, 12, 22, 100, 10, 1, 0xFFFF);
    fill_rect(buf, 13, 23, fhp[0] > 0 ? fhp[0] : 0, 8, p1_c);
    stroke_rect(buf, FRAME_WIDTH - 112, 22, 100, 10, 1, 0xFFFF);
    fill_rect(buf, FRAME_WIDTH - 13 - (fhp[1] > 0 ? fhp[1] : 0), 23,
              fhp[1] > 0 ? fhp[1] : 0, 8, p2_c);
    snprintf(hud, sizeof(hud), "%d - %d", rounds_won[0], rounds_won[1]);
    draw_text(buf, (FRAME_WIDTH - (int)strlen(hud) * 6) / 2, 23, hud, 1, 0xFFFF);
    if (ko_timer > 20) {
        draw_text(buf, (FRAME_WIDTH - 2 * 12) / 2, 96, "KO", 4, RGB565(255, 60, 40));
    }
}

// The AI's automated scorecard: live totals per genre, champion callout.
static void render_scorecard(uint16_t *buf, const SceneDef *scene, uint32_t scene_frame) {
    char line[28];
    int best = 0;
    (void)scene;

    fill_rect(buf, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, RGB565(8, 10, 24));
    for (int i = 0; i < 40; ++i) {
        int sx = (i * 53 + 11) % FRAME_WIDTH;
        int sy = (i * 37 + (int)(scene_frame / 2u)) % FRAME_HEIGHT;
        fill_rect(buf, sx, sy, 1, 1, RGB565(60, 70, 110));
    }

    draw_text(buf, (FRAME_WIDTH - 12 * 12) / 2, 24, "AI SCORECARD", 2, RGB565(255, 214, 90));
    for (int i = 1; i < 9; ++i) {
        if (ai_mode_score[i] > ai_mode_score[best]) best = i;
    }
    for (int i = 0; i < 9; ++i) {
        int y = 52 + i * 14;
        int reveal = (int)(scene_frame / 6u);
        uint16_t color = (i == best && ai_mode_score[best] > 0)
                             ? RGB565(120, 255, 140) : RGB565(210, 220, 240);
        if (i > reveal) continue; // rows type in one by one
        snprintf(line, sizeof(line), "%-8s %6u", MODE_NAMES[i],
                 (unsigned)ai_mode_score[i]);
        draw_text(buf, 76, y, line, 1, color);
        if (i == best && ai_mode_score[best] > 0) {
            draw_text(buf, 186, y, "* CHAMPION", 1, RGB565(120, 255, 140));
        }
    }
    snprintf(line, sizeof(line), "TOTAL %u", (unsigned)ai_total_score());
    draw_text(buf, (FRAME_WIDTH - (int)strlen(line) * 12) / 2, 186, line, 2, 0xFFFF);
}

// Deterministic per-slot art break length: 3-6 seconds.
static unsigned art_break_secs(int scene_index) {
    return 3u + (hash8((uint16_t)(scene_index * 7), 91u, 13u) % 4u);
}

// Generative art break: seeded plasma field + procedurally grown symmetric
// pixel creatures. run_seed feeds everything, so no two breaks ever match.
static void render_art_break(uint16_t *buf, int scene_index, uint32_t art_frame) {
    uint16_t seed = (uint16_t)(run_seed ^ (uint16_t)(scene_index * 977));
    const SceneDef *next = &SCENES[(scene_index + 1) % SCENE_COUNT];
    uint16_t palette[4];
    int k1 = 2 + (hash8(seed, 1u, 1u) & 3);
    int k2 = 1 + (hash8(seed, 2u, 2u) & 3);
    int k3 = 1 + (hash8(seed, 3u, 3u) & 1);
    int f = (int)art_frame;
    char caption[32];

    for (int i = 0; i < 4; ++i) {
        palette[i] = RGB565(
            40 + hash8(seed, (uint16_t)i, 51u) % 200,
            40 + hash8(seed, (uint16_t)i, 87u) % 200,
            60 + hash8(seed, (uint16_t)i, 33u) % 190);
    }

    // Plasma interference field, computed on a 2x2 grid for speed.
    for (int y = 0; y < FRAME_HEIGHT; y += 2) {
        uint16_t *row0 = buf + y * FRAME_WIDTH;
        uint16_t *row1 = buf + (y + 1) * FRAME_WIDTH;
        for (int x = 0; x < FRAME_WIDTH; x += 2) {
            int v = wave8((uint16_t)(x * k1 + f * 3))
                  + wave8((uint16_t)(y * k2 - f * 2))
                  + wave8((uint16_t)((x + y) * k3 + f * 2));
            uint16_t c = palette[(v >> 6) & 3];
            if (((v >> 4) & 7) == 0) c = 0x0000; // contour gaps
            row0[x] = c;
            row0[x + 1] = c;
            row1[x] = c;
            row1[x + 1] = c;
        }
    }

    // Three generations of mirrored pixel creatures drift across the frame.
    for (int s = 0; s < 3; ++s) {
        uint16_t sprite_seed = (uint16_t)(seed + s * 313 + (uint16_t)(art_frame / 45u));
        int cell = 6 + (hash8(sprite_seed, 9u, 9u) & 7);
        int cols = 4, rows_n = 7;
        int cx = 60 + s * 100 + (isin8((uint8_t)(f * 2u + s * 85)) >> 3);
        int cy = 70 + (s & 1) * 60 + (icos8((uint8_t)(f * 3u + s * 40)) >> 4);
        uint16_t body = palette[(s + 1) & 3];
        for (int r = 0; r < rows_n; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (hash8(sprite_seed, (uint16_t)c, (uint16_t)r) < 120) continue;
                fill_rect(buf, cx + c * cell, cy + r * cell, cell - 1, cell - 1, body);
                fill_rect(buf, cx - (c + 1) * cell, cy + r * cell, cell - 1, cell - 1, body);
            }
        }
        // Blinking eye pair.
        if (((art_frame / 20u) & 3u) != 3u) {
            fill_rect(buf, cx - cell, cy + cell, cell / 2, cell / 2, 0xFFFF);
            fill_rect(buf, cx + cell / 2, cy + cell, cell / 2, cell / 2, 0xFFFF);
        }
    }

    fill_rect(buf, 0, 152, FRAME_WIDTH, 1, 0x0000);
    snprintf(caption, sizeof(caption), "NEXT: %s (%u)", next->title, (unsigned)next->year);
    fill_rect(buf, (FRAME_WIDTH - (int)strlen(caption) * 6) / 2 - 6, 162,
              (int)strlen(caption) * 6 + 12, 14, 0x0000);
    draw_text(buf, (FRAME_WIDTH - (int)strlen(caption) * 6) / 2, 165, caption, 1,
              RGB565(255, 214, 90));
    draw_text(buf, (FRAME_WIDTH - 11 * 6) / 2, 40, "DREAM MODE", 1, 0xFFFF);
}

static void render_arcade_scene(uint16_t *buf, uint32_t global_frame) {
    int scene_index = 0;
    uint32_t scene_frame = 0;
    unsigned seconds_left = 0;
    int in_break = 0;
    const SceneDef *scene;

    scene_at(global_frame, &scene_index, &scene_frame, &seconds_left, &in_break);
    scene = &SCENES[scene_index];

    // Re-stir the run seed on every scene entry so no playthrough repeats.
    {
        static int last_scene = -1;
        int slot = scene_index * 2 + in_break;
        if (slot != last_scene) {
            run_seed = (uint16_t)(run_seed * 31u + 17u + (uint16_t)(time_us_64() & 0xFFu));
            last_scene = slot;
        }
    }

    if (in_break) {
        render_art_break(buf, scene_index, scene_frame);
        particles_step_draw(buf);
        draw_ui_bar(buf, scene_index, seconds_left);
        draw_clock_box(buf);
        draw_verse_ticker(buf, global_frame);
        draw_ionity_logo(buf);
        return;
    }

    switch (scene->mode) {
    case MODE_BLOCKS:
        render_blocks(buf, scene, scene_frame);
        break;
    case MODE_PLATFORM:
        render_platform(buf, scene, scene_frame);
        break;
    case MODE_SHOOTER:
        render_shooter(buf, scene, scene_frame);
        break;
    case MODE_CHESS:
        render_chess(buf, scene, scene_frame);
        break;
    case MODE_SNAKE:
        render_snake(buf, scene, scene_frame);
        break;
    case MODE_RACER:
        render_racer(buf, scene, scene_frame);
        break;
    case MODE_SPACE:
        render_space(buf, scene, scene_frame);
        break;
    case MODE_FIGHTER:
        render_fighter(buf, scene, scene_frame);
        break;
    case MODE_SCORE:
        render_scorecard(buf, scene, scene_frame);
        break;
    default:
        render_dungeon(buf, scene, scene_frame);
        break;
    }

    particles_step_draw(buf);
    draw_ui_bar(buf, scene_index, seconds_left);
    draw_clock_box(buf);
    draw_verse_ticker(buf, global_frame);
    draw_ionity_logo(buf);
    draw_startup_note(buf, scene_frame);
    draw_scene_card(buf, scene, scene_frame);
}

// ---------------------------------------------------------------- cores
static void core1_scanline_callback(void) {
    const uint16_t *scanline_ptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline_ptr))
        ;

    // Lines 0 and 1 are queued before DVI starts. Latch the published buffer
    // only at a frame boundary so a core0 flip cannot tear a frame.
    static uint scanline = 2;
    if (scanline == 0) {
        scanout_idx = front_idx;
    }
    scanline_ptr = &framebuf[scanout_idx][scanline * FRAME_WIDTH];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline_ptr);
    scanline = (scanline + 1) % FRAME_HEIGHT;
}

static void core1_main(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

int main(void) {
    // DVI needs a high system clock, so set it before the panel starts.
#if !DIAG_SKIP_OVERCLOCK
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
#endif

    setup_default_uart();
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    init_build_clock();
    update_device_line();

    // Seed the AI player's RNG from ADC thermal noise + boot time so every
    // power-on plays differently.
    {
        uint16_t noise = 0;
        adc_select_input(4);
        for (int i = 0; i < 16; ++i) {
            noise = (uint16_t)((noise << 1) ^ (adc_read() & 1u) ^ (noise >> 15));
        }
        run_seed = (uint16_t)(noise ^ (uint16_t)time_us_64() ^ 0x5A17u);
    }

    printf("\n[boot] pico-dvi 8-bit infinite arcade\n");
    printf("[boot] %dx%d RGB565 in %s DVI, sys clock %lu kHz\n",
           FRAME_WIDTH, FRAME_HEIGHT, MODE_LABEL,
           (unsigned long)(clock_get_hz(clk_sys) / 1000));
    printf("[boot] %d rotating original retro scenes, run seed %u\n",
           SCENE_COUNT, (unsigned)run_seed);

    memset(framebuf, 0, sizeof(framebuf));
    render_arcade_scene(framebuf[0], 0);

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
#ifdef DVI_INVERT_DIFFPAIRS_OVERRIDE
    // A solid single-colour "no signal" screen on the panel (instead of the
    // expected checkered standby pattern) means the sink's TMDS clock/data
    // recovery never locked - the classic cause is the diff-pair polarity not
    // matching this carrier's wiring.
    dvi0.ser_cfg.invert_diffpairs = DVI_INVERT_DIFFPAIRS_OVERRIDE > 0;
#endif
#if !DIAG_SKIP_OVERCLOCK
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    const uint16_t *scanline_ptr = &framebuf[0][0];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline_ptr);
    scanline_ptr = &framebuf[0][FRAME_WIDTH];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline_ptr);
    multicore_launch_core1(core1_main);
#endif

    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    absolute_time_t next_render = get_absolute_time();
    absolute_time_t next_status = make_timeout_time_ms(2000);
    uint32_t frame = 0;

    while (true) {
        watchdog_update();

        if (absolute_time_diff_us(get_absolute_time(), next_render) <= 0) {
            uint8_t render_idx = front_idx ^ 1;
            if (render_idx != scanout_idx) {
                render_arcade_scene(framebuf[render_idx], frame++);
                __dmb();
                front_idx = render_idx;
                next_render = delayed_by_ms(next_render, FRAME_INTERVAL_MS);
            }
        }

        if (absolute_time_diff_us(get_absolute_time(), next_status) <= 0) {
            update_device_line();
            next_status = make_timeout_time_ms(2000);
        }

        sleep_ms(1);
    }
}
