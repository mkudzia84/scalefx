/*
 * PicoUsbHost — PIO-USB + TinyUSB Host Implementation (RP2040/RP2350)
 *
 * Concrete UsbHost implementation for Raspberry Pi Pico controllers using
 * PIO-USB (software USB via PIO state machine) and the TinyUSB host stack.
 *
 * Requirements:
 *   - CPU clock must be 120 MHz or 240 MHz (PIO timing constraint)
 *   - PIO1 is dedicated to USB host (PIO0 available for other uses)
 *   - Core 1 runs init() + process() loop
 *   - Core 0 calls begin() + CDC read/write via SerialBus
 *
 * Singleton: accessed via UsbHost::instance() which returns PicoUsbHost&
 * on RP2040/RP2350 builds. Do not instantiate directly.
 */

#ifndef SFX_PICO_USB_HOST_H
#define SFX_PICO_USB_HOST_H

#include "usb_host.h"  // Must come first — defines SFX_PLATFORM_PICO

#if SFX_PLATFORM_PICO

class PicoUsbHost : public UsbHost {
public:
    PicoUsbHost() = default;

    // --- Lifecycle ----------------------------------------------------------

    bool begin() override;
    bool begin(const UsbPortConfig* configs, int numPorts) override;
    void end() override;
    bool init() override;
    void process() override;

    // --- CDC Communication --------------------------------------------------

    bool cdcConnected() const override;
    int cdcAvailable(int devIndex) const override;
    int cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) override;
    int cdcReadByte(int devIndex) override;
    int cdcWrite(int devIndex, const uint8_t* data, size_t len) override;
    void cdcFlush(int devIndex) override;

    // --- Diagnostics --------------------------------------------------------

    void printStatus() const override;
    const char* backendName() const override { return "PIO-USB"; }

    // --- Internal TinyUSB callback bridge -----------------------------------

    void _onDeviceMount(uint8_t devAddr);
    void _onDeviceUnmount(uint8_t devAddr);
    void _onCdcMount(uint8_t idx);
    void _onCdcUnmount(uint8_t idx);
    void _onCdcRx(uint8_t idx);
};

#endif // SFX_PLATFORM_PICO
#endif // SFX_PICO_USB_HOST_H
