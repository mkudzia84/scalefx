/*
 * USB Host Device Detector — minimal "dumb" ESP32-S3 USB-OTG host probe.
 *
 * Isolates why the HubFX isn't enumerating a plugged-in USB CDC expander.
 * Brings up the native USB-OTG host (internal PHY, GPIO19 D-/GPIO20 D+) via
 * the shared sfx_usb EspUsbHost + vendored esp_cdc_acm, explicitly powers the
 * root port, and prints every device that connects (VID/PID) to a PLAIN-TEXT
 * UART console (the CH343 → COMxx) — DELIBERATELY NOT USB-Serial-JTAG, so the
 * console never contends with the single internal USB PHY the host needs.
 *
 * Expected good output (Pico CDC expander plugged in):
 *   >>> MOUNT addr=1 VID=2E8A PID=000A
 * Heartbeat proves the loop is alive even when nothing enumerates.
 */

#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <usb/usb_host.h>            // usb_host_lib_set_root_port_power, esp_err_to_name
#include <esp_err.h>

#include <usb/sfx_usb_host.h>        // UsbHost (= EspUsbHost on ESP32-S3)

extern "C" void app_main(void) {
    printf("\n");
    printf("==================================================\n");
    printf(" ScaleFX USB Host Device Detector (ESP32-S3)\n");
    printf(" Console on UART0 (USB-Serial-JTAG OFF) — host PHY free\n");
    printf("==================================================\n");

    UsbHost& usb = UsbHost::instance();

    usb.onMount([](uint8_t addr, uint16_t vid, uint16_t pid) {
        printf(">>> MOUNT   addr=%u VID=%04X PID=%04X\n",
               (unsigned)addr, (unsigned)vid, (unsigned)pid);
    });
    usb.onUnmount([](uint8_t addr) {
        printf("<<< UNMOUNT addr=%u\n", (unsigned)addr);
    });

    // begin() = configure root port (no host install yet); init() installs the
    // USB Host Library + CDC-ACM driver + daemon/open tasks.  BOTH are needed
    // before the root port can be powered.
    bool okBegin = usb.begin();
    bool okInit  = usb.init();
    printf("[usb] begin=%s init=%s backend=%s\n",
           okBegin ? "ok" : "FAIL", okInit ? "ok" : "FAIL", usb.backendName());

    // EspUsbHost::begin()/init() do NOT power the root port at install time
    // (only the recovery path does) — power it explicitly here so a device on
    // VBUS actually enumerates.
    esp_err_t pwr = usb_host_lib_set_root_port_power(true);
    printf("[usb] root_port_power(true) -> %s\n", esp_err_to_name(pwr));

    printf("[usb] waiting for devices...\n");

    uint32_t lastHb = 0;
    uint32_t hbCount = 0;
    while (true) {
        // Drain deferred mount/unmount events on THIS (loop) task — this is what
        // fires the onMount/onUnmount callbacks above (Rule 56 deferral).
        usb.processPendingEvents();

        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - lastHb >= 1000) {
            lastHb = now;
            printf("[hb] alive t=%lus devices=%d\n",
                   (unsigned long)(hbCount++), usb.cdcDeviceCount());
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
