/*
 * TinyUSB Configuration for HubFX Pico
 * 
 * Enables both USB Device (native USB for Serial debug) and
 * USB Host (PIO-USB for connecting to GunFX boards)
 * 
 * This configuration uses:
 *   - Native USB (roothub port 0): Device mode - Serial debug output
 *   - PIO-USB (roothub port 1): Host mode - Connect to GunFX/peripherals
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// Common Configuration
// =====================================================================

// Use Pico SDK OS abstraction
#define CFG_TUSB_OS                 OPT_OS_PICO

// Board support - Raspberry Pi Pico
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT            0   // Native USB for device
#endif

// Enable debug output (0 = off, 1 = error, 2 = warning, 3 = info)
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0
#endif

// Memory section and alignment
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

// =====================================================================
// USB Device Configuration (Native USB - Port 0)
// =====================================================================

// Enable device stack on native USB
#define CFG_TUD_ENABLED             1

// Device endpoint 0 size
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE      64
#endif

// -----------------------------------------------
// Device Class Drivers
// -----------------------------------------------

// CDC (Serial) - for debug output
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256
#define CFG_TUD_CDC_EP_BUFSIZE      64

// HID - not used
#define CFG_TUD_HID                 0

// MSC (Mass Storage) - not used
#define CFG_TUD_MSC                 0

// MIDI - not used  
#define CFG_TUD_MIDI                0

// Vendor - not used
#define CFG_TUD_VENDOR              0

// =====================================================================
// USB Device Descriptor
// =====================================================================

// HubFX USB Identifiers (using Raspberry Pi community range)
#define USB_VID                     0x2e8a  // Raspberry Pi Foundation
#define USB_PID                     0x0181  // hubfx_pico (community range 0x0100-0x01FF)

// Device descriptor strings
#define USB_MANUFACTURER            "MSB (Marcin Scale Builds)"
#define USB_PRODUCT_NAME            "hubfx"
#define USB_SERIAL_NUMBER           "hubfx001"

// =====================================================================
// USB Host Configuration (PIO-USB - Port 1)
// =====================================================================

// Enable host stack with PIO-USB
#define CFG_TUH_ENABLED             1
#define CFG_TUH_RPI_PIO_USB         1

// Max devices (excluding hub device itself)
// With hub support, we can connect multiple GunFX boards
#define CFG_TUH_DEVICE_MAX          4

// Size of buffer for enumeration descriptors
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// -----------------------------------------------
// Host Class Drivers
// -----------------------------------------------

// HUB support - ENABLED for connecting multiple GunFX boards
// through a single USB host port (since we only have 1 PIO available)
#define CFG_TUH_HUB                 1
#define CFG_TUH_HUB_PORT_MAX        4   // Support up to 4 hub ports

// CDC Host - for communicating with GunFX boards
#define CFG_TUH_CDC                 4   // Support up to 4 CDC devices
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM  1

// HID Host - disabled
#define CFG_TUH_HID                 0
#define CFG_TUH_HID_EPIN_BUFSIZE    0
#define CFG_TUH_HID_EPOUT_BUFSIZE   0

// MSC Host - disabled
#define CFG_TUH_MSC                 0

// Vendor Host - disabled
#define CFG_TUH_VENDOR              0

// =====================================================================
// PIO-USB Specific Configuration
// =====================================================================

// IMPORTANT: Only ONE USB host port is possible!
// - PIO0 is used by I2S for audio output
// - PIO1 is used by USB host
// To connect multiple devices, use a USB hub on this single port.

// Default D+ pin for PIO-USB (D- = D+ + 1)
#ifndef PIO_USB_DP_PIN_DEFAULT
#define PIO_USB_DP_PIN_DEFAULT      2   // GP2 for D+, GP3 for D-
#endif

// PIO configuration - MUST use PIO1 (PIO0 is used by I2S audio)
#ifndef PIO_USB_TX_DEFAULT
#define PIO_USB_TX_DEFAULT          1   // Use PIO1 for USB TX
#endif

#ifndef PIO_USB_RX_DEFAULT  
#define PIO_USB_RX_DEFAULT          1   // Use PIO1 for USB RX
#endif

// State machine assignments
#ifndef PIO_SM_USB_TX_DEFAULT
#define PIO_SM_USB_TX_DEFAULT       0
#endif

#ifndef PIO_SM_USB_RX_DEFAULT
#define PIO_SM_USB_RX_DEFAULT       1
#endif

#ifndef PIO_SM_USB_EOP_DEFAULT
#define PIO_SM_USB_EOP_DEFAULT      2
#endif

// DMA channel for USB TX
#ifndef PIO_USB_DMA_TX_DEFAULT
#define PIO_USB_DMA_TX_DEFAULT      0
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
