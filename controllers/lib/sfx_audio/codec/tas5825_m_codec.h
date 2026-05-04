/**
 * @file tas5825_m_codec.h
 * @brief TI TAS5825**M** audio amplifier driver — smart-amp / IV-sense variant.
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
 *      `0x0B` (FAULT asserted) so GPIO1 is in a known state.
 *
 * For TAS5825P boards, use TAS5825PCodec instead — it implements the
 * Hybrid-Pro/boost-converter API that doesn't exist on this silicon.
 *
 * Reference: TAS5825M datasheet, SLAA846 (Advanced Features app note).
 */

#ifndef TAS5825_M_CODEC_H
#define TAS5825_M_CODEC_H

#include <Arduino.h>
#include <Wire.h>
#include "../audio/audio_config.h"
#include "tas5825_regs.h"

/**
 * @class TAS5825MCodec
 * @brief TI TAS5825M (smart-amp variant) audio codec driver.
 *
 * Singleton — there is at most one TAS5825M per board. Satisfies the
 * AudioMixer<TI2S, TCodec> contract:
 *     - static TAS5825MCodec& instance()
 *     - bool begin(uint32_t sample_rate = AUDIO_SAMPLE_RATE)
 *     - void reset()
 *     - void setVolume(float)
 *     - void setMute(bool)
 *     - bool isInitialized() const
 *     - const char* getModelName() const
 */
class TAS5825MCodec {
public:
    using Supply = sfx_audio::tas5825::Supply;

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
    bool begin(TwoWire& wire, int sda, int scl,
               uint32_t sample_rate = AUDIO_SAMPLE_RATE,
               Supply supply        = Supply::V20);

    // ─── Phase 2: post-clock activation ────────────────────────────────
    //
    // MUST be called AFTER I2S BCLK + LRCLK are stable. Configures
    // analog gain, clocks, GPIO1 routing, polls FS_MON for a valid
    // sample-rate code, then DEEP_SLEEP → HIZ → PLAY with fault
    // clears around the transitions.
    bool activate();

    // ─── AudioMixer contract ──────────────────────────────────────────
    bool begin(uint32_t sample_rate = AUDIO_SAMPLE_RATE);
    void reset();
    void setVolume(float volume);
    void setMute(bool mute);
    bool isInitialized() const { return initialized_; }
    const char* getModelName() const { return "TAS5825M"; }

    // ─── Volume / mute helpers ────────────────────────────────────────
    void setVolumeDB(float db);

    // ─── Supply voltage runtime change ────────────────────────────────
    bool setSupplyVoltage(Supply voltage);

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
     * Plays a quiet pilot tone, measures DC resistance + AC impedance
     * via IV-sense, stores Re/Le/Qts coefficients in the chip's
     * smart-amp DSP page. Blocks for ~500 ms while calibration runs.
     *
     * Currently a stub that just clears the calibration flag. Will
     * write the actual PPC3 sequence once the speaker model is
     * available in /audio/speaker_model.bin or similar.
     */
    bool calibrateSpeaker();

    /**
     * @brief Read the smart-amp's voice-coil temperature estimate.
     *
     * Returns 0 if smart-amp isn't enabled or never calibrated.
     * Otherwise returns degrees Celsius (typical speaker hot threshold
     * is ~150 °C for a small full-range driver).
     */
    int16_t readSpeakerTemp_C();

    /**
     * @brief Read the smart-amp's diaphragm excursion estimate.
     *
     * Returns peak excursion in millimetres × 100 (so 1.50 mm = 150).
     * Useful for flagging "speaker about to bottom out" conditions.
     */
    uint16_t readSpeakerExcursion_mm100();

    /**
     * @brief Enable / disable just the IV-sense ADCs.
     *
     * This is the lower-level toggle smart-amp uses internally. Can be
     * useful for custom protection schemes that read raw IV samples
     * via SDOUT and apply application-side logic.
     */
    bool setIvSenseEnabled(bool on);

    // ─── Diagnostics / introspection ──────────────────────────────────
    void dumpRegisters();

    // CODEC_STATUS protocol queries
    uint8_t getCodecType() const { return 1; }   // 1 = TAS5825M (per existing protocol)
    bool    getMuted() const { return muted_; }
    uint8_t getVolumeRegister() const { return currentVolume_; }
    Supply  getSupplyVoltage() const { return supply_; }
    int     getSdaPin() const { return sdaPin_; }
    int     getSclPin() const { return sclPin_; }
    uint8_t getDeviceControlRegister();
    uint8_t getFaultRegister();
    bool    testI2CConnection();

#if AUDIO_DEBUG
    bool     testCommunication();
    uint16_t readRegisterCache(uint8_t reg) const;
    bool     writeRegisterDebug(uint8_t reg, uint16_t value);
    void     printStatus();
    void     reinitialize(uint32_t sample_rate = 0);
    void*    getCommunicationInterface() { return i2c_; }
#endif

private:
    TAS5825MCodec();

    TwoWire* i2c_         = nullptr;
    int      sdaPin_      = -1;
    int      sclPin_      = -1;
    uint32_t sampleRate_  = 0;
    Supply   supply_      = Supply::V20;
    bool     initialized_ = false;
    bool     muted_       = false;
    bool     smartAmpOn_  = false;
    uint8_t  currentVolume_ = sfx_audio::tas5825::VOL_0DB;

    // Low-level I/O helpers (shared shape between M and P, but each
    // variant keeps its own copy so the code reads top-to-bottom in
    // one file per chip).
    bool writeRegister(uint8_t reg, uint8_t value);
    bool readRegister(uint8_t reg, uint8_t* value);
    bool selectBookPage(uint8_t book, uint8_t page);

    // M-specific init helpers
    bool waitForFsLock(uint32_t timeoutMs, uint8_t* outFsCode);
    bool initDSPCoefficients();
    bool configureAnalogGain();
};

#endif // TAS5825_M_CODEC_H
