# src/CMakeLists.txt — USB Host Device Detector as ONE ESP-IDF component.
#
# Pure espidf (no Arduino).  Like the hub's src/CMakeLists.txt we compile the
# app src AND the bits of the shared lib this probe needs (sfx_usb + the
# vendored esp_cdc_acm driver) together as this single component, which
# declares the IDF REQUIRES once for everything.
#
# TRIMMED to what sfx_usb needs:
#   * sfx_usb/usb/*.cpp            EspUsbHost + shared UsbHostState
#   * esp_cdc_acm/src/*.c          vendored USB CDC-ACM host class driver
#   * (sfx_platform / sfx_serial are HEADER-ONLY here — diag_log.h is a stub
#      under -DSFX_ENABLE_DIAG_LOG=0, sfx_stream.h is header-only, so no .cpp
#      from those modules is compiled.)
#
# pico_usb_host.cpp is gated `#if SFX_PLATFORM_PICO` → compiles to an empty TU
# on ESP32, so globbing it is safe.

# SFX_LIB is at controllers/lib (CMAKE_SOURCE_DIR = this project root,
# tests/hw/usbhost_detect → ../../../controllers/lib).
set(SFX_LIB "${CMAKE_SOURCE_DIR}/../../../controllers/lib")

file(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${SFX_LIB}/sfx_usb/usb/*.cpp
    ${SFX_LIB}/esp_cdc_acm/src/*.c
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "."                                 # src/
        "${SFX_LIB}/sfx_platform"           # <platform/sfx_platform.h>, <platform/sfx_stream.h>
        "${SFX_LIB}/sfx_serial"             # <serial/diag_log.h>, <serial/wire.h>
        "${SFX_LIB}/sfx_usb"                # <usb/sfx_usb_host.h>
        "${SFX_LIB}/esp_cdc_acm/include"    # vendored USB CDC-ACM host driver (usb/cdc_acm_host.h)
    REQUIRES
        # IDF subsystems used directly by the probe + sfx_usb + esp_cdc_acm.
        usb                                 # USB Host Library (usb_host.h, usb_host_lib_set_root_port_power)
        driver
        freertos
        esp_timer
        esp_psram                           # sfxPsramCalloc selects heap_caps PSRAM on ESP32
        esp_system
        esp_hw_support
        esp_rom
        soc
)
