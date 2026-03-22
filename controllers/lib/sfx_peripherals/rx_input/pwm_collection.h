/*
 * PwmCollection — Multi-channel PWM input via individual GPIO pins
 *
 * Wraps an array of PwmInput objects (from pwm/pwm_control.h) and
 * exposes the ChannelSource concept so it can be used interchangeably
 * with PpmDecoder inside RxInputs<TSource>.
 *
 * Each channel is measured on a separate GPIO pin using hardware
 * interrupts (async mode).
 *
 * ChannelSource concept:
 *   bool         begin(...)               — varies per source
 *   void         end()
 *   void         update()
 *   uint16_t     channel_us(uint8_t ch)   — 1-based channel index
 *   uint8_t      channelCount()
 *   bool         isValid()
 *   bool         isInitialized()
 */

#ifndef SFX_PWM_COLLECTION_H
#define SFX_PWM_COLLECTION_H

#include <Arduino.h>
#include "rx_common.h"
#include "../pwm/pwm_control.h"

class PwmCollection {
public:
    PwmCollection() = default;
    ~PwmCollection() { end(); }

    PwmCollection(const PwmCollection&) = delete;
    PwmCollection& operator=(const PwmCollection&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Initialization
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Initialize multiple PWM inputs on individual GPIO pins
     * @param pins      Array of GPIO pin numbers (one per channel)
     * @param numPins   Number of channels (capped at MAX_CHANNELS)
     * @return true if all channels initialized successfully
     */
    bool begin(const int* pins, uint8_t numPins) {
        end();
        _count = (numPins > RxConfig::MAX_CHANNELS) ? RxConfig::MAX_CHANNELS : numPins;

        bool allOk = true;
        for (uint8_t i = 0; i < _count; i++) {
            if (!_inputs[i].beginAsync(PwmInputType::Pwm, pins[i])) {
                allOk = false;
            }
        }
        _initialized = true;
        return allOk;
    }

    /**
     * @brief Stop all PWM inputs
     */
    void end() {
        for (uint8_t i = 0; i < _count; i++) {
            _inputs[i].end();
        }
        _count = 0;
        _initialized = false;
    }

    // ════════════════════════════════════════════════════════════
    //  ChannelSource interface
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Call regularly to check for timeout on each input
     */
    void update() {
        for (uint8_t i = 0; i < _count; i++) {
            _inputs[i].update();
        }
    }

    /**
     * @brief Get channel pulse width
     * @param ch  1-based channel number
     * @return Pulse width in µs, or CENTER_US if invalid
     */
    uint16_t channel_us(uint8_t ch) const {
        if (ch == 0 || ch > _count) return RxConfig::CENTER_US;
        const auto& inp = _inputs[ch - 1];
        int val = inp.average();
        return (val > 0) ? static_cast<uint16_t>(val) : RxConfig::CENTER_US;
    }

    /**
     * @brief Number of configured channels
     */
    uint8_t channelCount() const { return _count; }

    /**
     * @brief Check if at least one channel has valid data
     */
    bool isValid() const {
        for (uint8_t i = 0; i < _count; i++) {
            if (_inputs[i].isValid()) return true;
        }
        return false;
    }

    /**
     * @brief Check if begin() was called
     */
    bool isInitialized() const { return _initialized; }

    // ════════════════════════════════════════════════════════════
    //  Direct access to underlying PwmInput objects
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Get reference to a specific PwmInput
     * @param idx  0-based index
     */
    PwmInput& input(uint8_t idx) { return _inputs[idx]; }
    const PwmInput& input(uint8_t idx) const { return _inputs[idx]; }

    /**
     * @brief Get GPIO pin for a channel
     * @param ch  1-based channel
     * @return GPIO pin, or -1 if invalid
     */
    int pin(uint8_t ch) const {
        if (ch == 0 || ch > _count) return -1;
        return _inputs[ch - 1].pin();
    }

private:
    PwmInput _inputs[RxConfig::MAX_CHANNELS];
    uint8_t  _count       = 0;
    bool     _initialized = false;
};

#endif // SFX_PWM_COLLECTION_H
