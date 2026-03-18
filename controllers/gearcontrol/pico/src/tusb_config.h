/*
 * TinyUSB Configuration for gearcontrol_pico
 * Explicitly configures USB as DEVICE mode (CDC serial).
 * This Pico acts as a USB device — the ESP32-S3 HubFX is the USB Host.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// USB Device Descriptor Configuration
// =====================================================================

// gearcontrol_pico USB Identifiers
#define USB_VID                    0x2e8a  // Raspberry Pi Foundation
#define USB_PID                    0x0182  // gearcontrol_pico (community range 0x0100-0x01FF)

// Device descriptor strings
#define USB_MANUFACTURER            "MSB (Marcin Scale Builds)"
#define USB_PRODUCT_NAME            "GearControl"
// Note: USB_SERIAL_NUMBER uses Pico unique ID - see usb_descriptors.c

// =====================================================================
// Device Configuration
// =====================================================================

// CRITICAL: Explicitly set to DEVICE mode (not Host)
#define CFG_TUSB_RHPORT0_MODE      OPT_MODE_DEVICE
#define CFG_TUSB_OS                OPT_OS_PICO

// Enable Full-Speed (12 Mbps) - Pico supports FS only
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED

// CDC (Communication Device Class) - Serial over USB
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE      (TUD_OPT_HIGH_SPEED ? 512 : 64)

// HID (Optional - disabled for this device)
#define CFG_TUD_HID                 0

// Mass Storage (Optional - disabled)
#define CFG_TUD_MSC                 0

// Misc device class configs
#define CFG_TUD_MISC                0

// =====================================================================
// Common Configuration
// =====================================================================

// Endpoint 0 size
#define CFG_TUD_ENDPOINT0_SIZE      64

// String descriptor support
#define CFG_TUSB_DEBUG              0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
