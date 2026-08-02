cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(pico_dvi_art_client C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# PicoDVI drives DVI from PIO, which is what makes GP8-GP15 usable on RP2350.
if(NOT DEFINED PICODVI_PATH)
    set(PICODVI_PATH "$ENV{USERPROFILE}/pico/PicoDVI")
endif()
if(NOT EXISTS "${PICODVI_PATH}/software/libdvi")
    message(FATAL_ERROR "PicoDVI not found at ${PICODVI_PATH}. Pass -DPICODVI_PATH=...")
endif()

# The carrier wires DVI to GP8/9 (clk), GP10/11, GP12/13, GP14/15, with the
# negative leg on the lower pin - exactly PicoDVI's picodvi_dvi_cfg.
set(DVI_DEFAULT_SERIAL_CONFIG "picodvi_dvi_cfg" CACHE STRING "TMDS pin mapping")

add_subdirectory(${PICODVI_PATH}/software/libdvi libdvi)

# Which firmware to build: scripture_main.c (scripture edition V2) or
# scripture_ota_main.c (V4 OTA + RSS + logo). Same target name either way,
# so the build/flash tooling is unchanged.
set(APP_MAIN scripture_ota_main.c CACHE STRING "Firmware entry: scripture_main.c or scripture_ota_main.c")

add_executable(pico_dvi_art_client ${APP_MAIN})

target_include_directories(pico_dvi_art_client PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICODVI_PATH}/software/include
)

# Panel mode. OFF = 640x480p60 with a 320x240 buffer (252 MHz, safest).
# ON  = 800x480p60 with a 400x240 buffer (295.2 MHz, fills more of the 10.1"
# glass). Keep in sync with the desktop launcher and tools/build_firmware.py.
option(DVI_MODE_800X480 "Use 800x480p60 / 400x240 instead of 640x480p60 / 320x240" OFF)

# A solid single-colour "no signal" screen (instead of the expected checkered
# standby pattern) means the TMDS receiver never locked. That is the classic
# symptom of the diff-pair polarity not matching the carrier's wiring, so make
# it a one-flag toggle for testing without editing the shared pin-config
# header. 1 matches this carrier's documented wiring; 0 is diagnostic.
set(DVI_INVERT_DIFFPAIRS 1 CACHE STRING "TMDS diff-pair polarity (1=carrier default, 0=diagnostic)")

target_compile_definitions(pico_dvi_art_client PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=2
    DVI_N_TMDS_BUFFERS=3
    DVI_MONOCHROME_TMDS=0
    DVI_MODE_800X480=$<BOOL:${DVI_MODE_800X480}>
    DVI_INVERT_DIFFPAIRS_OVERRIDE=${DVI_INVERT_DIFFPAIRS}
    # Give a USB console a few seconds to attach so the boot log - which
    # explains the selected display mode - is not lost on a headless appliance.
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=8000
)

target_link_libraries(pico_dvi_art_client
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    hardware_pio
    hardware_vreg
    libdvi
)

# Wi-Fi note board. Pass the credentials as build flags so they are never
# committed: cmake -DWIFI_SSID="..." -DWIFI_PASSWORD="..." (or use
# tools/build_firmware.py --wifi-ssid ... --wifi-pass ...). When a SSID is
# set, the scripture firmware joins the network and serves a note page on
# port 80 that any phone or PC browser can use.
set(WIFI_SSID "" CACHE STRING "Wi-Fi SSID for the note server (empty = Wi-Fi off)")
set(WIFI_PASSWORD "" CACHE STRING "Wi-Fi password for the note server")
if(NOT WIFI_SSID STREQUAL "")
    target_compile_definitions(pico_dvi_art_client PRIVATE
        NOTE_WIFI_ENABLED=1
        WIFI_SSID="${WIFI_SSID}"
        WIFI_PASSWORD="${WIFI_PASSWORD}"
        CYW43_HOST_NAME="ionity-scripture"
        # The system clock runs at 252 MHz for DVI; slow the cyw43 PIO SPI
        # divider down so the radio link stays within spec.
        CYW43_PIO_CLOCK_DIV_INT=3
        CYW43_PIO_CLOCK_DIV_FRAC=0
    )
    target_link_libraries(pico_dvi_art_client pico_cyw43_arch_lwip_poll pico_lwip_mdns hardware_flash)
endif()

# Logs go to USB serial so a single cable does both power and diagnostics.
pico_enable_stdio_usb(pico_dvi_art_client 1)
pico_enable_stdio_uart(pico_dvi_art_client 0)

pico_add_extra_outputs(pico_dvi_art_client)

# Diagnostic build: -DDIAG_SKIP_OVERCLOCK=1 leaves the system clock alone and
# never starts DVI, which isolates wireless problems from display problems.
if(DIAG_SKIP_OVERCLOCK)
    target_compile_definitions(pico_dvi_art_client PRIVATE DIAG_SKIP_OVERCLOCK=1)
endif()
