/**
 * @file tas5825_m_codec.h
 * @brief TI TAS5825**M** audio amplifier driver — smart-amp / IV-sense variant.
 *
 * Restored 2026-07-30 (the bench board turned out to carry TAS5825M
 * silicon, not the P the BOM claimed) from the pre-arduino-removal
 * driver (git 1d5a942~1), ported to the native SfxI2cBus API.
 *
 * The M-variant differs from the P-variant in three ways the firmware
 * has to know about:
 *
 *   1. **Strict startup discipline** — the M latches CDET (clock detect)
 *      out of reset, requires `FAULT_CLEAR` writes at three points
 *      during init, and refuses HIZ → PLAY until `FS_MON` reports a
 *      valid sample-rate code. The P silently tolerates all three; the
 *      M does not.
 *
 *   2. **Smart-amp / IV-sense** — the M has output current + voltage
 *      sense ADCs feeding a speaker thermal/excursion model. This
 *      driver exposes `setIvSenseEnabled()`, `calibrateSpeaker()`,
 *      and the live `readSpeakerTemp_C()` / `readSpeakerExcursion()`
 *      queries so application code can implement protection.
 *
 *   3. **GPIO1_SEL value `0x01`** — *Reserved* on the M (it's
 *      Hybrid-Pro feedback on the P). This driver explicitly programs
 *      `0x0B` (FAULTZ) into GPIO1_SEL (0x62) and sets the pin's output
 *      enable in GPIO_CTRL (0x60) so GPIO1 is in a known state.
 *
 * Register map audited against the TI datasheet SLASEH7H (2026-07-30)
 * — see tas5825_regs.h for the gold-standard note.
 *
 * For TAS5825P boards, use TAS5825PCodec instead — it implements the
 * Hybrid-Pro/boost-converter API that doesn't exist on this silicon.
 *
 * Reference: TAS5825M datasheet, SLAA846 (Advanced Features app note).
 * Bench bring-up mirror: tests/hw/tas5825m_beep (self-contained IDF
 * probe with the same sequence, step-by-step instrumented).
 */

#ifndef TAS5825_M_CODEC_H
#define TAS5825_M_CODEC_H

#include <i2c/sfx_i2c.h>
#include "../audio/audio_config.h"
#include "tas5825_regs.h"

/**
 * @class TAS5825MCodec
 * @brief TI TAS5825M (smart-amp variant) audio codec driver.
 *
 * Singleton — there is at most one TAS5825M per board. Satisfies the
 * AudioMixer<TI2S, TCodec> contract; pick this or TAS5825PCodec at the
 * typedef level depending on which silicon is populated.
 */
class TAS5825MCodec {
public:
    static TAS5825MCodec& instance() {
        static TAS5825MCodec inst;
        return inst;
    }

    TAS5825MCodec(const TAS5825MCodec&)            = delete;
    TAS5825MCodec& operator=(const TAS5825MCodec&) = delete;
    TAS5825MCodec(TAS5825MCodec&&)                 = delete;
    TAS5825MCodec& operator=(TAS5825MCodec&&)      = delete;

    // ─── Phase 1: pre-clock init ───────────────────────────────────────
    //
    // Probe the codec, full reset, park in DEEP_SLEEP + MUTE, clear the
    // boot-time CDET latch. Safe to call before I2S clocks are running.
    bool begin(sfx_peripherals::SfxI2cBus& wire, int sda, int scl,
               uint32_t sample_rate = AUDIO_SAMPLE_RATE);

    // ─── Phase 2: post-clock activation ────────────────────────────────
    //
    // MUST be called AFTER I2S BCLK + LRCLK are stable. Configures
    // clocks and GPIO1 routing, polls FS_MON for a valid sample-rate
    // code, measures PVDD and auto-picks the analog gain to match the
    // rail, then HIZ → PLAY with fault clears around the transitions.
    bool activate();

    // ─── AudioMixer contract ──────────────────────────────────────────
    bool begin(uint32_t sample_rate = AUDIO_SAMPLE_RATE);
    void reset();
    void setVolume(float volume);
    void setMute(bool mute);
    bool isInitialized() const { return initialized_; }
    const char* getModelName() const { return "TAS5825M"; }

    // ─── Volume ───────────────────────────────────────────────────────
    void setVolumeDB(float db);

    // ─── Fault management ─────────────────────────────────────────────
    bool clearFaults();

    /// Read GLOBAL_FAULT1 (0x71). Bit-decode constants in tas5825_regs.h.
    uint8_t readFaultRegister();

    // ─── M-SPECIFIC: smart-amp / IV-sense API ─────────────────────────
    //
    // The TAS5825M has a closed-loop speaker protection system based
    // on integrated I/V sense at the output. These methods expose it
    // to application code so a future audio engine can react to
    // speaker temperature / excursion events.
    //
    // Currently the firmware does not configure smart-amp coefficients
    // (the speaker model needs PPC3 calibration data per speaker). The
    // hooks below stub-return safe defaults until that pipeline is
    // built — but the wire path is in place so a `smartAmp...` call
    // doesn't NACK silently.

    /**
     * @brief Enable / disable smart-amp speaker protection.
     *
     * When disabled (default after reset), the chip behaves as a
     * plain Class-D — same as a TAS5825P would. When enabled, the
     * IV-sense ADCs run, the speaker thermal/excursion model is
     * active, and the chip will reduce gain / mute to protect the
     * speaker.
     *
     * Requires `calibrateSpeaker()` to have been called at least
     * once with a valid speaker model loaded — otherwise the chip
     * uses generic protection thresholds that are conservative.
     */
    bool smartAmpEnable(bool on);

    /// Returns true when smart-amp is currently armed in firmware state.
    bool smartAmpEnabled() const { return smartAmpOn_; }

    /**
     * @brief Trigger a speaker calibration pass.
     *
     * Currently a stub — full PPC3 calibration sequence not yet
     * implemented; the chip's factory protection thresholds stay in
     * effect (conservative but functional for typical ≤4 Ω drivers).
     */
    bool calibrateSpeaker();

    /**
     * @brief Read the smart-amp's voice-coil temperature estimate.
     *
     * Returns 0 if smart-amp isn't enabled or never calibrated.
     */
    int16_t readSpeakerTemp_C();

    /**
     * @brief Read the smart-amp's diaphragm excursion estimate
     *        (millimetres × 100). 0 unless smart-amp is enabled.
     */
    uint16_t readSpeakerExcursion_mm100();

    /**
     * @brief Enable / disable just the IV-sense ADCs — the lower-level
     *        toggle smart-amp uses internally.
     */
    bool setIvSenseEnabled(bool on);

    // ─── Diagnostics / introspection ──────────────────────────────────
    void dumpRegisters();
    uint8_t getCodecType() const { return 1; }   // 1 = TAS5825M (per protocol)
    bool    getMuted() const { return muted_; }
    uint8_t getVolumeRegister() const { return currentVolume_; }
    int     getSdaPin() const { return sdaPin_; }
    int     getSclPin() const { return sclPin_; }
    uint8_t getDeviceControlRegister();
    uint8_t getFaultRegister();
    bool    testI2CConnection();

    // ─── Power telemetry ──────────────────────────────────────────────
    /// Live PVDD rail read via the chip's own ADC (0 on wire error).
    uint32_t readPvdd_mV();
    /// AGAIN step chosen by the PVDD auto-detect at activate().
    uint8_t  getAgainRegister() const { return againReg_; }
    /// DIE_ID read at probe (0x95 = TAS5825M silicon).
    uint8_t  getDieId() const { return dieId_; }

private:
    TAS5825MCodec();

    sfx_peripherals::SfxI2cBus* i2c_ = nullptr;
    int      sdaPin_      = -1;
    int      sclPin_      = -1;
    uint32_t sampleRate_  = 0;
    bool     initialized_ = false;
    bool     muted_       = false;
    bool     smartAmpOn_  = false;
    uint8_t  currentVolume_ = sfx_audio::tas5825::VOL_0DB;
    uint8_t  againReg_    = sfx_audio::tas5825::AGAIN_FALLBACK;
    uint8_t  dieId_       = 0;

    // Low-level I/O helpers (shared shape between M and P, but each
    // variant keeps its own copy so the code reads top-to-bottom in
    // one file per chip).
    bool writeRegister(uint8_t reg, uint8_t value);
    bool readRegister(uint8_t reg, uint8_t* value);
    bool selectBookPage(uint8_t book, uint8_t page);

    // M-specific init helpers
    bool waitForFsLock(uint32_t timeoutMs, uint8_t* outFsCode);
    bool configureAnalogGain();
};

#endif // TAS5825_M_CODEC_H
