/*
 * hubfx_usb_probe — minimal USB-OTG HOST enumeration probe (pure ESP-IDF).
 *
 * Installs the USB host library, powers the root port, and prints VID/PID
 * for every device that mounts (incl. devices behind the on-board hub chip
 * — HUBS_SUPPORTED is on).  A 2 s heartbeat proves the probe is alive even
 * with nothing plugged in.  Console: UART0 @ 115200, plain text.
 *
 * Interpretation (2026-07-26 hub-init instrumentation):
 *   The IDF hub driver HIDES hub-class devices from clients, so the on-board
 *   hub chip (U41) never produces a client "DEVICE" line even when healthy.
 *   Three signals now expose it anyway:
 *
 *   1. "ENUM: ..." lines — the enumeration-filter callback fires for EVERY
 *      device the root port enumerates, U41 included.  A healthy board
 *      prints an ENUM line with bDeviceClass=09 (hub) within ~1 s of boot.
 *        ENUM line, class=09        -> U41 powered, strapped, linked upstream.
 *        NO ENUM line at all        -> the ROOT port never saw a connect:
 *                                      upstream D+/D- path (R19/R20), U41
 *                                      AVDD, RESET_HUB, or X2 crystal dead —
 *                                      match against ISSUES.md §6 bench plan.
 *   2. IDF DEBUG logs from tags HUB / EXT_HUB / EXT_PORT / HCD — root-port
 *      connect/reset, hub-descriptor parse, downstream port power/connects.
 *   3. Heartbeat now polls usb_host_lib_info(): num_devices counts what the
 *      host library tracks INTERNALLY (hubs included), so "devs=1" with no
 *      client DEVICE line = hub enumerated, nothing plugged downstream.
 *
 *   "DEVICE address=N VID=xxxx PID=xxxx"  -> full path GOOD (a device behind
 *                                             the hub enumerated + reported).
 *
 * Task stacks are generous (8 KB) — enumeration runs entirely on the
 * daemon task stack and 4 KB overflows on live hot-plug (2026-06-07 lore).
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

static const char *TAG = "usbprobe";

static usb_host_client_handle_t s_client;
static volatile int s_devices_seen = 0;
static volatile int s_enums_seen   = 0;

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
// Fires for EVERY device the host stack enumerates — including the on-board
// hub chip U41, which the hub driver hides from the client API.  This is the
// probe's "U41 is alive" line: bDeviceClass 0x09 = hub.
static bool enum_filter_cb(const usb_device_desc_t *desc, uint8_t *bConfigurationValue)
{
    s_enums_seen++;
    const bool is_hub = (desc->bDeviceClass == 0x09);
    ESP_LOGI(TAG, "ENUM: VID=%04x PID=%04x bcdUSB=%04x class=%02x proto=%02x maxp0=%u  (#%d)%s",
             desc->idVendor, desc->idProduct, desc->bcdUSB,
             desc->bDeviceClass, desc->bDeviceProtocol, desc->bMaxPacketSize0,
             s_enums_seen,
             is_hub ? "  <-- HUB CHIP (U41 path) DETECTED" : "");
    (void)bConfigurationValue;   // keep the device's default configuration
    return true;                 // never veto — we only observe
}
#endif

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        const uint8_t addr = msg->new_dev.address;
        usb_device_handle_t dev = NULL;
        esp_err_t err = usb_host_device_open(s_client, addr, &dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NEW_DEV addr=%u but open failed: %s", addr, esp_err_to_name(err));
            return;
        }
        const usb_device_desc_t *desc = NULL;
        err = usb_host_get_device_descriptor(dev, &desc);
        if (err == ESP_OK && desc) {
            s_devices_seen++;
            ESP_LOGI(TAG, "DEVICE address=%u VID=%04x PID=%04x bcdDevice=%04x class=%02x  (#%d)",
                     addr, desc->idVendor, desc->idProduct, desc->bcdDevice,
                     desc->bDeviceClass, s_devices_seen);
        } else {
            ESP_LOGE(TAG, "NEW_DEV addr=%u but descriptor read failed: %s", addr, esp_err_to_name(err));
        }
        usb_host_device_close(s_client, dev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "DEVICE GONE (handle=%p)", msg->dev_gone.dev_hdl);
    }
}

static void daemon_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) ESP_LOGI(TAG, "lib event: NO_CLIENTS");
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)   ESP_LOGI(TAG, "lib event: ALL_FREE");
    }
}

static void client_task(void *arg)
{
    for (;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

void app_main(void)
{
    printf("\n=== hubfx_usb_probe ===\n");
    printf("USB-OTG HOST enumeration probe. Plug a board into a hub host port.\n");

    // Surface the USB stack's own root-port / hub-driver diagnostics — these
    // tags log the root-port connect edge, hub-descriptor parse and
    // downstream port power that the client API never shows.  (Max level is
    // DEBUG via sdkconfig; VERBOSE would need CONFIG_LOG_MAXIMUM_LEVEL bump.)
    esp_log_level_set("HUB",      ESP_LOG_DEBUG);
    esp_log_level_set("EXT_HUB",  ESP_LOG_DEBUG);
    esp_log_level_set("EXT_PORT", ESP_LOG_DEBUG);
    esp_log_level_set("HCD",      ESP_LOG_DEBUG);
    esp_log_level_set("USBH",     ESP_LOG_DEBUG);
    esp_log_level_set("USB HOST", ESP_LOG_DEBUG);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
        .enum_filter_cb = enum_filter_cb,
#endif
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "usb_host_install OK  (enum-filter %s)",
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
             "armed — expect an ENUM line for the U41 hub chip within ~1 s"
#else
             "DISABLED — rebuild with CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK"
#endif
    );

    // Root port power — some IDF versions gate VBUS on this; harmless if
    // already on.  Log the result rather than aborting (API may return
    // ESP_ERR_INVALID_STATE when power switching isn't configured).
    esp_err_t perr = usb_host_lib_set_root_port_power(true);
    ESP_LOGI(TAG, "root_port_power(true) -> %s", esp_err_to_name(perr));

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &s_client));
    ESP_LOGI(TAG, "client registered");

    xTaskCreatePinnedToCore(daemon_task, "usb_daemon", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_client", 8192, NULL, 5, NULL, 0);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        // usb_host_lib_info counts what the host library tracks INTERNALLY —
        // hub devices included.  libdevs=1 with client devices_seen=0 means
        // "U41 enumerated, nothing plugged downstream" (healthy idle board);
        // libdevs=0 forever means the root port never saw a connect.
        usb_host_lib_info_t info = {0};
        esp_err_t ierr = usb_host_lib_info(&info);
        ESP_LOGI(TAG, "heartbeat uptime=%llds client_devices=%d enums=%d libdevs=%d clients=%d%s",
                 esp_timer_get_time() / 1000000, s_devices_seen, s_enums_seen,
                 (ierr == ESP_OK) ? info.num_devices : -1,
                 (ierr == ESP_OK) ? info.num_clients : -1,
                 (s_enums_seen == 0) ? "  [no ENUM yet -> root port has never connected]" : "");
    }
}
