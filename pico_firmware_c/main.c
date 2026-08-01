/*
 * pico-dvi-art client - C firmware for a Pico 2 W (RP2350) in a
 * Waveshare PICO-DVI-LCD carrier.
 *
 * Why C and not MicroPython/CircuitPython:
 *   The carrier wires DVI to GP8-GP15. On RP2350 the HSTX peripheral only
 *   exists on GP12-GP19, so CircuitPython's picodvi (HSTX-only on RP2350)
 *   physically cannot drive it. PicoDVI generates DVI with PIO, which works
 *   on any pins - and its picodvi_dvi_cfg is an exact match for this board.
 *
 * Mode: 640x480p60 DVI carrying a pixel-doubled 320x240 RGB565 framebuffer,
 * which is the same mode Waveshare's own hello_dvi demo uses on this panel.
 *
 * Split across the two cores:
 *   core1 - TMDS encode + DMA (dvi_scanbuf_main_16bpp), never blocked
 *   core0 - renders the animation into a local back buffer, hands scanlines
 *           to core1, and services lightweight Wi-Fi control/telemetry.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"

#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"

#include "wifi_config.h"

// Diagnostic build switch: skip the DVI overclock and scan-out entirely, to
// separate wireless problems from display problems.
#ifndef DIAG_SKIP_OVERCLOCK
#define DIAG_SKIP_OVERCLOCK 0
#endif

// ---------------------------------------------------------------- display
// The panel is 1024x600 and scales whatever standard DVI mode we feed it. The
// framebuffer is always half the mode in each axis because libdvi pixel-doubles
// it (DVI_SYMBOLS_PER_WORD=2 horizontally, DVI_VERTICAL_REPEAT=2 vertically).
// Must match pc_server/config.py DVI_MODES.
#ifndef DVI_MODE_800X480
#define DVI_MODE_800X480 0
#endif

#if DVI_MODE_800X480
#define FRAME_WIDTH   400
#define DVI_TIMING    dvi_timing_800x480p_60hz
#else
#define FRAME_WIDTH   320
#define DVI_TIMING    dvi_timing_640x480p_60hz
#endif
#define FRAME_HEIGHT  240
#define FRAME_PIXELS  (FRAME_WIDTH * FRAME_HEIGHT)

#define VREG_VSEL     VREG_VOLTAGE_1_20

// One join attempt must fit inside the watchdog window, and the watchdog itself
// is capped at roughly 8.3s by the hardware counter.
#define WATCHDOG_TIMEOUT_MS 8000
#define JOIN_TIMEOUT_MS     20000
#define WIFI_BACKOFF_MIN_MS 3000
#define WIFI_BACKOFF_MAX_MS 60000
#define JOIN_FAILURES_BEFORE_RESET 3
// lwIP retransmits an unanswered SYN with a growing backoff; give a connect
// long enough to survive that before calling it dead.
#define TCP_CONNECT_WINDOW_MS 12000
// The CYW43 defaults to PM2 powersave, where the radio dozes between beacons.
// Mesh nodes routinely fail to buffer traffic for dozing clients, so ARP for
// us goes unanswered and the board turns unreachable seconds after it joins.
// This is a mains-powered display: keep the radio awake.
#define RADIO_PM_MODE cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1)

struct dvi_inst dvi0;
static bool dvi_running = false;

// Updated by the network code for the small status overlay.
static char status_line[48] = "STARTING";
static char device_line[48] = "IP 0.0.0.0  MCU --.-C";
static float cached_temperature = 0.0f;

// Two framebuffers: core0 scans one out while rendering into the other.
static uint16_t framebuf[2][FRAME_PIXELS];
static volatile uint8_t front_idx = 0;

static inline uint16_t *front_buffer(void) { return framebuf[front_idx]; }
static inline uint16_t *back_buffer(void)  { return framebuf[front_idx ^ 1]; }

// ---------------------------------------------------------------- protocol
static const uint8_t MAGIC_FRAME[4]   = {0xAA, 0xBB, 0xCC, 0xDD};
static const uint8_t MAGIC_COMMAND[4] = {0xAA, 0xBB, 0xCC, 0xEE};
#define HEADER_SIZE 8

enum rx_state { RX_HEADER, RX_COMMAND, RX_DISCARD };

static struct {
    enum rx_state state;
    uint8_t  header[HEADER_SIZE];
    uint32_t header_got;
    uint32_t payload_len;
    uint32_t payload_got;
    uint8_t  command[256];
    uint32_t legacy_frames;
    uint32_t drops;
} rx;

static struct tcp_pcb *client_pcb = NULL;
static volatile bool link_up = false;
static volatile bool reboot_to_bootsel = false;
static volatile bool reboot_requested = false;

// ---------------------------------------------------------------- helpers
static float chip_temperature(void) {
    adc_select_input(4);
    // 12-bit conversion over the 3.3V reference, per the RP2350 datasheet.
    const float conversion = 3.3f / (1 << 12);
    float volts = adc_read() * conversion;
    return 27.0f - (volts - 0.706f) / 0.001721f;
}

static const char *ip_string(void) {
    if (!netif_default) return "0.0.0.0";
    return ip4addr_ntoa(netif_ip4_addr(netif_default));
}

static void update_device_line(void) {
    cached_temperature = chip_temperature();
    snprintf(device_line, sizeof(device_line), "IP %s  MCU %.1fC",
             ip_string(), (double)cached_temperature);
}

// Fingerprint the credentials rather than printing them: this proves whether
// the strings reaching the radio are byte-identical to the ones on the PC,
// without ever putting a password on the wire or in a log.
static uint32_t str_fingerprint(const char *s) {
    uint32_t hash = 2166136261u;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

// Strongest BSS advertising our SSID, learned from a scan. This network is a
// mesh with several BSSIDs spread over channels 1 and 8; letting the firmware
// pick one is a lottery that frequently lands on a distant node and gets the
// association refused. Aiming at a specific BSSID + channel makes the join
// deterministic.
static uint8_t best_bssid[6];
static int     best_rssi;
static uint32_t best_channel;
static bool    best_valid;

static int on_scan_result(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result) return 0;
    printf("[scan] %-32.*s rssi %4d chan %3d auth %u\n",
           result->ssid_len, result->ssid, result->rssi,
           result->channel, result->auth_mode);
    if (result->ssid_len == strlen(WIFI_SSID) &&
        !memcmp(result->ssid, WIFI_SSID, result->ssid_len) &&
        (!best_valid || result->rssi > best_rssi)) {
        memcpy(best_bssid, result->bssid, sizeof(best_bssid));
        best_rssi = result->rssi;
        best_channel = result->channel;
        best_valid = true;
    }
    return 0;
}

// A scan proves whether the radio and its SPI bus are healthy: if the AP shows
// up with a sane RSSI then a failed join is genuinely about credentials, not
// about the wireless chip being starved or overclocked. It also picks the
// BSSID that wifi_join() will aim at.
static void wifi_scan(void) {
    cyw43_wifi_scan_options_t options = {0};
    printf("[scan] looking for networks\n");
    best_valid = false;
    cyw43_arch_lwip_begin();
    int rc = cyw43_wifi_scan(&cyw43_state, &options, NULL, on_scan_result);
    cyw43_arch_lwip_end();
    if (rc != 0) {
        printf("[scan] could not start (rc %d)\n", rc);
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(12000);
    while (cyw43_wifi_scan_active(&cyw43_state) &&
           absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        sleep_ms(200);
    }
    if (best_valid) {
        printf("[scan] best '%s' at %02x:%02x:%02x:%02x:%02x:%02x "
               "rssi %d chan %lu\n", WIFI_SSID,
               best_bssid[0], best_bssid[1], best_bssid[2],
               best_bssid[3], best_bssid[4], best_bssid[5],
               best_rssi, (unsigned long)best_channel);
    } else {
        printf("[scan] '%s' not found\n", WIFI_SSID);
    }
    printf("[scan] done\n");
}

static const char *link_name(int status) {
    switch (status) {
        case CYW43_LINK_DOWN:    return "down";
        case CYW43_LINK_JOIN:    return "joining";
        case CYW43_LINK_NOIP:    return "no-ip";
        case CYW43_LINK_UP:      return "up";
        case CYW43_LINK_FAIL:    return "fail";
        case CYW43_LINK_NONET:   return "no-net";
        case CYW43_LINK_BADAUTH: return "badauth";
        default:                 return "?";
    }
}

// Do NOT use cyw43_arch_wifi_connect_*_ms() here. That helper polls with
// `while (status >= 0)`, so it bails out the instant the driver reports a
// transient CYW43_LINK_BADAUTH (-3). On a multi-AP/mesh SSID (this network
// advertises several BSSIDs) the first association attempt is routinely
// refused while the controller steers the client, and the driver itself
// expects that - cyw43_ctrl.c turns a BADAUTH back into ACTIVE when a good
// AUTH follows it. MicroPython connects on this exact network because its
// connect() is async and the caller tolerates the bounce, so mirror that:
// issue one join and then poll patiently.
//
// Deliberately ONE attempt per call, always WPA2-AES (the scan reports auth 5
// = WPA2 for every BSS). Hammering an AP with back-to-back association
// requests - especially cycling auth modes - gets the MAC rate-limited, which
// turns a recoverable BADAUTH into a solid wall of refusals. The caller backs
// off between calls instead.
static bool wifi_join(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    int last_status = 0xFF;

    // Drop any half-open association left over from a previous attempt or a
    // reboot, otherwise the AP still has us on its client list and refuses the
    // new request.
    cyw43_arch_lwip_begin();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_lwip_end();
    sleep_ms(200);
    watchdog_update();

    // Plain join: no BSSID, no channel. Aiming at a specific BSSID makes the
    // firmware report NONET on this mesh even when that BSS is 38 dBm away, so
    // let the chip pick the node and rely on the patient poll below plus the
    // caller's back-off to ride out a refusal.
    int rc;
    cyw43_arch_lwip_begin();
    rc = cyw43_wifi_join(&cyw43_state,
                         strlen(WIFI_SSID), (const uint8_t *)WIFI_SSID,
                         strlen(WIFI_PASS), (const uint8_t *)WIFI_PASS,
                         CYW43_AUTH_WPA2_AES_PSK, NULL, CYW43_CHANNEL_NONE);
    cyw43_arch_lwip_end();
    printf("[wifi] join requested (rc %d)\n", rc);
    if (rc != 0) return false;

    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        cyw43_arch_poll();
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != last_status) {
            printf("[wifi] link %s (%d)\n", link_name(status), status);
            snprintf(status_line, sizeof(status_line), "WIFI %s", link_name(status));
            last_status = status;
        }
        if (status == CYW43_LINK_UP) {
            printf("[wifi] joined '%s' as %s\n", WIFI_SSID, ip_string());
            return true;
        }
        sleep_ms(100);
    }

    printf("[wifi] no link after %lu ms\n", (unsigned long)timeout_ms);
    return false;
}

// A radio that keeps getting refused, or keeps associating to a node that
// bridges nothing, holds firmware-side state that only a full power-cycle
// clears. If it does not come back, the watchdog reboot is the last resort.
static void radio_power_cycle(void) {
    printf("[wifi] power-cycling the radio\n");
    watchdog_update();
    cyw43_arch_deinit();
    sleep_ms(500);
    watchdog_update();
    if (cyw43_arch_init() != 0) {
        printf("[wifi] radio did not come back - rebooting\n");
        watchdog_reboot(0, 0, 0);
    }
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, RADIO_PM_MODE);
    printf("[wifi] radio back up\n");
    watchdog_update();
}

static void send_line(const char *text) {    if (!client_pcb || !link_up) return;
    size_t len = strlen(text);
    cyw43_arch_lwip_begin();
    if (tcp_sndbuf(client_pcb) >= len) {
        tcp_write(client_pcb, text, len, TCP_WRITE_FLAG_COPY);
        tcp_output(client_pcb);
    }
    cyw43_arch_lwip_end();
}

static void handle_command(const uint8_t *payload, uint32_t len) {
    char buf[257];
    uint32_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';
    printf("[cmd] %s\n", buf);

    // The C firmware cannot rewrite its own flash, so an OTA request drops the
    // board into BOOTSEL instead. The PC-side watcher sees the RPI-RP2 drive
    // appear and copies the new .uf2 across - same outcome, fully automatic.
    if (strstr(buf, "\"ota\"")) {
        reboot_to_bootsel = true;
    } else if (strstr(buf, "\"reboot\"")) {
        reboot_requested = true;
    }
}

// ---------------------------------------------------------------- rx parse
static void consume(const uint8_t *data, uint32_t len) {
    while (len > 0) {
        switch (rx.state) {
        case RX_HEADER: {
            uint32_t want = HEADER_SIZE - rx.header_got;
            uint32_t take = len < want ? len : want;
            memcpy(rx.header + rx.header_got, data, take);
            rx.header_got += take;
            data += take;
            len  -= take;
            if (rx.header_got < HEADER_SIZE) return;

            rx.header_got = 0;
            rx.payload_got = 0;
            rx.payload_len = (uint32_t)rx.header[4]
                           | ((uint32_t)rx.header[5] << 8)
                           | ((uint32_t)rx.header[6] << 16)
                           | ((uint32_t)rx.header[7] << 24);

            if (!memcmp(rx.header, MAGIC_FRAME, 4)) {
                // Updated firmware renders locally. Drain legacy frame packets
                // so an older desktop app cannot overwrite or desynchronise it.
                rx.legacy_frames++;
                rx.state = RX_DISCARD;
            } else if (!memcmp(rx.header, MAGIC_COMMAND, 4)) {
                rx.state = (rx.payload_len <= sizeof(rx.command)) ? RX_COMMAND : RX_DISCARD;
            } else {
                rx.drops++;
                rx.state = RX_DISCARD;
            }
            break;
        }
        case RX_COMMAND: {
            uint32_t want = rx.payload_len - rx.payload_got;
            uint32_t take = len < want ? len : want;
            memcpy(rx.command + rx.payload_got, data, take);
            rx.payload_got += take;
            data += take;
            len  -= take;
            if (rx.payload_got == rx.payload_len) {
                handle_command(rx.command, rx.payload_len);
                rx.state = RX_HEADER;
            }
            break;
        }
        case RX_DISCARD: {
            uint32_t want = rx.payload_len - rx.payload_got;
            uint32_t take = len < want ? len : want;
            rx.payload_got += take;
            data += take;
            len  -= take;
            if (rx.payload_got == rx.payload_len) rx.state = RX_HEADER;
            break;
        }
        }
    }
}

// ---------------------------------------------------------------- lwIP glue
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        printf("[net] server closed the link\n");
        link_up = false;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        consume((const uint8_t *)q->payload, q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    printf("[net] link error %d\n", err);
    client_pcb = NULL;
    link_up = false;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) {
        printf("[net] connect failed %d\n", err);
        link_up = false;
        return err;
    }
    printf("[net] linked - control and telemetry only\n");
    snprintf(status_line, sizeof(status_line), "LOCAL ART - CONTROL ONLINE");
    link_up = true;
    rx.state = RX_HEADER;
    rx.header_got = 0;
    rx.payload_got = 0;

    char line[96];
    snprintf(line, sizeof(line), "HELLO %d %s LOCAL\n", FIRMWARE_VERSION, DEVICE_ID);
    send_line(line);
    snprintf(line, sizeof(line), "TEMP %.2f\n", chip_temperature());
    send_line(line);
    return ERR_OK;
}

static bool connect_to_server(void) {
    ip_addr_t server;
    if (!ip4addr_aton(SERVER_IP, &server)) {
        printf("[net] SERVER_IP '%s' is not a valid address\n", SERVER_IP);
        return false;
    }
    cyw43_arch_lwip_begin();
    client_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!client_pcb) {
        cyw43_arch_lwip_end();
        return false;
    }
    tcp_recv(client_pcb, on_recv);
    tcp_err(client_pcb, on_err);
    printf("[net] connecting to %s:%d\n", SERVER_IP, SERVER_PORT);
    snprintf(status_line, sizeof(status_line), "WIFI OK - CONNECTING TO SERVER");
    err_t err = tcp_connect(client_pcb, &server, SERVER_PORT, on_connected);
    cyw43_arch_lwip_end();
    return err == ERR_OK;
}

static void close_link(void) {
    cyw43_arch_lwip_begin();
    if (client_pcb) {
        tcp_recv(client_pcb, NULL);
        tcp_err(client_pcb, NULL);
        tcp_abort(client_pcb);
        client_pcb = NULL;
    }
    cyw43_arch_lwip_end();
    link_up = false;
}

// ---------------------------------------------------------------- offline
// 5x7 ASCII font, 0x20..0x5F (space through underscore). Five column bytes per
// glyph, bit 0 = top row. Small enough to be worth carrying so the panel can
// explain itself instead of showing an anonymous test pattern.
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

static void draw_text(uint16_t *buf, int x, int y, const char *s, int scale,
                      uint16_t rgb) {
    for (; *s; ++s) {
        draw_char(buf, x, y, *s, scale, rgb);
        x += 6 * scale;
    }
}

static uint16_t wheel_rgb565(uint8_t hue, uint8_t brightness) {
    uint8_t region = hue / 43;
    uint8_t offset = (uint8_t)((hue - region * 43) * 6);
    uint8_t up = offset;
    uint8_t down = (uint8_t)(255 - offset);
    uint8_t r = 0, g = 0, b = 0;
    switch (region) {
    case 0: r = 255;  g = up;   break;
    case 1: r = down; g = 255;  break;
    case 2: g = 255;  b = up;   break;
    case 3: g = down; b = 255;  break;
    case 4: r = up;   b = 255;  break;
    default:r = 255;  b = down; break;
    }
    r = (uint8_t)(((uint16_t)r * brightness) >> 8);
    g = (uint8_t)(((uint16_t)g * brightness) >> 8);
    b = (uint8_t)(((uint16_t)b * brightness) >> 8);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Integer-only local renderer: no framebuffer crosses Wi-Fi and no floating
// point is used in the hot path. At 320x240 this comfortably leaves core0 time
// to feed DVI and service lwIP.
static void draw_local_art(uint16_t *buf, uint32_t frame) {
    int cx = FRAME_WIDTH / 2;
    int cy = FRAME_HEIGHT / 2;
    for (uint y = 0; y < FRAME_HEIGHT; ++y) {
        for (uint x = 0; x < FRAME_WIDTH; ++x) {
            int dx = (int)x - cx;
            int dy = (int)y - cy;
            uint32_t radius = (uint32_t)(dx * dx + dy * dy);
            uint32_t waves = ((x * 5u + frame * 3u) ^
                              (y * 7u - frame * 2u) ^
                              (radius >> 5));
            uint8_t hue = (uint8_t)(waves + frame + (radius >> 7));
            uint8_t brightness = (uint8_t)(112 + ((waves >> 2) & 0x7f));
            buf[y * FRAME_WIDTH + x] = wheel_rgb565(hue, brightness);
        }
    }

    const uint16_t white = 0xFFFF;
    const uint16_t cyan = 0x07FF;
    for (uint y = 8; y < 50 && y < FRAME_HEIGHT; ++y) {
        for (uint x = 8; x < FRAME_WIDTH - 8; ++x) {
            buf[y * FRAME_WIDTH + x] = 0x0000;
        }
    }

    draw_text(buf, 14, 14, "PICO DVI - LOCAL", 1, white);
    draw_text(buf, 14, 28, status_line, 1, cyan);
    draw_text(buf, 14, 40, device_line, 1, white);
}

// ---------------------------------------------------------------- cores
static void core1_main(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

// Push one whole frame worth of scanline pointers at core1.
static void scan_out_frame(void) {
    // Without DVI running there is no consumer, and queue_add_blocking would
    // spin forever - taking USB down with it and leaving the board needing a
    // physical BOOTSEL. Pace the loop instead so diagnostics stay reachable.
    if (!dvi_running) {
        sleep_ms(16);
        return;
    }
    uint16_t *buf = front_buffer();
    for (uint y = 0; y < FRAME_HEIGHT; ++y) {
        const uint16_t *scanline = &buf[y * FRAME_WIDTH];
        queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
        while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline))
            ;
    }
}

int main(void) {
    // DVI needs a 252MHz system clock, and the CYW43 bus timing is derived from
    // it, so the clock must be final before the wireless chip is brought up.
#if !DIAG_SKIP_OVERCLOCK
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
#endif

    setup_default_uart();
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    update_device_line();

    printf("\n[boot] pico-dvi-art client v%d (%s)\n", FIRMWARE_VERSION, DEVICE_ID);
    printf("[boot] %dx%d RGB565 in %s DVI, sys clock %lu kHz\n",
           FRAME_WIDTH, FRAME_HEIGHT,
           DVI_MODE_800X480 ? "800x480p60" : "640x480p60",
           (unsigned long)(clock_get_hz(clk_sys) / 1000));

    printf("[boot] ssid len %u fp %08lx / pass len %u fp %08lx\n",
           (unsigned)strlen(WIFI_SSID), (unsigned long)str_fingerprint(WIFI_SSID),
           (unsigned)strlen(WIFI_PASS), (unsigned long)str_fingerprint(WIFI_PASS));

    memset(framebuf, 0, sizeof(framebuf));
    draw_local_art(framebuf[0], 0);

    // Associate before DVI starts: once core1 is scanning out, the per-scanline
    // DMA interrupt starves the CYW43 driver during the WPA handshake.
    if (cyw43_arch_init()) {
        printf("[wifi] init failed - staying offline\n");
    } else {
        cyw43_arch_enable_sta_mode();
        cyw43_wifi_pm(&cyw43_state, RADIO_PM_MODE);
        printf("[wifi] connecting to %s\n", WIFI_SSID);
        // Join straight away. A scan leaves the radio hopping channels and
        // makes the association that follows it noticeably less reliable, so
        // it is only used as a diagnostic after a failure.
        wifi_join(JOIN_TIMEOUT_MS);
    }

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
#ifdef DVI_INVERT_DIFFPAIRS_OVERRIDE
    // A solid single-colour "no signal" screen on the panel (rather than the
    // expected checkered standby pattern) means the sink's TMDS clock/data
    // recovery never locked - the classic cause is the diff-pair polarity
    // not matching this carrier's wiring. Let a build override it for testing
    // without touching the shared PicoDVI pin-config header.
    dvi0.ser_cfg.invert_diffpairs = DVI_INVERT_DIFFPAIRS_OVERRIDE > 0;
#endif
#if !DIAG_SKIP_OVERCLOCK
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    multicore_launch_core1(core1_main);
    dvi_running = true;
#endif

    // From here on the board must never need a human with a BOOTSEL button: if
    // anything wedges, the watchdog restarts it. Every blocking path above feeds
    // the timer, and a whole frame of scan-out takes 16ms, so 8s is generous.
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    absolute_time_t next_retry = get_absolute_time();
    uint32_t wifi_backoff_ms = WIFI_BACKOFF_MIN_MS;
    bool scanned_once = false;
    uint32_t join_failures = 0;
    absolute_time_t next_stat  = make_timeout_time_ms(5000);
    uint32_t local_frame = 0;
    absolute_time_t fps_window = get_absolute_time();
    uint32_t fps_frames = 0;
    float fps = 0.0f;
    absolute_time_t next_render = get_absolute_time();

    while (true) {
        watchdog_update();
        if (reboot_to_bootsel) {
            printf("[cmd] rebooting into BOOTSEL for a firmware update\n");
            sleep_ms(50);
            reset_usb_boot(0, 0);
        }
        if (reboot_requested) {
            printf("[cmd] rebooting\n");
            sleep_ms(50);
            watchdog_reboot(0, 0, 0);
        }

        if (!link_up && absolute_time_diff_us(get_absolute_time(), next_retry) <= 0) {
            close_link();
            int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            printf("[net] retry: wifi status %d, ip %s\n", status, ip_string());

            bool associated = (status == CYW43_LINK_UP) || wifi_join(JOIN_TIMEOUT_MS);
            if (associated) {
                wifi_backoff_ms = WIFI_BACKOFF_MIN_MS;
                join_failures = 0;
                connect_to_server();
                // lwIP retransmits the SYN with a growing backoff, so a healthy
                // but momentarily busy network can take several seconds to
                // answer. Aborting after three would throw away connections
                // that were about to succeed.
                next_retry = make_timeout_time_ms(TCP_CONNECT_WINDOW_MS);
            } else {
                // Back off exponentially. Association requests fired every few
                // seconds get the MAC rate-limited by the AP, which is exactly
                // what turns a transient refusal into a permanent one - so give
                // the AP room to forget about us. The local animation keeps
                // running on the panel meanwhile.
                if (!scanned_once) {
                    // One diagnostic sweep so the log shows whether the AP is
                    // even in range. Repeating it would only disturb the radio
                    // before the next join.
                    scanned_once = true;
                    wifi_scan();
                }
                // An AP that has decided to refuse us often keeps refusing
                // until the client's association state is genuinely gone. A
                // full radio power-cycle is the only thing on this side that
                // clears it, so escalate to that rather than retrying forever.
                if (++join_failures >= JOIN_FAILURES_BEFORE_RESET) {
                    join_failures = 0;
                    printf("[wifi] repeated refusals - escalating\n");
                    radio_power_cycle();
                }
                printf("[net] wifi retry in %lu ms\n",
                       (unsigned long)wifi_backoff_ms);
                snprintf(status_line, sizeof(status_line),
                         "WIFI REFUSED BY AP - RETRY IN %lus",
                         (unsigned long)(wifi_backoff_ms / 1000));
                next_retry = make_timeout_time_ms(wifi_backoff_ms);
                wifi_backoff_ms *= 2;
                if (wifi_backoff_ms > WIFI_BACKOFF_MAX_MS) {
                    wifi_backoff_ms = WIFI_BACKOFF_MAX_MS;
                }
            }
        }


        if (absolute_time_diff_us(get_absolute_time(), next_render) <= 0) {
            draw_local_art(back_buffer(), local_frame++);
            front_idx ^= 1;
            fps_frames++;
            next_render = delayed_by_ms(next_render, 33);
        }

        scan_out_frame();

        if (absolute_time_diff_us(get_absolute_time(), fps_window) <= 0) {
            fps = fps_frames / 2.0f;
            fps_frames = 0;
            fps_window = make_timeout_time_ms(2000);
        }
        if (link_up && absolute_time_diff_us(get_absolute_time(), next_stat) <= 0) {
            char line[96];
            update_device_line();
            snprintf(line, sizeof(line), "TEMP %.2f\n", (double)cached_temperature);
            send_line(line);
            snprintf(line, sizeof(line), "STAT fps=%.1f drops=%lu legacy=%lu\n",
                     (double)fps, (unsigned long)rx.drops,
                     (unsigned long)rx.legacy_frames);
            send_line(line);
            next_stat = make_timeout_time_ms(5000);
        }
    }
}
