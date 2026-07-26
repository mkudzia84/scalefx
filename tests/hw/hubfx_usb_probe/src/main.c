/*
 * hubfx_usb_probe — minimal USB-OTG HOST enumeration probe (pure ESP-IDF).
 *
 * Installs the USB host library, powers the root port, and prints VID/PID
 * for every device that mounts (incl. devices behind the on-board hub chip
 * — HUBS_SUPPORTED is on).  A 2 s heartbeat proves the probe is alive even
 * with nothing plugged in.  Console: UART0 @ 115200, plain text.
 *
 * Interpretation:
 *   "DEVICE address=N VID=xxxx PID=xxxx"  -> USB hardware path is GOOD;
 *                                             a detection bug is in the
 *                                             production firmware.
 *   heartbeats only, no DEVICE lines       -> hardware (cable / hub chip /
 *                                             VBUS power / connector).
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

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "usb_host_install OK");

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
        ESP_LOGI(TAG, "heartbeat uptime=%llds devices_seen=%d",
                 esp_timer_get_time() / 1000000, s_devices_seen);
    }
}
