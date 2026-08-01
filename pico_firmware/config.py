# Device configuration for the Pico W art client.
#
# Credentials live in device_secrets.py (git-ignored, never OTA'd). Copy
# device_secrets.example.py to device_secrets.py and edit it once over USB;
# everything below only holds tuning values that are safe to publish.

try:
    import device_secrets as _secrets
except ImportError:  # file missing -> fall back to the placeholders below
    _secrets = None


def _secret(name, default):
    return getattr(_secrets, name, default) if _secrets else default


# --- Wi-Fi ---
WIFI_SSID = _secret("WIFI_SSID", "YOUR_WIFI_NAME")
WIFI_PASS = _secret("WIFI_PASS", "YOUR_WIFI_PASSWORD")
WIFI_TIMEOUT_S = 20
WIFI_COUNTRY = "BE"  # regulatory domain, e.g. BE, NL, DE, GB, US

# --- Frame server (the PC running pc_server/server.py) ---
SERVER_IP = _secret("SERVER_IP", "192.168.1.10")
SERVER_PORT = _secret("SERVER_PORT", 5001)
SOCKET_TIMEOUT_S = 10
RECONNECT_DELAY_S = 3

# --- Display ---
WIDTH = 400
HEIGHT = 240
BYTES_PER_PIXEL = 2  # RGB565
FRAME_SIZE = WIDTH * HEIGHT * BYTES_PER_PIXEL  # 192000

# --- Telemetry (RP2040 on-chip temperature shown as "MCU" on the HUD) ---
TELEMETRY_INTERVAL_S = 5
DEVICE_ID = _secret("DEVICE_ID", "pico-dvi-01")

# --- OTA over GitHub ---
GITHUB_USER = _secret("GITHUB_USER", "YOUR_USERNAME")
GITHUB_REPO = _secret("GITHUB_REPO", "pico-dvi-art-server")
GITHUB_BRANCH = _secret("GITHUB_BRANCH", "main")
GITHUB_PATH = "pico_firmware"
RAW_BASE = "https://raw.githubusercontent.com/{}/{}/{}/{}/".format(
    GITHUB_USER, GITHUB_REPO, GITHUB_BRANCH, GITHUB_PATH
)
OTA_CHECK_MINUTES = 60  # 0 disables the periodic check (boot check still runs)
OTA_ON_BOOT = True
