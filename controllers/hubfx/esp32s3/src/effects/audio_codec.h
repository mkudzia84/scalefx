/*
 * audio_codec.h — HubFX-specific `CodecAdapter` specialization.
 *
 *   `EspDualCoreAudio<Mixer>` is generic — it knows it has a codec
 *   (via `Mixer::Codec`) but not which I²C pins the codec sits on or
 *   what supply voltage drives it.  This file plugs those board-rev
 *   specifics into the generic helper by specializing the
 *   `CodecAdapter` trait for the HubFX rev's TAS5825P.
 *
 *   The specialization references the sketch's `Gpio::I2C_SDA`,
 *   `Gpio::I2C_SCL`, and `Codec::SUPPLY_VOLTAGE` constants — i.e.
 *   THIS HEADER MUST BE INCLUDED AFTER THE SKETCH'S `Gpio::` AND
 *   `Codec::` NAMESPACES ARE DECLARED.  In practice that means the
 *   `#include` lives in the middle of `hubfx_esp32s3.ino`, right
 *   before the `EspDualCoreAudio<Mixer> audio;` static.
 *
 *   Co-located here (next to `audio_layout.h`) so the entire
 *   board-specific audio plumbing — channel map, codec wiring — is
 *   in one folder.  Porting to a future board with a different codec
 *   or different pins is a single-file edit.
 */

#ifndef HUBFX_AUDIO_CODEC_H
#define HUBFX_AUDIO_CODEC_H

#include "../hubfx_i2c.h"                  // hubI2cBus() — shared native I2C bus
#include <audio/audio_config.h>            // AUDIO_SAMPLE_RATE
#include <audio/esp_dual_core_audio.h>     // CodecAdapter primary template
#include <codec/tas5825_p_codec.h>         // TAS5825PCodec singleton

// CodecAdapter specialization — encapsulates everything TAS5825P-specific
// that the generic `EspDualCoreAudio<>` helper would otherwise need to
// know.  Static methods → fully inlined at the helper's call sites;
// picked up automatically via `Mixer::Codec`.
template <>
struct CodecAdapter<TAS5825PCodec> {
    static constexpr bool kHasCodec = true;

    static bool probe() {
        return TAS5825PCodec::instance().begin(
            hubI2cBus(),
            Gpio::I2C_SDA,
            Gpio::I2C_SCL,
            AUDIO_SAMPLE_RATE,
            Codec::SUPPLY_VOLTAGE);
    }

    static bool activate() {
        return TAS5825PCodec::instance().activate();
    }
};

#endif  // HUBFX_AUDIO_CODEC_H
