# sfx_usb — USB Host Abstraction

USB Host CDC communication for ScaleFX client controllers (HubFX). Provides a platform-independent abstract interface with compile-time backend selection.

**Client-only** — excluded from server controller builds via `SCALEFX_SERVER` guard.

## Architecture

```
                        ┌─────────────────────┐
                        │   UsbHost (abstract) │  ← Platform factory singleton
                        │   usb_host.h/.cpp    │     UsbHost::instance()
                        └──────────┬──────────┘
                   ┌───────────────┴───────────────┐
                   │                               │
        ┌──────────┴──────────┐         ┌──────────┴──────────┐
        │    PicoUsbHost      │         │    EspUsbHost        │
        │ pico_usb_host.h/.cpp│         │ esp_usb_host.h/.cpp  │
        │                     │         │                      │
        │ Backend: PIO-USB    │         │ Backend: HW USB-OTG  │
        │ Stack:   TinyUSB    │         │ Stack:   ESP-IDF USB  │
        │ Arch:    RP2040/2350│         │ Arch:    ESP32-S3     │
        └─────────────────────┘         └──────────────────────┘

        ┌─────────────────────┐
        │    UsbRegistry      │  ← Tracks connected slave controllers
        │   usb_registry.h    │     SlaveType → BusClient* mapping
        │     (singleton)     │
        └─────────────────────┘
```

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `usb_host.h` | 252 | Abstract `UsbHost` interface + types + factory singleton declaration |
| `usb_host.cpp` | 95 | Base class helpers (device tracking) + platform factory (`instance()`) |
| `pico_usb_host.h` | 65 | `PicoUsbHost` class declaration (RP2040/RP2350) |
| `pico_usb_host.cpp` | 301 | PIO-USB + TinyUSB host implementation |
| `esp_usb_host.h` | 98 | `EspUsbHost` class declaration (ESP32-S3) |
| `esp_usb_host.cpp` | 551 | HW USB-OTG + ESP-IDF CDC-ACM driver implementation |
| `usb_registry.h` | 244 | `UsbRegistry` singleton — tracks connected slave controllers |

## UsbHost — Abstract Interface

The `UsbHost` class defines a platform-independent API for USB Host CDC communication. A compile-time factory singleton (`UsbHost::instance()`) returns the platform-appropriate concrete subclass.

### Lifecycle

| Phase | Method | Context | Purpose |
|-------|--------|---------|---------|
| Configure | `begin()` | Core 0 / setup | Store port config, validate platform constraints |
| Initialize | `init()` | Core 1 (Pico) / USB task (ESP32) | Install USB stack, create tasks |
| Run | `process()` | Core 1 loop (Pico) / no-op (ESP32) | Poll USB events, dispatch RX data |
| Teardown | `end()` | Any | Close devices, uninstall stack |

### CDC Communication

All CDC methods operate by device index (0-based, max 4 simultaneous devices):

| Method | Returns | Purpose |
|--------|---------|---------|
| `cdcConnected()` | `bool` | Any device connected and ready? |
| `cdcDeviceCount()` | `int` | Number of tracked CDC devices |
| `cdcAvailable(idx)` | `int` | Bytes available to read from device |
| `cdcRead(idx, buf, len)` | `int` | Read data (-1 on error) |
| `cdcReadByte(idx)` | `int` | Read single byte (-1 on error) |
| `cdcWrite(idx, data, len)` | `int` | Write data (-1 on error) |
| `cdcFlush(idx)` | `void` | Flush pending writes |

### Callbacks

| Callback | Signature | Trigger |
|----------|-----------|---------|
| `onMount` | `void(uint8_t devAddr, uint16_t vid, uint16_t pid)` | USB device enumerated |
| `onUnmount` | `void(uint8_t devAddr)` | USB device disconnected |
| `onCdcReceive` | `void(uint8_t devAddr, const uint8_t* data, size_t len)` | CDC data received |

### Device Info & Diagnostics

| Method | Purpose |
|--------|---------|
| `getCdcDevice(idx)` | Get `CdcDeviceInfo` struct (address, VID/PID, state) |
| `printStatus()` | Log full USB host status via DiagLog |
| `isReady()` | `true` when initialized AND task running |
| `stats()` | Traffic counters (mounted/unmounted, bytes TX/RX) |
| `backendName()` | `"PIO-USB"` or `"HW USB-OTG"` |

### Data Types

```cpp
enum class UsbDeviceState : uint8_t {
    Disconnected, Connected, Mounted, Ready
};

struct UsbPortConfig {
    bool enabled;       // Port active
    uint8_t dp_pin;     // D+ GPIO (Pico only, ignored on ESP32-S3)
    char name[32];      // Human-readable port name
};

struct CdcDeviceInfo {
    bool connected;             // USB physically attached
    uint8_t dev_addr;           // USB device address
    uint8_t itf_num;            // CDC interface/slot number
    uint16_t vid, pid;          // USB Vendor/Product ID
    UsbDeviceState state;       // Current device state
    uint8_t port_id;            // USB port index
};

struct UsbHostStats {
    uint32_t devices_mounted;   // Total mount events
    uint32_t devices_unmounted; // Total unmount events
    uint32_t bytes_sent;        // Total TX bytes
    uint32_t bytes_received;    // Total RX bytes
};
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `USB_HOST_MAX_PORTS` | 1 | Maximum USB host ports per board |
| `USB_HOST_MAX_CDC_DEVICES` | 4 | Maximum simultaneous CDC devices |
| `USB_HOST_PORT_0_DP_DEFAULT` | 2 | Default D+ pin (Pico PIO-USB) |

## PicoUsbHost — PIO-USB Backend (RP2040/RP2350)

Software USB implementation using the RP2040/RP2350 PIO (Programmable I/O) state machine and the TinyUSB host stack.

### Requirements

- **CPU clock:** 120 MHz or 240 MHz (PIO timing constraint — auto-adjusted in `begin()`)
- **PIO1:** Dedicated to USB Host (PIO0 available for other uses)
- **Dual-core:** `begin()` on Core 0, `init()` + `process()` on Core 1
- **GPIO:** Configurable D+ pin (default GP2), D- = D+ + 1 (GP3)

### Implementation Details

| Aspect | Detail |
|--------|--------|
| USB stack | TinyUSB host mode via `tuh_*` API |
| Line coding | 1 Mbps, 8N1 (ScaleFX protocol) |
| RX path | `tuh_cdc_read()` polled in `process()` → fire `_cdcRxCallback` |
| TX path | `tuh_cdc_write()` + `tuh_cdc_write_flush()` |
| Event bridge | TinyUSB C callbacks → `PicoUsbHost::_on*()` methods via singleton cast |

### TinyUSB Callback Bridge

TinyUSB requires global C functions. These bridge into the singleton:

```
tuh_mount_cb()       →  PicoUsbHost::_onDeviceMount()
tuh_umount_cb()      →  PicoUsbHost::_onDeviceUnmount()
tuh_cdc_mount_cb()   →  PicoUsbHost::_onCdcMount()
tuh_cdc_umount_cb()  →  PicoUsbHost::_onCdcUnmount()
tuh_cdc_rx_cb()      →  PicoUsbHost::_onCdcRx()
```

## EspUsbHost — HW USB-OTG Backend (ESP32-S3)

Native hardware USB-OTG implementation using ESP-IDF's USB Host Library and the official CDC-ACM class driver.

### Requirements

- **ESP32-S3:** Hardware USB-OTG peripheral (fixed GPIO19 D-, GPIO20 D+)
- **CDC-ACM component:** `espressif/usb_host_cdc_acm ^2.0.0` (managed component)
- **`cdc_on_boot` disabled:** USB-OTG shared between device/host mode, must be in host mode
- **Serial debug:** Via UART0 (USB-UART bridge), NOT USB CDC

### Implementation Details

| Aspect | Detail |
|--------|--------|
| USB stack | ESP-IDF USB Host Library (`usb/usb_host.h`) |
| CDC driver | `usb/cdc_acm_host.h` — handles enumeration + data |
| Daemon task | FreeRTOS task running `usb_host_lib_handle_events()` (Core 0, priority 2) |
| CDC driver task | Internal to CDC-ACM component (Core 0, priority 5) |
| Line coding | 1 Mbps, 8N1, DTR+RTS asserted |
| RX path | CDC data callback → FreeRTOS `StreamBuffer` (4 KB per device) → `cdcRead()` |
| TX path | `cdc_acm_host_data_tx_blocking()` with 100 ms timeout |
| `process()` | No-op — fully event-driven via FreeRTOS tasks |
| Event bridge | Static C callbacks → `EspUsbHost::_handle*()` via singleton cast |

### Per-Device Slot Tracking

Each connected CDC device gets a `CdcSlot`:
- `cdcHandle` — opaque `cdc_acm_dev_hdl_t` for the CDC-ACM driver
- `rxStream` — FreeRTOS `StreamBufferHandle_t` (4 KB ring buffer for received data)
- `devAddr` — sequential device address (1, 2, 3...) assigned at mount
- `open` — slot active flag

### Graceful Degradation

If the `usb_host_cdc_acm` managed component is not installed, the code compiles but `init()` returns `false` with a clear error message. The `__has_include` guard enables this without breaking the build.

## UsbRegistry — Connected Slave Tracker

Platform-independent singleton that maintains a registry of connected USB slave controllers. Decoupled from `UsbHost` — provides lookup and state management without any USB stack dependency.

### SlaveType Enumeration

```cpp
enum class SlaveType : uint8_t {
    Unknown      = 0,
    GunFX        = 1,
    LightFX      = 2,
    GearControl  = 3,
    COUNT        = 4
};
```

### SlaveEntry Structure

```cpp
struct SlaveEntry {
    SlaveType  type;       // Controller type
    int        usbIndex;   // USB CDC device index in UsbHost (-1 if unassigned)
    bool       connected;  // USB physically connected
    bool       ready;      // INIT handshake completed
    BusClient* client;     // Protocol client instance (not owned)
};
```

### API

| Method | Purpose |
|--------|---------|
| `registerSlave(type, client, idx)` | Register a slave type with its BusClient and USB index |
| `setConnected(type, bool)` | Mark USB connection state (disconnect clears ready) |
| `setReady(type, bool)` | Mark INIT handshake state |
| `getClient(type)` | Get BusClient* if registered AND ready (else nullptr) |
| `find(type)` | Find SlaveEntry by type |
| `findByUsbIndex(idx)` | Find SlaveEntry by USB device index |
| `resetAll()` | Clear all connection/ready state (keep registrations) |
| `count()` / `operator[]` | Iterate over entries |

### Ownership Model

The registry does **not** own BusClient instances. Client objects (e.g., `GunFxClient`, `LightFxClient`) are allocated by the controller firmware and their pointers are registered here. The registry provides lookup only.

### State Transitions

```
registerSlave() → [type, client, usbIndex set]
    ↓
setConnected(true) → USB device physically attached
    ↓
setReady(true) → INIT handshake completed, getClient() returns non-null
    ↓
setConnected(false) → USB disconnect (ready auto-cleared)
```

## Dependencies

| Dependency | Reason |
|------------|--------|
| `sfx_platform` | `sfx_platform.h` (platform macros), `diag_log.h` (logging) |
| `Arduino` | `<Arduino.h>` (base types) |
| TinyUSB | Pico backend: PIO-USB host stack |
| ESP-IDF USB Host | ESP32 backend: `usb_host.h` + `cdc_acm_host.h` |
| FreeRTOS | ESP32 backend: tasks, stream buffers |

**No dependency on:** sfx_serial, sfx_peripherals, sfx_audio, sfx_storage, sfx_server.

## Usage

```cpp
#include <usb/usb_host.h>
#include <usb/usb_registry.h>

// Core 0 setup
UsbHost& usb = UsbHost::instance();
usb.begin();
usb.onMount([](uint8_t addr, uint16_t vid, uint16_t pid) {
    // Handle new device — identify slave type, register in UsbRegistry
});
usb.onUnmount([](uint8_t devAddr) {
    // Handle disconnect — update UsbRegistry
});

// Core 1 (Pico) or USB task (ESP32)
usb.init();
while (true) {
    usb.process();  // Polled on Pico, no-op on ESP32
}
```
