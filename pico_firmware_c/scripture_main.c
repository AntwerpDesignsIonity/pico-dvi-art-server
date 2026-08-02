/*
 * pico-dvi scripture edition - local-render C firmware for a Pico 2 W
 * (RP2350) in a Waveshare PICO-DVI-LCD carrier.
 *
 * Version 2: an ambient scripture display. 1024 public-domain King James
 * verses live in flash and rotate at randomized intervals with a typewriter
 * reveal, over a slow aurora background. Fixed UI: date, time, MCU
 * temperature, verse counter, progress bar and the IONITY wordmark.
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

#include "verses.h"

#ifndef DIAG_SKIP_OVERCLOCK
#define DIAG_SKIP_OVERCLOCK 0
#endif

// ---------------------------------------------------------------- display
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

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

struct dvi_inst dvi0;

static uint16_t framebuf[2][FRAME_PIXELS];
static volatile uint8_t front_idx = 0;
static volatile uint8_t scanout_idx = 0;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} DateTime;

static DateTime build_clock = {2026, 1, 1, 0, 0, 0};
static uint16_t run_seed;

// ---------------------------------------------------------------- helpers
static int iabs(int value) {
    return value < 0 ? -value : value;
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

static void fill_rect(uint16_t *buf, int x, int y, int w, int h, uint16_t color) {
    int x0, y0, x1, y1;
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

// ---------------------------------------------------------------- clock
static float chip_temperature(void) {
    adc_select_input(4);
    const float conversion = 3.3f / (1 << 12);
    float volts = adc_read() * conversion;
    return 27.0f - (volts - 0.706f) / 0.001721f;
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

// ---------------------------------------------------------------- verse flow
// Verses appear in a randomized order at randomized intervals (12-30 s).
static int cur_verse = 0;
static uint32_t verse_frame = 0;
static uint32_t verse_frames_total = 20u * FPS_APPROX;
static uint32_t verses_shown = 0;

static void pick_next_verse(void) {
    uint16_t roll = (uint16_t)(run_seed = (uint16_t)(run_seed * 25173u + 13849u));
    int next = (int)(roll % VERSE_DB_COUNT);
    if (next == cur_verse) next = (next + 1) % VERSE_DB_COUNT;
    cur_verse = next;
    verse_frame = 0;
    verses_shown += 1;
    verse_frames_total =
        (12u + (uint32_t)hash8(roll, (uint16_t)verses_shown, 3u) % 19u) * FPS_APPROX;
}

// Word-wrap into lines of at most max_chars, breaking on spaces.
#define MAX_LINES 12
static int wrap_verse(const char *text, int max_chars,
                      int line_start[MAX_LINES], int line_len[MAX_LINES]) {
    int len = (int)strlen(text);
    int pos = 0;
    int lines = 0;
    while (pos < len && lines < MAX_LINES) {
        int end = pos + max_chars;
        if (end >= len) {
            end = len;
        } else {
            int back = end;
            while (back > pos && text[back] != ' ') --back;
            if (back > pos) end = back;
        }
        line_start[lines] = pos;
        line_len[lines] = end - pos;
        lines += 1;
        pos = end;
        while (pos < len && text[pos] == ' ') ++pos;
    }
    return lines;
}

// ---------------------------------------------------------------- rendering
static void render_background(uint16_t *buf, uint32_t frame) {
    // Slow aurora: banded vertical gradient warped by two sine fields.
    for (int y = 0; y < FRAME_HEIGHT; y += 2) {
        int g1 = wave8((uint16_t)(y * 2 + (frame >> 3)));
        int g2 = wave8((uint16_t)(y * 3 - (frame >> 2) + 80));
        int r = 4 + (g2 >> 5);
        int g = 6 + (g1 >> 4);
        int b = 18 + (g1 >> 3) + (g2 >> 5);
        uint16_t ca = RGB565(r, g, b);
        uint16_t cb = RGB565(r, g, b + 6);
        uint16_t *row0 = buf + y * FRAME_WIDTH;
        uint16_t *row1 = row0 + FRAME_WIDTH;
        int bend = isin8((uint8_t)((y >> 1) + (frame >> 4))) >> 5;
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            uint16_t c = ((x + bend) & 15) == 0 ? cb : ca;
            row0[x] = c;
            row1[x] = c;
        }
    }
    // Star field with slow twinkle.
    for (int i = 0; i < 56; ++i) {
        int sx = (i * 47 + 13) % FRAME_WIDTH;
        int sy = (i * 61 + 7) % FRAME_HEIGHT;
        uint8_t tw = wave8((uint16_t)(frame * 2u + i * 23u));
        if (tw > 90) {
            uint16_t c = tw > 200 ? 0xFFFF : RGB565(120, 130, 170);
            fill_rect(buf, sx, sy, 1, 1, c);
            if (tw > 230) {
                fill_rect(buf, sx - 1, sy, 3, 1, RGB565(90, 100, 140));
                fill_rect(buf, sx, sy - 1, 1, 3, RGB565(90, 100, 140));
            }
        }
    }
    // Two drifting halos of light.
    for (int h = 0; h < 2; ++h) {
        int cx = 60 + h * 190 + (isin8((uint8_t)((frame >> 2) + h * 128)) >> 2);
        int cy = 60 + h * 110 + (isin8((uint8_t)((frame >> 3) + 64 + h * 40)) >> 3);
        for (int r = 26; r > 0; r -= 7) {
            uint8_t glow = (uint8_t)(30 + r);
            uint16_t c = RGB565(glow / 2, glow / 2, glow);
            // hollow diamond rings read as soft light at this resolution
            for (int t = 0; t < r; ++t) {
                fill_rect(buf, cx - t, cy - (r - t), 1, 1, c);
                fill_rect(buf, cx + t, cy - (r - t), 1, 1, c);
                fill_rect(buf, cx - t, cy + (r - t), 1, 1, c);
                fill_rect(buf, cx + t, cy + (r - t), 1, 1, c);
            }
        }
    }
}

static void render_scripture(uint16_t *buf, uint32_t frame) {
    const uint16_t gold = RGB565(224, 178, 84);
    const uint16_t gold_dim = RGB565(140, 110, 52);
    const uint16_t ink = RGB565(236, 240, 248);
    const uint16_t panel = RGB565(10, 12, 26);
    const char *text = VERSE_TEXTS[cur_verse];
    const char *ref = VERSE_REFS[cur_verse];
    int line_start[MAX_LINES];
    int line_len[MAX_LINES];
    int scale = 2;
    int max_chars = (FRAME_WIDTH - 48) / 12;
    int lines = wrap_verse(text, max_chars, line_start, line_len);
    char info[32];
    DateTime now;

    if (lines > 5) {
        scale = 1;
        max_chars = (FRAME_WIDTH - 48) / 6;
        lines = wrap_verse(text, max_chars, line_start, line_len);
    }

    if (verse_frame >= verse_frames_total) {
        pick_next_verse();
        text = VERSE_TEXTS[cur_verse];
        ref = VERSE_REFS[cur_verse];
        scale = 2;
        max_chars = (FRAME_WIDTH - 48) / 12;
        lines = wrap_verse(text, max_chars, line_start, line_len);
        if (lines > 5) {
            scale = 1;
            max_chars = (FRAME_WIDTH - 48) / 6;
            lines = wrap_verse(text, max_chars, line_start, line_len);
        }
    }

    render_background(buf, frame);

    // ---- verse card
    {
        int line_h = 8 * scale + 2;
        int card_h = lines * line_h + 44;
        int card_w = FRAME_WIDTH - 28;
        int card_x = 14;
        int card_y = 26 + (FRAME_HEIGHT - 60 - card_h) / 2;
        int reveal = (int)(verse_frame * 3u); // typewriter budget in chars
        int text_y = card_y + 18;

        fill_rect(buf, card_x + 3, card_y + 3, card_w, card_h, RGB565(3, 4, 8));
        fill_rect(buf, card_x, card_y, card_w, card_h, panel);
        stroke_rect(buf, card_x, card_y, card_w, card_h, 1, gold);
        stroke_rect(buf, card_x + 3, card_y + 3, card_w - 6, card_h - 6, 1, gold_dim);
        // corner caps
        fill_rect(buf, card_x - 1, card_y - 1, 7, 3, gold);
        fill_rect(buf, card_x - 1, card_y - 1, 3, 7, gold);
        fill_rect(buf, card_x + card_w - 6, card_y - 1, 7, 3, gold);
        fill_rect(buf, card_x + card_w - 2, card_y - 1, 3, 7, gold);
        fill_rect(buf, card_x - 1, card_y + card_h - 2, 7, 3, gold);
        fill_rect(buf, card_x - 1, card_y + card_h - 6, 3, 7, gold);
        fill_rect(buf, card_x + card_w - 6, card_y + card_h - 2, 7, 3, gold);
        fill_rect(buf, card_x + card_w - 2, card_y + card_h - 6, 3, 7, gold);

        // typewriter body text, centred lines
        {
            int shown = 0;
            for (int i = 0; i < lines; ++i) {
                int lx = (FRAME_WIDTH - line_len[i] * 6 * scale) / 2;
                for (int c = 0; c < line_len[i]; ++c) {
                    if (shown >= reveal) break;
                    draw_char(buf, lx + c * 6 * scale, text_y + i * line_h,
                              text[line_start[i] + c], scale, ink);
                    shown += 1;
                }
                if (shown >= reveal && reveal < (int)strlen(text)) {
                    if (((frame / 6u) & 1u) == 0u) {
                        int done_in_line = reveal;
                        for (int j = 0; j < i; ++j) done_in_line -= line_len[j];
                        if (done_in_line >= 0 && done_in_line <= line_len[i]) {
                            fill_rect(buf, lx + done_in_line * 6 * scale,
                                      text_y + i * line_h, 5 * scale, 7 * scale, gold);
                        }
                    }
                    break;
                }
            }
        }

        // reference plate under the card
        {
            int ref_w = (int)strlen(ref) * 6 + 34;
            int ref_x = (FRAME_WIDTH - ref_w) / 2;
            int ref_y = card_y + card_h - 16;
            fill_rect(buf, ref_x, ref_y, ref_w, 12, RGB565(24, 20, 8));
            stroke_rect(buf, ref_x, ref_y, ref_w, 12, 1, gold_dim);
            draw_text(buf, ref_x + 6, ref_y + 3, ref, 1, gold);
            draw_text(buf, ref_x + ref_w - 24, ref_y + 3, "KJV", 1, gold_dim);
        }
    }

    // ---- top bar: date | title | time + temperature
    now = current_clock();
    fill_rect(buf, 0, 0, FRAME_WIDTH, 18, RGB565(4, 5, 12));
    fill_rect(buf, 0, 18, FRAME_WIDTH, 1, gold_dim);
    snprintf(info, sizeof(info), "%04d-%02d-%02d", now.year, now.month, now.day);
    draw_text(buf, 6, 6, info, 1, RGB565(170, 180, 210));
    draw_text(buf, (FRAME_WIDTH - 9 * 6) / 2, 6, "SCRIPTURE", 1, gold);
    snprintf(info, sizeof(info), "%02d:%02d:%02d", now.hour, now.minute, now.second);
    draw_text(buf, FRAME_WIDTH - 6 - 8 * 6, 6, info, 1, 0xFFFF);
    {
        static int temp_c10;
        if ((frame % 30u) == 0u) temp_c10 = (int)(chip_temperature() * 10.0f);
        snprintf(info, sizeof(info), "MCU %d.%dC", temp_c10 / 10, iabs(temp_c10 % 10));
        draw_text(buf, FRAME_WIDTH - 6 - (int)strlen(info) * 6, 22, info, 1,
                  RGB565(140, 200, 160));
    }

    // ---- progress bar for the current verse dwell
    {
        int w = (int)((uint64_t)(FRAME_WIDTH - 12) * verse_frame / verse_frames_total);
        fill_rect(buf, 6, FRAME_HEIGHT - 20, FRAME_WIDTH - 12, 2, RGB565(30, 34, 54));
        fill_rect(buf, 6, FRAME_HEIGHT - 20, w, 2, gold);
    }

    // ---- footer: IONITY mark + verse counter
    {
        const uint16_t blue = RGB565(36, 90, 200);
        fill_rect(buf, 0, FRAME_HEIGHT - 16, FRAME_WIDTH, 16, RGB565(4, 5, 12));
        fill_rect(buf, 4, FRAME_HEIGHT - 14, 48, 12, 0xFFFF);
        stroke_rect(buf, 4, FRAME_HEIGHT - 14, 48, 12, 1, blue);
        draw_text(buf, 8, FRAME_HEIGHT - 11, "IONITY", 1, blue);
        snprintf(info, sizeof(info), "VERSE %04u/%04u",
                 (unsigned)(cur_verse + 1), (unsigned)VERSE_DB_COUNT);
        draw_text(buf, FRAME_WIDTH - 6 - (int)strlen(info) * 6, FRAME_HEIGHT - 11,
                  info, 1, RGB565(170, 180, 210));
    }

    verse_frame += 1;
}

// ---------------------------------------------------------------- cores
static void core1_scanline_callback(void) {
    const uint16_t *scanline_ptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline_ptr))
        ;

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

    {
        uint16_t noise = 0;
        adc_select_input(4);
        for (int i = 0; i < 16; ++i) {
            noise = (uint16_t)((noise << 1) ^ (adc_read() & 1u) ^ (noise >> 15));
        }
        run_seed = (uint16_t)(noise ^ (uint16_t)time_us_64() ^ 0x5A17u);
    }
    pick_next_verse();

    printf("\n[boot] pico-dvi scripture edition\n");
    printf("[boot] %dx%d RGB565 in %s DVI, %d verses in flash, seed %u\n",
           FRAME_WIDTH, FRAME_HEIGHT, MODE_LABEL, VERSE_DB_COUNT,
           (unsigned)run_seed);

    memset(framebuf, 0, sizeof(framebuf));
    render_scripture(framebuf[0], 0);

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
#ifdef DVI_INVERT_DIFFPAIRS_OVERRIDE
    dvi0.ser_cfg.invert_diffpairs = DVI_INVERT_DIFFPAIRS_OVERRIDE > 0;
#endif
#if !DIAG_SKIP_OVERCLOCK
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Queue the first two scanlines, then hand scanout to core1.
    {
        const uint16_t *line0 = &framebuf[0][0];
        const uint16_t *line1 = &framebuf[0][FRAME_WIDTH];
        queue_add_blocking_u32(&dvi0.q_colour_valid, &line0);
        queue_add_blocking_u32(&dvi0.q_colour_valid, &line1);
    }
    multicore_launch_core1(core1_main);
#endif

    watchdog_enable(WATCHDOG_TIMEOUT_MS, 1);

    {
        uint32_t frame = 0;
        absolute_time_t next_frame = get_absolute_time();
        for (;;) {
            uint8_t back = (uint8_t)(1u - front_idx);
            if (back != scanout_idx) {
                render_scripture(framebuf[back], frame);
                __dmb();
                front_idx = back;
                frame += 1;
                next_frame = delayed_by_ms(next_frame, FRAME_INTERVAL_MS);
            }
            watchdog_update();
            sleep_until(next_frame);
        }
    }
}
