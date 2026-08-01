/*
 * Template only. tools/build_firmware.py generates the real wifi_config.h
 * from pico_firmware/device_secrets.py, and that generated file is
 * git-ignored - this repo is public so credentials must never be committed.
 */
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASS        "YOUR_WIFI_PASSWORD"

#define SERVER_IP        "192.168.1.10"
#define SERVER_PORT      5001

#define DEVICE_ID        "pico-dvi-01"
#define FIRMWARE_VERSION 1

#endif /* WIFI_CONFIG_H */
