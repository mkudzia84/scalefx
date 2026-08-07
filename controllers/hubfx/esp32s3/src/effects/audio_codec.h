/*
 * audio_codec.h — HubFX-specific `CodecAdapter` specialization.
 *
 *   `EspDualCoreAudio<Mixer>` is generic — it knows it has a codec
 *   (via `Mixer::Codec`) but not which I²C pins the codec sits on.
 *   This file plugs those board specifics into the generic helper by
 *   specializing the `CodecAdapter` trait for the HubFX's TAS5825.
 *
 *   WHICH SILICON: the BOM says TAS5825P, but some board lots carry
 *   TAS5825M (found 2026-07-30 — check the DIE_ID line in the boot log
 *   or `codec-status`: 0x95 = M).  Select the driver at build time:
 *
 *     default                   → TAS5825PCodec (permissive startup)
 *     -DHUBFX_CODEC_TAS5825M    → TAS5825MCodec (strict startup +
 *                                  smart-amp/IV-sense surface)
 *
 *   Both drivers auto-detect the PVDD rail via the chip's PVDD ADC at
 *   activate() and pick the analog gain to match — the old
 *   `Codec::SUPPLY_VOLTAGE` constant / `audio.codec_supply` YAML key
 *   are gone.
 *
 *   The specialization references the sketch's `Gpio::I2C_SDA` /
 *   `Gpio::I2C_SCL` constants — i.e. THIS HEADER MUST BE INCLUDED
 *   AFTER THE SKETCH'S `Gpio::` NAMESPACE IS DECLARED.
 */

#ifndef HUBFX_AUDIO_CODEC_H
#define HUBFX_AUDIO_CODEC_H

#include "../hubfx_i2c.h"                  // hubI2cBus() — shared native I2C bus
#include <audio/audio_config.h>            // AUDIO_SAMPLE_RATE
#include <audio/esp_dual_core_audio.h>     // CodecAdapter primary template

#if defined(HUBFX_CODEC_TAS5825M)
#include <codec/tas5825_m_codec.h>
using HubFxCodecChip = TAS5825MCodec;
#else
#include <codec/tas5825_p_codec.h>
using HubFxCodecChip = TAS5825PCodec;
#endif

// CodecAdapter specialization — encapsulates everything TAS5825-specific
// that the generic `EspDualCoreAudio<>` helper would otherwise need to
// know.  Static methods → fully inlined at the helper's call sites;
// picked up automatically via `Mixer::Codec`.
template <>
struct CodecAdapter<HubFxCodecChip> {
    static constexpr bool kHasCodec = true;

    static bool probe() {
        return HubFxCodecChip::instance().begin(
            hubI2cBus(),
            Gpio::I2C_SDA,
            Gpio::I2C_SCL,
            AUDIO_SAMPLE_RATE);
    }

    static bool activate() {
        return HubFxCodecChip::instance().activate();
    }
};

#endif  // HUBFX_AUDIO_CODEC_H
