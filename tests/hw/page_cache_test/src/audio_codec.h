/*
 * audio_codec.h — minimal TAS5825P bring-up for the test firmware.
 *
 * Lifted register sequence from `controllers/lib/sfx_audio/codec/
 * tas5825_p_codec.cpp` (HubFX production driver), stripped of the
 * Hybrid-Pro / boost / EQ logic we don't need here.  Two-phase init
 * mirrors production exactly so the same audio plays through the same
 * silicon path:
 *
 *   begin(wire, sda, scl)   — probe + reset + DEEP_SLEEP
 *   activate(sampleRate)    — clock-source select, SAP, HIZ → PLAY
 *
 * The split exists because the I²S clocks must be running BEFORE
 * `activate()` — otherwise the chip never reaches PLAY.  Caller:
 *   1. AudioCodec::instance().begin(Wire, 8, 9)
 *   2. start I²S (TAS5825P's BCLK + LRCK toggling)
 *   3. AudioCodec::instance().activate(48000)
 *
 * Wired to the HubFX board:
 *   I²C address: 0x4C
 *   SDA / SCL:   GPIO 8 / GPIO 9
 *   PVDD supply: 12 V (codec auto-detects via internal ADC on P silicon)
 */

#ifndef AUDIO_CODEC_TEST_H
#define AUDIO_CODEC_TEST_H

#include <Arduino.h>
#include <Wire.h>
#include <cstdint>

class AudioCodec {
public:
    static AudioCodec& instance() {
        static AudioCodec inst;
        return inst;
    }

    AudioCodec(const AudioCodec&)            = delete;
    AudioCodec& operator=(const AudioCodec&) = delete;

    /// Phase 1: I²C probe + soft reset → DEEP_SLEEP.  Safe to call
    /// before I²S clocks are running.  `wire.begin()` is NOT called
    /// here — caller wires up `Wire.begin(sda, scl)` first.
    bool begin(TwoWire& wire);

    /// Phase 2: configure clock source + SAP + HIZ → PLAY.  Caller
    /// must have I²S clocks running.  Returns true if the chip
    /// reached PLAY state.
    bool activate(uint32_t sampleRate);

    /// Set digital volume.  `volume_db` clamped to [-100 dB, +24 dB].
    /// 0 dB ≈ 0.5 in linear units; full scale = +24 dB.  Production
    /// uses [-15 dB, -10 dB] range for typical playback.
    void setVolumeDb(float volume_db);

    /// Setting mute writes 0x08 to DEVICE_CTRL_2; un-mute restores
    /// PLAY.  Caller should pause its audio stream before muting to
    /// avoid pops on un-mute.
    void setMute(bool mute);

    bool isInitialized() const { return _initialized; }
    bool isPlaying()     const { return _playing; }

private:
    AudioCodec() = default;

    bool writeReg(uint8_t reg, uint8_t val);
    bool readReg(uint8_t reg, uint8_t& val);

    TwoWire* _i2c         = nullptr;
    bool     _initialized = false;
    bool     _playing     = false;
    uint8_t  _volume      = 48;   // 0 dB (production default)
};

#endif  // AUDIO_CODEC_TEST_H
