/*
 * pico-dvi scripture edition V4 (OTA) - local-render C firmware for a
 * Pico 2 W (RP2350) in a Waveshare PICO-DVI-LCD carrier.
 *
 * Same ambient scripture display as V2, plus over-the-air updates: the
 * note-board web server also accepts a firmware image on POST /fw, stages
 * it in the upper half of flash, verifies a CRC32 and installs it on the
 * next boot from a RAM-resident copier. A failed or interrupted upload
 * never touches the running image. V2 (scripture_main.c, 23000 verses,
 * no OTA) is kept as-is; this variant carries 9000 verses to leave room
 * for the staging area.
 */

#include <stdio.h>
#include <stdlib.h>
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

#include "verses_ota.h"
#include "ionity_logo_bitmap.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#define FW_VERSION "4.0.0"

#ifndef NOTE_WIFI_ENABLED
#define NOTE_WIFI_ENABLED 0
#endif
#if NOTE_WIFI_ENABLED
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/apps/mdns.h"
#endif

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

static void draw_bitmap565(uint16_t *buf, int x, int y, int w, int h,
                           const uint16_t *bmp) {
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            int dx = x + px;
            int dy = y + py;
            if (dx < 0 || dx >= FRAME_WIDTH || dy < 0 || dy >= FRAME_HEIGHT) continue;
            uint16_t c = bmp[py * w + px];
            if (c) buf[dy * FRAME_WIDTH + dx] = c;
        }
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

// ---------------------------------------------------------------- OTA
// Flash layout (4 MB): running app in the lower 2016 KiB, staged image in
// the next 2016 KiB, one state sector at the top. An upload goes to the
// stage area only; the installer runs from RAM on the next boot.
#define OTA_APP_MAX     (2016u * 1024u)
#define OTA_STAGE_OFF   OTA_APP_MAX
#define OTA_STATE_OFF   (2u * OTA_APP_MAX)
#define OTA_MAGIC       0x494F5433u /* 'IOT3' */
#define XIP_BASE_ADDR   0x10000000u

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t crc;
} OtaState;

static uint32_t crc32_step(uint32_t crc, const uint8_t *data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Install a verified staged image. Runs entirely from RAM: it overwrites
// the region this firmware executes from, then resets the chip directly.
static void __no_inline_not_in_flash_func(ota_install)(uint32_t length) {
    static uint8_t chunk[FLASH_SECTOR_SIZE];
    uint32_t ints = save_and_disable_interrupts();
    for (uint32_t off = 0; off < length; off += FLASH_SECTOR_SIZE) {
        uint32_t n = length - off;
        if (n > FLASH_SECTOR_SIZE) n = FLASH_SECTOR_SIZE;
        const uint8_t *src = (const uint8_t *)(XIP_BASE_ADDR + OTA_STAGE_OFF + off);
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
            chunk[i] = i < n ? src[i] : 0xFF;
        }
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, chunk, FLASH_SECTOR_SIZE);
    }
    flash_range_erase(OTA_STATE_OFF, FLASH_SECTOR_SIZE); // clear the flag
    (void)ints;
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; // AIRCR system reset
    for (;;) { __asm volatile ("nop"); }
}

// Called first thing in main(), before DVI or Wi-Fi come up.
static void ota_apply_if_staged(void) {
    const OtaState *state = (const OtaState *)(XIP_BASE_ADDR + OTA_STATE_OFF);
    if (state->magic != OTA_MAGIC) return;
    if (state->length == 0 || state->length > OTA_STAGE_OFF) return;
    uint32_t crc = crc32_step(0,
        (const uint8_t *)(XIP_BASE_ADDR + OTA_STAGE_OFF), state->length);
    if (crc != state->crc) {
        // corrupt stage: discard it and boot normally
        flash_range_erase(OTA_STATE_OFF, FLASH_SECTOR_SIZE);
        return;
    }
    ota_install(state->length); // does not return
}

static void ota_mark_staged(uint32_t length, uint32_t crc) {
    static uint8_t sector[FLASH_SECTOR_SIZE];
    OtaState state = {OTA_MAGIC, length, crc};
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, &state, sizeof(state));
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(OTA_STATE_OFF, FLASH_SECTOR_SIZE);
    flash_range_program(OTA_STATE_OFF, sector, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

// Upload progress shared with the render/main loop.
static volatile int fw_mode = 0;        // 1 = upload in progress, display off
static volatile int fw_reboot = 0;      // set when staged and verified
static volatile int fw_aborted = 0;     // upload ended without staging
static uint32_t fw_expected, fw_received, fw_crc_want;

// ---------------------------------------------------------------- note board
// Anyone on the same Wi-Fi can push a note to the footer: the firmware joins
// the network and serves a one-field web page on port 80. The note stays
// until it is replaced (or cleared with an empty submit).
#define NOTE_MAX 120
static char note_text[NOTE_MAX + 1] = "";

#if NOTE_WIFI_ENABLED

static char net_ip[20] = "";
static volatile int net_up = 0;
static int net_started = 0;
static struct tcp_pcb *note_listen_pcb;
static char http_out[1400];
static uint32_t net_frame_seen = 0;

typedef enum {
    STREAM_RSS,
    STREAM_WEATHER,
} StreamKind;

typedef struct {
    const char *tag;
    const char *host;
    const char *path;
    StreamKind kind;
} StreamFeed;

static const StreamFeed stream_feeds[] = {
    {"BBC NEWS", "feeds.bbci.co.uk", "/news/rss.xml", STREAM_RSS},
    {"REUTERS", "feeds.reuters.com", "/reuters/topNews", STREAM_RSS},
    {"CNN WORLD", "rss.cnn.com", "/rss/edition.rss", STREAM_RSS},
    {"WEATHER", "wttr.in", "/?format=j1", STREAM_WEATHER},
};
static uint32_t stream_feed_idx = 0;
static uint32_t stream_next_refresh_ms = 0;
static volatile int stream_busy = 0;
static char stream_rss[96] = "SCRIPTURE";
static char stream_weather[64] = "OUTSIDE";
static char stream_place[32] = "";
static int stream_temp_c10 = 0;

typedef struct {
    struct tcp_pcb *pcb;
    ip_addr_t ip;
    volatile int resolved;
    volatile int connected;
    volatile int done;
    volatile int ok;
    volatile err_t err;
    char host[64];
    char path[128];
    char *body;
    int body_cap;
    int body_len;
    int header_done;
    char header[512];
    int header_len;
} HttpGetCtx;

static void text_clean(char *s) {
    int w = 0;
    for (int i = 0; s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 0x20 || c > 0x7A) continue;
        if (c == ' ' && w > 0 && s[w - 1] == ' ') continue;
        s[w++] = (char)c;
    }
    while (w > 0 && s[w - 1] == ' ') --w;
    s[w] = '\0';
}

static void decode_html_entities(char *s) {
    char out[96];
    int w = 0;
    for (int i = 0; s[i] && w < (int)sizeof(out) - 1; ++i) {
        if (s[i] == '&') {
            if (!strncmp(s + i, "&amp;", 5)) { out[w++] = '&'; i += 4; continue; }
            if (!strncmp(s + i, "&lt;", 4)) { out[w++] = '<'; i += 3; continue; }
            if (!strncmp(s + i, "&gt;", 4)) { out[w++] = '>'; i += 3; continue; }
            if (!strncmp(s + i, "&quot;", 6)) { out[w++] = '"'; i += 5; continue; }
            if (!strncmp(s + i, "&#39;", 5)) { out[w++] = '\''; i += 4; continue; }
        }
        out[w++] = s[i];
    }
    out[w] = '\0';
    snprintf(s, 96, "%s", out);
}

static const char *json_find_string(const char *text, const char *key) {
    const char *p = strstr(text, key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p = strchr(p, '"');
    if (!p) return NULL;
    return p + 1;
}

static void json_copy_value(const char *start, char *dst, int cap) {
    int w = 0;
    for (int i = 0; start[i] && w < cap - 1; ++i) {
        char c = start[i];
        if (c == '"' || c == '\r' || c == '\n') break;
        if (c == '\\' && start[i + 1]) {
            ++i;
            c = start[i];
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) continue;
        dst[w++] = c;
    }
    dst[w] = '\0';
    text_clean(dst);
}

static void parse_weather(const char *body) {
    const char *area = strstr(body, "\"nearest_area\"");
    if (area) area = strstr(area, "\"value\"");
    if (area) {
        json_copy_value(area, stream_place, sizeof(stream_place));
    }
    const char *temp = strstr(body, "\"temp_C\"");
    if (temp) {
        temp = strchr(temp, ':');
        if (temp) {
            ++temp;
            while (*temp == ' ' || *temp == '"') ++temp;
            stream_temp_c10 = (int)(strtol(temp, NULL, 10) * 10);
        }
    }
    if (stream_place[0]) {
        snprintf(stream_weather, sizeof(stream_weather), "%s %d.%dC",
                 stream_place, stream_temp_c10 / 10, iabs(stream_temp_c10 % 10));
    } else {
        snprintf(stream_weather, sizeof(stream_weather), "OUTSIDE %d.%dC",
                 stream_temp_c10 / 10, iabs(stream_temp_c10 % 10));
    }
}

static void parse_rss(const char *body, const char *tag) {
    const char *item = strstr(body, "<item>");
    if (!item) item = body;
    const char *title = strstr(item, "<title>");
    if (!title) return;
    title += 7;
    const char *end = strstr(title, "</title>");
    if (!end || end <= title) return;
    int len = (int)(end - title);
    if (len > 140) len = 140;
    char tmp[144];
    memcpy(tmp, title, (size_t)len);
    tmp[len] = '\0';
    decode_html_entities(tmp);
    text_clean(tmp);
    snprintf(stream_rss, sizeof(stream_rss), "%s: %s", tag, tmp);
    if ((int)strlen(stream_rss) >= (int)sizeof(stream_rss)) {
        stream_rss[sizeof(stream_rss) - 1] = '\0';
    }
}

static void http_client_close(HttpGetCtx *ctx) {
    if (ctx->pcb) {
        tcp_arg(ctx->pcb, NULL);
        tcp_recv(ctx->pcb, NULL);
        tcp_err(ctx->pcb, NULL);
        tcp_close(ctx->pcb);
        ctx->pcb = NULL;
    }
}

static void http_client_fail(HttpGetCtx *ctx, err_t err) {
    ctx->err = err;
    ctx->ok = 0;
    ctx->done = 1;
    http_client_close(ctx);
}

static err_t http_client_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    HttpGetCtx *ctx = (HttpGetCtx *)arg;
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        http_client_fail(ctx, err);
        return ERR_OK;
    }
    if (!p) {
        ctx->ok = 1;
        ctx->done = 1;
        http_client_close(ctx);
        return ERR_OK;
    }

    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *data = (const uint8_t *)q->payload;
        uint32_t len = q->len;
        while (len > 0) {
            if (!ctx->header_done) {
                while (len > 0 && ctx->header_len < (int)sizeof(ctx->header) - 1) {
                    ctx->header[ctx->header_len++] = (char)*data++;
                    len -= 1;
                    if (ctx->header_len >= 4 &&
                        memcmp(ctx->header + ctx->header_len - 4, "\r\n\r\n", 4) == 0) {
                        ctx->header[ctx->header_len] = '\0';
                        ctx->header_done = 1;
                        break;
                    }
                }
                if (!ctx->header_done) break;
            }
            while (len > 0 && ctx->body_len < ctx->body_cap - 1) {
                ctx->body[ctx->body_len++] = (char)*data++;
                len -= 1;
            }
            break;
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t http_client_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    HttpGetCtx *ctx = (HttpGetCtx *)arg;
    if (err != ERR_OK) {
        http_client_fail(ctx, err);
        return err;
    }
    int n = snprintf(ctx->header, sizeof(ctx->header),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Ionity\r\n"
                     "Connection: close\r\nAccept: */*\r\n\r\n",
                     ctx->path, ctx->host);
    err_t wr = tcp_write(pcb, ctx->header, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (wr != ERR_OK) {
        http_client_fail(ctx, wr);
        return wr;
    }
    tcp_output(pcb);
    ctx->connected = 1;
    return ERR_OK;
}

static void http_client_err(void *arg, err_t err) {
    HttpGetCtx *ctx = (HttpGetCtx *)arg;
    if (!ctx) return;
    ctx->err = err;
    ctx->ok = 0;
    ctx->done = 1;
    ctx->pcb = NULL;
}

static void http_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    HttpGetCtx *ctx = (HttpGetCtx *)arg;
    if (ipaddr) {
        ctx->ip = *ipaddr;
        ctx->resolved = 1;
    } else {
        http_client_fail(ctx, ERR_ARG);
    }
}

static int http_get_text(const char *host, const char *path, char *body, int body_cap, uint32_t timeout_ms) {
    HttpGetCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.host, sizeof(ctx.host), "%s", host);
    snprintf(ctx.path, sizeof(ctx.path), "%s", path);
    ctx.body = body;
    ctx.body_cap = body_cap;

    err_t dr = dns_gethostbyname(host, &ctx.ip, http_dns_found, &ctx);
    if (dr == ERR_ARG) return 0;
    if (dr == ERR_INPROGRESS) {
        absolute_time_t deadline = delayed_by_ms(get_absolute_time(), timeout_ms);
        while (!ctx.resolved) {
            cyw43_arch_poll();
            watchdog_update();
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return 0;
            sleep_ms(1);
        }
    } else if (dr != ERR_OK) {
        return 0;
    }

    ctx.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!ctx.pcb) return 0;
    tcp_arg(ctx.pcb, &ctx);
    tcp_err(ctx.pcb, http_client_err);
    tcp_recv(ctx.pcb, http_client_recv);
    tcp_sent(ctx.pcb, NULL);
    err_t cr = tcp_connect(ctx.pcb, &ctx.ip, 80, http_client_connected);
    if (cr != ERR_OK) {
        http_client_close(&ctx);
        return 0;
    }

    absolute_time_t deadline = delayed_by_ms(get_absolute_time(), timeout_ms);
    while (!ctx.done) {
        cyw43_arch_poll();
        watchdog_update();
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            http_client_fail(&ctx, ERR_TIMEOUT);
            break;
        }
        sleep_ms(1);
    }
    if (!ctx.done) http_client_close(&ctx);
    body[ctx.body_len] = '\0';
    return ctx.ok;
}

static void refresh_stream_data(void) {
    if (stream_busy || !net_up) return;
    stream_busy = 1;
    char buf[8192];
    const StreamFeed *feed = &stream_feeds[stream_feed_idx % (sizeof(stream_feeds) / sizeof(stream_feeds[0]))];
    if (feed->kind == STREAM_WEATHER) {
        if (http_get_text(feed->host, feed->path, buf, (int)sizeof(buf), 12000)) {
            parse_weather(buf);
        }
    } else {
        if (http_get_text(feed->host, feed->path, buf, (int)sizeof(buf), 12000)) {
            parse_rss(buf, feed->tag);
        }
    }
    stream_feed_idx += 1;
    stream_next_refresh_ms = to_ms_since_boot(get_absolute_time()) + 15u * 60u * 1000u;
    stream_busy = 0;
}

static void note_set(const char *raw) {
    int n = 0;
    for (const char *p = raw; *p && n < NOTE_MAX; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'') continue;
        if (c < 0x20 || c > 0x5F) continue;
        if (c == ' ' && n == 0) continue;
        note_text[n++] = c;
    }
    while (n > 0 && note_text[n - 1] == ' ') --n;
    note_text[n] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decode an application/x-www-form-urlencoded value in place-ish.
static void url_decode(const char *src, int src_len, char *dst, int dst_cap) {
    int n = 0;
    for (int i = 0; i < src_len && n < dst_cap - 1; ++i) {
        char c = src[i];
        if (c == '+') {
            dst[n++] = ' ';
        } else if (c == '%' && i + 2 < src_len) {
            int hi = hexval(src[i + 1]);
            int lo = hexval(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[n++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
}

// One firmware upload at a time; body bytes stream straight into the
// stage area through a sector-sized buffer.
static struct tcp_pcb *fw_pcb;
static uint8_t fw_buf[FLASH_SECTOR_SIZE];
static uint32_t fw_fill;
static uint32_t fw_flash_off;
static int fw_header_done;
static char fw_hdr[512];
static uint32_t fw_hdr_len;

static void fw_flush_buffer(void) {
    if (fw_fill == 0) return;
    memset(fw_buf + fw_fill, 0xFF, sizeof(fw_buf) - fw_fill);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(OTA_STAGE_OFF + fw_flash_off, FLASH_SECTOR_SIZE);
    flash_range_program(OTA_STAGE_OFF + fw_flash_off, fw_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    fw_flash_off += FLASH_SECTOR_SIZE;
    fw_fill = 0;
}

static void fw_take_body(const uint8_t *data, uint32_t len) {
    while (len > 0 && fw_received < fw_expected) {
        uint32_t space = (uint32_t)sizeof(fw_buf) - fw_fill;
        uint32_t n = len < space ? len : space;
        if (fw_received + n > fw_expected) n = fw_expected - fw_received;
        memcpy(fw_buf + fw_fill, data, n);
        fw_fill += n;
        fw_received += n;
        data += n;
        len -= n;
        if (fw_fill == sizeof(fw_buf)) fw_flush_buffer();
    }
}

static void fw_finish(struct tcp_pcb *pcb) {
    fw_flush_buffer();
    uint32_t crc = crc32_step(0,
        (const uint8_t *)(XIP_BASE_ADDR + OTA_STAGE_OFF), fw_expected);
    const char *reply;
    if (crc == fw_crc_want) {
        ota_mark_staged(fw_expected, crc);
        reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nSTAGED - REBOOTING\n";
        fw_reboot = 1;
        printf("[ota] staged %u bytes, crc ok - rebooting to install\n",
               (unsigned)fw_expected);
    } else {
        reply = "HTTP/1.1 422 Unprocessable\r\nConnection: close\r\n\r\nCRC MISMATCH\n";
        printf("[ota] crc mismatch, upload discarded\n");
        fw_aborted = 1;
    }
    tcp_write(pcb, reply, (u16_t)strlen(reply), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    fw_pcb = NULL;
    fw_mode = 0;
}

static err_t note_http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        if (pcb == fw_pcb) { fw_pcb = NULL; fw_mode = 0; fw_aborted = 1; }
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    // ---- streaming firmware upload path
    if (pcb == fw_pcb) {
        for (struct pbuf *q = p; q; q = q->next) {
            if (!fw_header_done) {
                const uint8_t *d = (const uint8_t *)q->payload;
                uint32_t n = q->len;
                while (n > 0 && fw_hdr_len < sizeof(fw_hdr) - 1) {
                    fw_hdr[fw_hdr_len++] = (char)*d++;
                    n -= 1;
                    if (fw_hdr_len >= 4 &&
                        memcmp(fw_hdr + fw_hdr_len - 4, "\r\n\r\n", 4) == 0) {
                        fw_header_done = 1;
                        break;
                    }
                }
                if (fw_header_done && n > 0) fw_take_body(d, n);
            } else {
                fw_take_body((const uint8_t *)q->payload, q->len);
            }
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        if (fw_header_done && fw_received >= fw_expected) fw_finish(pcb);
        return ERR_OK;
    }

    if (p->len > 4) {
        char req[512];
        int len = p->len < (int)sizeof(req) - 1 ? (int)p->len : (int)sizeof(req) - 1;
        memcpy(req, p->payload, (size_t)len);
        req[len] = '\0';

        // ---- firmware upload start: POST /fw?len=NNN&crc=HEX8
        if (strncmp(req, "POST /fw?", 9) == 0 && !fw_pcb) {
            const char *l = strstr(req, "len=");
            const char *c = strstr(req, "crc=");
            uint32_t want_len = l ? (uint32_t)strtoul(l + 4, NULL, 10) : 0;
            uint32_t want_crc = c ? (uint32_t)strtoul(c + 4, NULL, 16) : 0;
            if (want_len > 0 && want_len <= OTA_STAGE_OFF) {
                // flash writes stall XIP, which would crash core1's DVI
                // loop mid-fetch - stop the display first
                multicore_reset_core1();
                fw_pcb = pcb;
                fw_expected = want_len;
                fw_crc_want = want_crc;
                fw_received = 0;
                fw_fill = 0;
                fw_flash_off = 0;
                fw_hdr_len = 0;
                fw_header_done = 0;
                fw_mode = 1;
                printf("[ota] receiving %u bytes\n", (unsigned)want_len);
                // replay this first pbuf through the streaming path
                for (struct pbuf *q = p; q; q = q->next) {
                    const uint8_t *d = (const uint8_t *)q->payload;
                    uint32_t n = q->len;
                    while (n > 0 && !fw_header_done && fw_hdr_len < sizeof(fw_hdr) - 1) {
                        fw_hdr[fw_hdr_len++] = (char)*d++;
                        n -= 1;
                        if (fw_hdr_len >= 4 &&
                            memcmp(fw_hdr + fw_hdr_len - 4, "\r\n\r\n", 4) == 0) {
                            fw_header_done = 1;
                        }
                    }
                    if (fw_header_done && n > 0) fw_take_body(d, n);
                }
                tcp_recved(pcb, p->tot_len);
                pbuf_free(p);
                if (fw_header_done && fw_received >= fw_expected) fw_finish(pcb);
                return ERR_OK;
            }
        }

        // ---- device identity for scanners and the apps
        if (strncmp(req, "GET /id", 7) == 0) {
            int n = snprintf(http_out, sizeof(http_out),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
                "{\"device\":\"ionity-scripture\",\"version\":\"" FW_VERSION "\","
                "\"verses\":%d,\"ota\":true}\n", VERSE_DB_COUNT);
            tcp_write(pcb, http_out, (u16_t)n, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_close(pcb);
            return ERR_OK;
        }

        const char *q = strstr(req, "GET /note?t=");
        if (q) {
            const char *val = q + 12;
            const char *end = val;
            while (*end && *end != ' ' && *end != '&' && *end != '\r') ++end;
            char decoded[NOTE_MAX * 3 + 1];
            url_decode(val, (int)(end - val), decoded, (int)sizeof(decoded));
            note_set(decoded);
            printf("[note] set to \"%s\"\n", note_text);
        }

        int n = snprintf(http_out, sizeof(http_out),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>IONITY NOTE</title><style>"
            "body{font-family:monospace;background:#0a0c1a;color:#e0b254;"
            "text-align:center;padding-top:8vh}"
            "input{font-size:1.1em;padding:10px;width:82%%;max-width:420px;"
            "background:#141830;color:#fff;border:1px solid #e0b254}"
            "button{font-size:1.1em;padding:10px 28px;margin-top:14px;"
            "background:#e0b254;color:#0a0c1a;border:0;font-weight:bold}"
            "p{color:#aab4d2}</style></head><body>"
            "<h2>IONITY SCRIPTURE<br>NOTE BOARD</h2>"
            "<form action='/note'>"
            "<input name='t' maxlength='%d' placeholder='Type a note for the screen...' autofocus>"
            "<br><button>SEND TO SCREEN</button></form>"
            "<p>ON SCREEN NOW:<br><b>%s</b></p>"
            "<p>Submit an empty note to clear it.</p>"
            "</body></html>",
            NOTE_MAX, note_text[0] ? note_text : "(no note)");
        tcp_write(pcb, http_out, (u16_t)n, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t note_http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    tcp_recv(newpcb, note_http_recv);
    return ERR_OK;
}

static void note_server_start(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        tcp_close(pcb);
        return;
    }
    note_listen_pcb = tcp_listen_with_backlog(pcb, 2);
    if (note_listen_pcb) tcp_accept(note_listen_pcb, note_http_accept);
}

// Called once per frame from the render loop; drives the Wi-Fi state.
static void net_task(uint32_t frame) {
    cyw43_arch_poll();
    if ((frame % FPS_APPROX) != 0u) return; // status checks once a second

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (status == CYW43_LINK_UP) {
        if (!net_up) {
            const ip4_addr_t *addr = netif_ip4_addr(netif_default);
            snprintf(net_ip, sizeof(net_ip), "%s", ip4addr_ntoa(addr));
            net_up = 1;
            printf("[note] wifi up, note board at http://%s/\n", net_ip);
            stream_next_refresh_ms = 0;
        }
        if (!net_started) {
            note_server_start();
            // announce as ionity-scripture.local so no one has to hunt IPs
            mdns_resp_init();
            mdns_resp_add_netif(netif_default, "ionity-scripture");
            net_started = 1;
        }
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms >= stream_next_refresh_ms) refresh_stream_data();
    } else if (status == CYW43_LINK_FAIL || status == CYW43_LINK_NONET ||
               status == CYW43_LINK_BADAUTH) {
        net_up = 0;
        cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD,
                                      CYW43_AUTH_WPA2_AES_PSK);
    } else if (status == CYW43_LINK_DOWN) {
        net_up = 0;
    }
}
#endif // NOTE_WIFI_ENABLED

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
    {
        const char *headline = stream_rss[0] ? stream_rss : "SCRIPTURE";
        int headline_w = (int)strlen(headline) * 6;
        int cx = (FRAME_WIDTH - headline_w) / 2;
        if (headline_w <= FRAME_WIDTH - 120) {
            draw_text(buf, cx, 6, headline, 1, gold);
        } else {
            int span = headline_w + 48;
            int off = (int)((frame / 2u) % (uint32_t)span);
            for (int c = 0; headline[c]; ++c) {
                int px = 60 + FRAME_WIDTH / 2 - off + c * 6;
                if (px >= 60 && px + 6 <= FRAME_WIDTH - 60) {
                    draw_char(buf, px, 6, headline[c], 1, gold);
                }
            }
        }
    }
    snprintf(info, sizeof(info), "%02d:%02d:%02d", now.hour, now.minute, now.second);
    draw_text(buf, FRAME_WIDTH - 6 - 8 * 6, 6, info, 1, 0xFFFF);
    {
        static int temp_c10;
        if ((frame % 30u) == 0u) temp_c10 = (int)(chip_temperature() * 10.0f);
        snprintf(info, sizeof(info), "MCU %d.%dC", temp_c10 / 10, iabs(temp_c10 % 10));
        draw_text(buf, FRAME_WIDTH - 6 - (int)strlen(info) * 6, 22, info, 1,
                  RGB565(140, 200, 160));
        if (stream_weather[0]) {
            draw_text(buf, 8, 22, stream_weather, 1, RGB565(120, 200, 220));
        }
    }

    // ---- progress bar for the current verse dwell
    {
        int w = (int)((uint64_t)(FRAME_WIDTH - 12) * verse_frame / verse_frames_total);
        fill_rect(buf, 6, FRAME_HEIGHT - 20, FRAME_WIDTH - 12, 2, RGB565(30, 34, 54));
        fill_rect(buf, 6, FRAME_HEIGHT - 20, w, 2, gold);
    }

    // ---- footer: IONITY mark + note board + verse counter
    {
        const uint16_t blue = RGB565(36, 90, 200);
        int count_w;
        fill_rect(buf, 0, FRAME_HEIGHT - 16, FRAME_WIDTH, 16, RGB565(4, 5, 12));
        draw_bitmap565(buf, 4, FRAME_HEIGHT - 19, IONITY_LOGO_W, IONITY_LOGO_H,
                       &IONITY_LOGO_RGB565[0][0]);
        snprintf(info, sizeof(info), "VERSE %04u/%04u",
                 (unsigned)(cur_verse + 1), (unsigned)VERSE_DB_COUNT);
        count_w = (int)strlen(info) * 6;
        draw_text(buf, FRAME_WIDTH - 6 - count_w, FRAME_HEIGHT - 11,
                  info, 1, RGB565(170, 180, 210));

        // note area between the mark and the counter
        {
            int area_x = IONITY_LOGO_W + 12;
            int area_w = FRAME_WIDTH - 6 - count_w - 6 - area_x;
            const char *msg = note_text;
            char hint[40];
#if NOTE_WIFI_ENABLED
            if (!note_text[0]) {
                if (net_up) {
                    snprintf(hint, sizeof(hint), "NOTES: HTTP://%s", net_ip);
                    msg = hint;
                } else {
                    msg = ((frame / 15u) & 1u) ? "WIFI JOINING." : "WIFI JOINING..";
                }
            }
#else
            (void)hint;
#endif
            if (msg[0]) {
                int msg_len = (int)strlen(msg);
                int msg_w = msg_len * 6;
                if (msg_w <= area_w) {
                    draw_text(buf, area_x + (area_w - msg_w) / 2, FRAME_HEIGHT - 11,
                              msg, 1, gold);
                } else {
                    // marquee scroll for long notes
                    int span = msg_w + 48;
                    int off = (int)((frame / 2u) % (uint32_t)span);
                    for (int c = 0; c < msg_len; ++c) {
                        int px = area_x + area_w - off + c * 6;
                        if (px >= area_x && px + 6 <= area_x + area_w) {
                            draw_char(buf, px, FRAME_HEIGHT - 11, msg[c], 1, gold);
                        }
                    }
                }
            }
        }
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
    // Install a verified staged OTA image before anything else starts.
    ota_apply_if_staged();

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

#if NOTE_WIFI_ENABLED
    if (cyw43_arch_init() == 0) {
        cyw43_arch_enable_sta_mode();
        cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD,
                                      CYW43_AUTH_WPA2_AES_PSK);
        printf("[boot] joining wifi \"%s\"\n", WIFI_SSID);
    } else {
        printf("[boot] cyw43 init failed - note board disabled\n");
    }
#endif

    watchdog_enable(WATCHDOG_TIMEOUT_MS, 1);

    {
        uint32_t frame = 0;
        absolute_time_t next_frame = get_absolute_time();
        for (;;) {
#if NOTE_WIFI_ENABLED
            net_task(frame);
            if (fw_mode || fw_reboot || fw_aborted) {
                // display is stopped during an upload; keep the network
                // and watchdog alive, then reboot into the result
                if (fw_reboot || fw_aborted) {
                    sleep_ms(300); // let the HTTP reply drain
                    watchdog_reboot(0, 0, 50);
                    for (;;) { tight_loop_contents(); }
                }
                watchdog_update();
                sleep_ms(1);
                continue;
            }
#endif
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
