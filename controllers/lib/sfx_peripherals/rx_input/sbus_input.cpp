/*
 * SbusInput — implementation
 *
 * S.Bus frame: 25 bytes
 *   [0x0F] [data×22] [flags] [end]
 *
 * Data bytes 1–22 contain 16 channels packed at 11 bits each (LSB first).
 * Flag byte 23: CH17, CH18, frame-lost, failsafe.
 */

#include "sbus_input.h"

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <string.h>

// ─── begin ─────────────────────────────────────────────────────
bool SbusInput::begin(sfx::Stream* serial)
{
    if (!serial) return false;
    end();
    _serial = serial;
    return true;
}

// ─── end ───────────────────────────────────────────────────────
void SbusInput::end()
{
    _serial     = nullptr;
    _bufIdx     = 0;
    _hasFrame   = false;
    _flags      = 0;
    _frameCount = 0;
    _errorCount = 0;
    _lastFrameMs = 0;
    _lastByte_us = 0;
    memset(_channels_us, 0, sizeof(_channels_us));
}

// ─── update ────────────────────────────────────────────────────
void SbusInput::update()
{
    if (!_serial) return;

    while (_serial->available()) {
        uint8_t b = static_cast<uint8_t>(_serial->read());
        uint32_t now_us = SFX_MICROS();

        // Timing-based resync: gap > FRAME_GAP_US means start of new frame
        if (_bufIdx > 0 && (now_us - _lastByte_us) > SbusConfig::FRAME_GAP_US) {
            _bufIdx = 0;
        }
        _lastByte_us = now_us;

        // Wait for start byte
        if (_bufIdx == 0 && b != SbusConfig::START_BYTE) {
            continue;
        }

        _buf[_bufIdx++] = b;

        if (_bufIdx >= SbusConfig::FRAME_SIZE) {
            parseFrame();
            _bufIdx = 0;
        }
    }
}

// ─── parseFrame ────────────────────────────────────────────────
void SbusInput::parseFrame()
{
    // Validate start byte (should always be 0x0F at this point)
    if (_buf[0] != SbusConfig::START_BYTE) {
        _errorCount++;
        return;
    }

    // Validate the end byte: standard is 0x00, but receivers also emit
    // 0x04/0x14/0x24/0x34 — the low nibble is always 0.  A non-zero low
    // nibble means we are mis-framed; drop the frame and count it so the
    // start-byte hunt resyncs.  (Verified on the input_monitor bench rig;
    // timing-only resync is unreliable because the UART driver delivers RX
    // in bursts, so SFX_MICROS() gaps between reads don't reflect wire timing.)
    if ((_buf[SbusConfig::FRAME_SIZE - 1] & 0x0F) != 0x00) {
        _errorCount++;
        return;
    }

    // Extract 16 × 11-bit channels from bytes 1–22 (176 bits, LSB-packed)
    const uint8_t* p = &_buf[1];
    for (uint8_t ch = 0; ch < SbusConfig::NUM_CHANNELS; ch++) {
        uint16_t bitPos = ch * 11;
        uint8_t  byte0  = bitPos / 8;
        uint8_t  bit0   = bitPos % 8;

        // Grab up to 3 bytes to cover the 11-bit span
        uint32_t word = p[byte0] | (static_cast<uint32_t>(p[byte0 + 1]) << 8);
        if (bit0 + 11 > 16) {
            word |= (static_cast<uint32_t>(p[byte0 + 2]) << 16);
        }
        uint16_t raw = (word >> bit0) & 0x7FF;

        _channels_us[ch] = SbusConfig::rawToUs(raw);
    }

    // Flags (byte 23)
    _flags = _buf[23];

    _lastFrameMs = SFX_MILLIS();
    _hasFrame    = true;
    _frameCount++;
}

// ─── channel_us ────────────────────────────────────────────────
uint16_t SbusInput::channel_us(uint8_t ch) const
{
    if (ch < 1 || ch > SbusConfig::NUM_CHANNELS || !_hasFrame) {
        return RxConfig::CENTER_US;
    }
    return _channels_us[ch - 1];
}

// ─── isValid ───────────────────────────────────────────────────
bool SbusInput::isValid() const
{
    if (!_hasFrame || _lastFrameMs == 0) return false;
    return (SFX_MILLIS() - _lastFrameMs) < RxConfig::SIGNAL_TIMEOUT_MS;
}

#endif // SFX_PLATFORM_ESP32
