# Device secrets - COPY THIS FILE TO device_secrets.py AND EDIT IT.
#
#   cp device_secrets.example.py device_secrets.py
#
# device_secrets.py is git-ignored and excluded from the OTA manifest, so your
# Wi-Fi password never reaches GitHub and is never overwritten by an update.
# This example file stays in the repo as a template only.

WIFI_SSID = "YOUR_WIFI_NAME"
WIFI_PASS = "YOUR_WIFI_PASSWORD"

# Local IP of the PC running pc_server/server.py
SERVER_IP = "192.168.1.10"
SERVER_PORT = 5001

# Friendly name for this board, shown in the server log
DEVICE_ID = "pico-dvi-01"

# GitHub repository holding pico_firmware/ (must be public for raw OTA)
GITHUB_USER = "YOUR_USERNAME"
GITHUB_REPO = "pico-dvi-art-server"
GITHUB_BRANCH = "main"
