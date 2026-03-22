/*
 * Pico ISR PPM Policy — GPIO interrupt edge-timing PPM capture
 *
 * Uses a single GPIO interrupt to measure pulse widths via edge timing.
 * Compatible with RP2040 and RP2350 (Arduino-Pico framework).
 *
 * Do not include directly — included from ppm_input.h via platform guard.
 */

#ifndef SFX_PICO_ISR_PPM_POLICY_H
#define SFX_PICO_ISR_PPM_POLICY_H

#if SFX_PLATFORM_PICO

#include <Arduino.h>
#include <atomic>
#include "../rx_common.h"
#include "platform/sfx_platform.h"

class PicoIsrPpmPolicy {
public:
    PicoIsrPpmPolicy() = default;
    ~PicoIsrPpmPolicy() { end(); }

    PicoIsrPpmPolicy(const PicoIsrPpmPolicy&) = delete;
    PicoIsrPpmPolicy& operator=(const PicoIsrPpmPolicy&) = delete;

    /**
     * @brief Start GPIO interrupt for PPM capture
     * @param pin         GPIO connected to PPM output of receiver
     * @param risingEdge  true = rising edge marks channel boundary
     * @return true if interrupt attached
     */
    bool begin(int pin, bool risingEdge);

    /**
     * @brief Stop interrupt and release pin
     */
    void end();

    /**
     * @brief Non-blocking: copy latest frame if a new one is ready
     * @param pulses_us  Output array of channel pulse widths (µs)
     * @param maxCh      Size of pulses_us array
     * @param outCount   Number of channels decoded
     * @return true if a new frame was available
     */
    bool readFrame(uint16_t* pulses_us, uint8_t maxCh, uint8_t& outCount);

private:
    int  _pin = -1;
    bool _initialized = false;

    // Double-buffer: ISR writes to _isrBuf, readFrame copies from _frameBuf
    volatile uint16_t _isrBuf[RxConfig::MAX_CHANNELS]   = {};
    volatile uint16_t _frameBuf[RxConfig::MAX_CHANNELS]  = {};
    volatile uint8_t  _isrChCount  = 0;
    volatile uint8_t  _frameChCount = 0;
    volatile uint32_t _lastEdge_us = 0;
    volatile uint8_t  _isrIdx      = 0;
    std::atomic<bool> _frameReady{false};

    static void SFX_IRAM_FUNC isrHandler(void* param);
};

#endif // SFX_PLATFORM_PICO
#endif // SFX_PICO_ISR_PPM_POLICY_H
