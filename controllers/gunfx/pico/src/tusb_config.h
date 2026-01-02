/*
 * TinyUSB Configuration for gunfx_pico
 * Customizes USB VID/PID and device descriptor
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// USB Device Descriptor Configuration
// =====================================================================

// gunfx_pico USB Identifiers
#define USB_VID                    0x2e8a  // Raspberry Pi Foundation
#define USB_PID                    0x0180  // gunfx_pico (community range 0x0100-0x01FF)

// Device descriptor strings
#define USB_MANUFACTURER            "MSB (Marcin Scale Builds)"
#define USB_PRODUCT_NAME            "GunFX"
// Note: USB_SERIAL_NUMBER uses Pico unique ID - see usb_descriptors.c

// =====================================================================
// Device Configuration
// =====================================================================

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

// Descriptor Configuration (removed unused macro)

// String descriptor support
#define CFG_TUSB_DEBUG              0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
