# Custom src/CMakeLists.txt — the HubFX firmware as ONE ESP-IDF component.
#
# Pure espidf (no Arduino).  Under the old framework=arduino path the shared
# library at ../../lib came in via PlatformIO's lib_extra_dirs LDF; pure espidf
# has no equivalent that also carries the IDF component REQUIRES the lib needs
# (driver/*, esp_psram, fatfs, …).  So we compile the firmware src AND the
# shared lib together as this single component, which declares the REQUIRES
# once for everything.  Platform-gated lib sources (pico/* under
# `#if SFX_PLATFORM_PICO`, esp32/* under `#if SFX_PLATFORM_ESP32`) compile to
# empty TUs on the wrong target, so the broad glob is safe.

set(SFX_LIB "${CMAKE_SOURCE_DIR}/../../lib")

file(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${SFX_LIB}/*.cpp
    ${SFX_LIB}/*.c
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "."                                 # src/  (config/*, effects/*, …)
        "${SFX_LIB}/sfx_platform"           # <platform/sfx_platform.h>, <platform/sfx_stream.h>
        "${SFX_LIB}/sfx_serial"             # <serial/core/core.h>, <serial/wire.h>, …
        "${SFX_LIB}/sfx_board"              # <server/board_of.h>, <roles/…>, <motion/…>
        "${SFX_LIB}/sfx_peripherals"        # <i2c/…>, <gpio/…>, <ports/…>, <pwm/…>, <led/…>, <power/…>, <rx_input/…>, <jeti_ex/…>, <indicators/…>
        "${SFX_LIB}/sfx_audio"              # <audio/…>, <codec/…>
        "${SFX_LIB}/sfx_storage"            # <storage/…>, <server/storage_service.h>
        "${SFX_LIB}/sfx_config"             # <config/…>, <server/multi_config_server.h>
        "${SFX_LIB}/sfx_usb"                # <usb/…>
    REQUIRES
        # Managed components (src/idf_component.yml).
        espressif__esp-dsp                  # Xtensa LX7 SIMD audio mix kernel
        chmorgan__esp-libhelix-mp3          # fixed-point Helix MP3 decoder
        # IDF subsystems used directly by the firmware + shared lib.
        driver
        esp_driver_i2c
        esp_driver_mcpwm
        esp_driver_uart
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2s
        esp_timer
        esp_psram
        esp_adc
        esp_system
        esp_hw_support
        esp_rom
        esp_app_format
        soc
        fatfs
        vfs
        esp_littlefs
        usb
        sdmmc
)
