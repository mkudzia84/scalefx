/*
 * Pico ISR PPM Policy — implementation
 *
 * GPIO interrupt fires on each edge.  The ISR measures the time between
 * consecutive edges of the selected polarity (rising or falling).
 * When a gap > PPM_SYNC_US is detected, the accumulated channels are
 * copied to the frame buffer and a flag is set for readFrame().
 */

// Pull in platform detection (defines SFX_PLATFORM_PICO) before the guard.
// Without this, the macro is undefined → 0 → the whole body is skipped and
// every linkage symbol declared in the .h becomes undefined at link time.
#include <platform/sfx_platform.h>

#if SFX_PLATFORM_PICO

#include "pico_isr_ppm_policy.h"
#include <string.h>

// ─── ISR ───────────────────────────────────────────────────────
void SFX_IRAM_FUNC PicoIsrPpmPolicy::isrHandler(void* param)
{
    auto* self = static_cast<PicoIsrPpmPolicy*>(param);
    const uint32_t now_us = micros();
    const uint32_t elapsed = now_us - self->_lastEdge_us;
    self->_lastEdge_us = now_us;

    if (elapsed == 0) return;

    // Sync gap — commit the frame
    if (elapsed >= RxConfig::PPM_SYNC_US) {
        if (self->_isrIdx > 0) {
            memcpy((void*)self->_frameBuf, (const void*)self->_isrBuf,
                   self->_isrIdx * sizeof(uint16_t));
            self->_frameChCount = self->_isrIdx;
            self->_frameReady.store(true, std::memory_order_release);
        }
        self->_isrIdx = 0;
        return;
    }

    // Valid channel pulse
    if (elapsed >= RxConfig::MIN_PULSE_US &&
        elapsed <= RxConfig::MAX_PULSE_US &&
        self->_isrIdx < RxConfig::MAX_CHANNELS)
    {
        self->_isrBuf[self->_isrIdx++] = static_cast<uint16_t>(elapsed);
    }
}

// ─── begin ─────────────────────────────────────────────────────
bool PicoIsrPpmPolicy::begin(int pin, bool risingEdge)
{
    if (_initialized) end();

    _pin = pin;
    _isrIdx = 0;
    _frameChCount = 0;
    _frameReady.store(false, std::memory_order_relaxed);
    _lastEdge_us = 0;

    pinMode(_pin, INPUT);

    PinStatus mode = risingEdge ? RISING : FALLING;
    SFX_ATTACH_INTERRUPT_PARAM(_pin, isrHandler, mode, this);

    _initialized = true;
    return true;
}

// ─── end ───────────────────────────────────────────────────────
void PicoIsrPpmPolicy::end()
{
    if (_initialized && _pin >= 0) {
        detachInterrupt(digitalPinToInterrupt(_pin));
    }
    _initialized = false;
    _pin = -1;
}

// ─── readFrame ─────────────────────────────────────────────────
bool PicoIsrPpmPolicy::readFrame(uint16_t* pulses_us, uint8_t maxCh, uint8_t& outCount)
{
    outCount = 0;
    if (!_frameReady.load(std::memory_order_acquire)) return false;

    const uint8_t count = _frameChCount;
    const uint8_t toCopy = (count < maxCh) ? count : maxCh;

    noInterrupts();
    memcpy(pulses_us, (const void*)_frameBuf, toCopy * sizeof(uint16_t));
    interrupts();

    _frameReady.store(false, std::memory_order_release);

    outCount = toCopy;
    return toCopy > 0;
}

#endif // SFX_PLATFORM_PICO
