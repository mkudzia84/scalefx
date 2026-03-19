/**
 * @file pcm5102a_codec.cpp
 * @brief TI PCM5102A Stereo DAC Driver — Implementation
 */

#include "pcm5102a_codec.h"
#include "../audio/audio_log.h"

// Logging macro for PCM5102A (defined locally if not in audio_log.h)
#ifndef PCM5102_LOG
#define PCM5102_LOG(fmt, ...) \
    do { DiagLog::instance().info("[PCM5102] " fmt, ##__VA_ARGS__); } while(0)
#endif

// ---- Construction ----

PCM5102ACodec::PCM5102ACodec()
    : _xsmtPin(-1)
    , _fmtPin(-1)
    , _fltPin(-1)
    , _dempPin(-1)
    , _initialized(false)
    , _muted(true)            // PCM5102A internal pull-down keeps XSMT LOW (muted)
    , _leftJustified(false)   // I2S format (default)
    , _lowLatencyFilter(false) // Normal latency (default)
    , _deEmphasis(false)      // De-emphasis off (default)
    , _sampleRate(0) {
}

// ---- Codec interface ----

bool PCM5102ACodec::begin(uint32_t sample_rate) {
    if (_initialized) return true;

    _sampleRate = sample_rate;
    PCM5102_LOG("Initializing PCM5102A (sample rate: %lu Hz)", (unsigned long)sample_rate);

    // Configure XSMT pin (soft mute — REQUIRED for audio output)
    if (_xsmtPin >= 0) {
        pinMode(_xsmtPin, OUTPUT);
        digitalWrite(_xsmtPin, HIGH);  // Unmute immediately
        _muted = false;
        PCM5102_LOG("  XSMT pin %d -> HIGH (unmuted)", _xsmtPin);
    } else {
        PCM5102_LOG("  WARNING: No XSMT pin configured — DAC may stay muted (internal pull-down)");
    }

    // Configure FMT pin (audio format)
    if (_fmtPin >= 0) {
        pinMode(_fmtPin, OUTPUT);
        digitalWrite(_fmtPin, _leftJustified ? HIGH : LOW);
        PCM5102_LOG("  FMT pin %d -> %s", _fmtPin, _leftJustified ? "HIGH (Left-Justified)" : "LOW (I2S)");
    }

    // Configure FLT pin (digital filter)
    if (_fltPin >= 0) {
        pinMode(_fltPin, OUTPUT);
        digitalWrite(_fltPin, _lowLatencyFilter ? HIGH : LOW);
        PCM5102_LOG("  FLT pin %d -> %s", _fltPin, _lowLatencyFilter ? "HIGH (low latency)" : "LOW (normal)");
    }

    // Configure DEMP pin (de-emphasis)
    if (_dempPin >= 0) {
        pinMode(_dempPin, OUTPUT);
        digitalWrite(_dempPin, _deEmphasis ? HIGH : LOW);
        PCM5102_LOG("  DEMP pin %d -> %s", _dempPin, _deEmphasis ? "HIGH (on)" : "LOW (off)");
    }

    _initialized = true;
    PCM5102_LOG("PCM5102A initialized OK");
    return true;
}

void PCM5102ACodec::reset() {
    if (_xsmtPin >= 0) {
        // Toggle XSMT LOW then HIGH to force a mute/unmute cycle
        digitalWrite(_xsmtPin, LOW);
        delayMicroseconds(100);  // Brief mute pulse
        digitalWrite(_xsmtPin, HIGH);
        _muted = false;
    }

    // Reset format pins to defaults
    if (_fmtPin >= 0) {
        _leftJustified = false;
        digitalWrite(_fmtPin, LOW);
    }
    if (_fltPin >= 0) {
        _lowLatencyFilter = false;
        digitalWrite(_fltPin, LOW);
    }
    if (_dempPin >= 0) {
        _deEmphasis = false;
        digitalWrite(_dempPin, LOW);
    }

    PCM5102_LOG("Reset to defaults");
}

void PCM5102ACodec::setVolume(float volume) {
    // PCM5102A has no hardware volume control — mixer handles gain scaling.
    // Use XSMT as a hard mute threshold: volume at 0 = mute, any value > 0 = unmute.
    if (volume <= 0.0f) {
        setMute(true);
    } else if (_muted) {
        setMute(false);
    }
}

void PCM5102ACodec::setMute(bool mute) {
    _muted = mute;
    if (_xsmtPin >= 0) {
        // XSMT: HIGH = unmuted, LOW = muted
        digitalWrite(_xsmtPin, mute ? LOW : HIGH);
        PCM5102_LOG("Mute %s (XSMT -> %s)", mute ? "ON" : "OFF", mute ? "LOW" : "HIGH");
    }
}

// ---- PCM5102A-specific control ----

void PCM5102ACodec::setLeftJustified(bool lj) {
    _leftJustified = lj;
    if (_fmtPin >= 0 && _initialized) {
        digitalWrite(_fmtPin, lj ? HIGH : LOW);
        PCM5102_LOG("Format: %s", lj ? "Left-Justified" : "I2S");
    }
}

void PCM5102ACodec::setLowLatencyFilter(bool lowLatency) {
    _lowLatencyFilter = lowLatency;
    if (_fltPin >= 0 && _initialized) {
        digitalWrite(_fltPin, lowLatency ? HIGH : LOW);
        PCM5102_LOG("Filter: %s latency", lowLatency ? "low" : "normal");
    }
}

void PCM5102ACodec::setDeEmphasis(bool enabled) {
    _deEmphasis = enabled;
    if (_dempPin >= 0 && _initialized) {
        digitalWrite(_dempPin, enabled ? HIGH : LOW);
        PCM5102_LOG("De-emphasis: %s", enabled ? "ON" : "OFF");
    }
}

// ---- Status ----

uint8_t PCM5102ACodec::_getControlBits() const {
    // Synthetic control byte for CODEC_STATUS response:
    //   Bit 0: XSMT state (1 = unmuted)
    //   Bit 1: FMT  (1 = Left-Justified)
    //   Bit 2: FLT  (1 = low latency)
    //   Bit 3: DEMP (1 = on)
    //   Bit 7: initialized
    uint8_t bits = 0;
    if (!_muted)          bits |= (1 << 0);
    if (_leftJustified)   bits |= (1 << 1);
    if (_lowLatencyFilter) bits |= (1 << 2);
    if (_deEmphasis)       bits |= (1 << 3);
    if (_initialized)      bits |= (1 << 7);
    return bits;
}
