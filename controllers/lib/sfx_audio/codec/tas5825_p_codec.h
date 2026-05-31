/**
 * @file tas5825_p_codec.h
 * @brief TI TAS5825**P** audio amplifier driver — Class-H + Hybrid-Pro variant.
 *
 * The P-variant differs from the M-variant in three ways the firmware
 * has to know about:
 *
 *   1. **Permissive startup discipline** — the P does not enforce the
 *      M's CDET / FS_MON guards: writing PLAY while clocks are still
 *      locking is allowed and the chip auto-recovers. The init flow
 *      below is intentionally less paranoid than the M's; it's the
 *      proven flow on the original HubFX bring-up board.
 *
 *   2. **Hybrid-Pro feedback** — the P drives an external DC-DC
 *      converter via the HPFB pin (multiplexed onto GPIO1 with bit
 *      pattern `0x01` of GPIO1_SEL) using a 4 ms-lookahead audio
 *      power prediction algorithm. This driver exposes
 *      `hybridProEnable()`, `hybridProSetTarget()`, and
 *      `boostConfigure()` so the application can tune the boost
 *      response curve to the actual external converter on the BOM.
 *
 *   3. **No smart-amp / IV-sense** — the P has no current/voltage
 *      sense ADCs at the output and no built-in speaker thermal
 *      model. Speaker protection is purely passive (PVDD UVP/OVP +
 *      thermal shutdown). For boards that need active speaker
 *      protection use the M variant.
 *
 * The HubFX board does NOT have an external DC-DC boost on the BOM,
 * so even on a P-populated board the Hybrid-Pro feature is unused.
 * This driver is here for completeness and for future boards that
 * might leverage the P's Class-H efficiency.
 *
 * Reference: TAS5825P datasheet, TAS5825P Hybrid-Pro User Guidance.
 */

#ifndef TAS5825_P_CODEC_H
#define TAS5825_P_CODEC_H

#include <i2c/sfx_i2c.h>
#include "../audio/audio_config.h"
#include "tas5825_regs.h"

/**
 * @class TAS5825PCodec
 * @brief TI TAS5825P (Class-H + Hybrid-Pro variant) audio codec driver.
 *
 * Singleton. Same `AudioMixer<TI2S, TCodec>` contract as TAS5825MCodec
 * — pick one or the other at the typedef level depending on which
 * silicon is populated on the target board.
 */
class TAS5825PCodec {
public:
    using Supply = sfx_audio::tas5825::Supply;

    static TAS5825PCodec& instance() {
        static TAS5825PCodec inst;
        return inst;
    }

    TAS5825PCodec(const TAS5825PCodec&)            = delete;
    TAS5825PCodec& operator=(const TAS5825PCodec&) = delete;
    TAS5825PCodec(TAS5825PCodec&&)                 = delete;
    TAS5825PCodec& operator=(TAS5825PCodec&&)      = delete;

    // ─── Phase 1: pre-clock init ───────────────────────────────────────
    bool begin(sfx_peripherals::SfxI2cBus& wire, int sda, int scl,
               uint32_t sample_rate = AUDIO_SAMPLE_RATE,
               Supply supply        = Supply::V20);

    // ─── Phase 2: post-clock activation ────────────────────────────────
    bool activate();

    // ─── AudioMixer contract ──────────────────────────────────────────
    bool begin(uint32_t sample_rate = AUDIO_SAMPLE_RATE);
    void reset();
    void setVolume(float volume);
    void setMute(bool mute);
    bool isInitialized() const { return initialized_; }
    const char* getModelName() const { return "TAS5825P"; }

    // ─── Volume / supply ──────────────────────────────────────────────
    void setVolumeDB(float db);
    bool setSupplyVoltage(Supply voltage);

    // ─── Fault management ─────────────────────────────────────────────
    bool clearFaults();
    uint8_t readFaultRegister();

    // ─── P-SPECIFIC: Hybrid-Pro / boost-converter API ────────────────
    //
    // The TAS5825P controls an external DC-DC boost converter to scale
    // PVDD with audio peak demand (Class-H). The Hybrid-Pro algorithm
    // looks ahead 4 ms in the audio stream and drives the boost
    // setpoint via the HPFB feedback pin (GPIO1).
    //
    // These methods are no-ops if the application doesn't have an
    // external boost on the BOM — the P will still operate as a
    // plain Class-D amp with fixed PVDD, just without the efficiency
    // win.

    /**
     * @brief Enable / disable Hybrid-Pro Class-H control.
     *
     * When enabled, GPIO1 is reconfigured as HPFB output and the
     * 4 ms-lookahead engine runs continuously. When disabled, GPIO1
     * is rerouted to FAULT-asserted and the chip behaves as a
     * fixed-PVDD Class-D — same as the M would.
     *
     * @return true if the chip accepted the mode change
     */
    bool hybridProEnable(bool on);

    /// True if Hybrid-Pro is currently armed in firmware state.
    bool hybridProEnabled() const { return hybridProOn_; }

    /**
     * @brief Set the Hybrid-Pro target PVDD voltage.
     *
     * Drives the boost converter setpoint when audio is silent. As
     * audio peaks arrive the engine raises the setpoint up to the
     * configured PVDD ceiling (`boostConfigure` max).
     *
     * @param targetMv  Target idle PVDD in millivolts (e.g. 6000 for
     *                  a 6 V idle on a 12 V max-PVDD design).
     * @return true on success
     */
    bool hybridProSetTarget(uint16_t targetMv);

    /**
     * @brief Configure the external boost converter envelope.
     *
     * Tells the Hybrid-Pro engine the bounds the external DC-DC can
     * track. Typical values for a 12V-max design: `{6000, 12000}`.
     *
     * @param minMv Minimum PVDD (idle floor)
     * @param maxMv Maximum PVDD (peak ceiling)
     */
    bool boostConfigure(uint16_t minMv, uint16_t maxMv);

    /**
     * @brief Read the current PVDD voltage (Hybrid-Pro feedback path).
     *
     * Returns the live boost-converter output as observed by the
     * P's internal ADC. Useful for diagnostics + closed-loop
     * verification.
     *
     * @return PVDD in millivolts, or 0 if Hybrid-Pro is disabled.
     */
    uint16_t readBoostVoltage_mV();

    // ─── Diagnostics / introspection ──────────────────────────────────
    void dumpRegisters();
    uint8_t getCodecType() const { return 2; }   // 2 = TAS5825P (per protocol)
    bool    getMuted() const { return muted_; }
    uint8_t getVolumeRegister() const { return currentVolume_; }
    Supply  getSupplyVoltage() const { return supply_; }
    int     getSdaPin() const { return sdaPin_; }
    int     getSclPin() const { return sclPin_; }
    uint8_t getDeviceControlRegister();
    uint8_t getFaultRegister();
    bool    testI2CConnection();

private:
    TAS5825PCodec();

    sfx_peripherals::SfxI2cBus* i2c_         = nullptr;
    int      sdaPin_      = -1;
    int      sclPin_      = -1;
    uint32_t sampleRate_  = 0;
    Supply   supply_      = Supply::V20;
    bool     initialized_ = false;
    bool     muted_       = false;
    bool     hybridProOn_ = false;
    uint16_t boostMinMv_  = 0;
    uint16_t boostMaxMv_  = 0;
    uint8_t  currentVolume_ = sfx_audio::tas5825::VOL_0DB;

    bool writeRegister(uint8_t reg, uint8_t value);
    bool readRegister(uint8_t reg, uint8_t* value);
    bool selectBookPage(uint8_t book, uint8_t page);

    bool initDSPCoefficients();
    bool configureAnalogGain();
};

#endif // TAS5825_P_CODEC_H
