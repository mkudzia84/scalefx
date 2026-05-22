/*
 * PCA9685 — 16-channel 12-bit I²C PWM driver
 *
 * NXP PCA9685 / PCA9685BS — single-master I²C PWM controller, 16 channels,
 * 12-bit duty per channel, configurable PWM frequency (24..1526 Hz),
 * push-pull or open-drain output. Hardware ALL_LED broadcast register
 * for atomic multi-channel updates.
 *
 * Surface this driver provides:
 *
 *   Native 12-bit API (preferred for new code):
 *     setChannel(ch, duty12)        — single channel, 0..4095
 *     setAllChannels(duty12)        — all 16 channels via ALL_LED broadcast
 *     setFrequency(freq_Hz)         — PRESCALE write (sleep / set / wake)
 *     wake(), sleep()               — power state
 *
 *   PwmOutput concept (pwm_output.h) — drop-in for LedControlT / LedManager:
 *     static constexpr uint8_t NUM_PINS = 16
 *     static constexpr bool    HAS_HW_PWM = true
 *     setPinDirection(pin, isInput)  — no-op (PCA9685 outputs only)
 *     setLedBrightness(pin, 0..255)  — 8-bit input scaled to 12-bit duty
 *     writePin(pin, high)            — convenience: brightness 0 or 255
 *     isAvailable()                  — from I2CDevice base
 *
 * Conservative init flow (used by begin()):
 *   1. I²C general-call SWRST (NXP UM10851 §7.1.4) — write 0x06 to 0x00.
 *      Recovers a chip that's silent at its assigned address.
 *   2. MODE1 ← SLEEP | ALLCALL — required before PRESCALE write (§7.3.5).
 *   3. PRESCALE ← target frequency (default ~1526 Hz, prescale 0x03 — chip
 *      max, sub-flicker for human eyes).
 *   4. MODE2 ← OUTDRV (push-pull, recommended for MOSFET-gate drive).
 *   5. MODE1 ← AI | ALLCALL — wake, enable auto-increment for burst writes.
 *      Then 500 µs settle (§7.3.1.1) and MODE1 ← AI | ALLCALL | RESTART.
 *   6. ALL_LED ← all off.
 *
 * Duty=0 uses the chip's FULL_OFF flag (LEDn_OFF_H[4] = 1) for hard-
 * zero output; duty in [1, 4095] uses plain PWM mode (ON=0, OFF=duty)
 * with no flag bits set.  Earlier revs of this driver clamped duty<1
 * up to OFF=1 to avoid the "ON==OFF undefined behaviour" trap, but
 * the resulting ~0.024 % duty leak was visible as a dim glow on the
 * HubFX rail LEDs through their MOSFET pre-drivers.  FULL_OFF takes
 * precedence over the LEDn_OFF count (UM10851 §7.3.3 Note 2) so the
 * output is hard-zero with no per-cycle pulse.  FULL_ON is never set —
 * duty=4095 produces 99.976 % duty which is visually identical to
 * full-on, and avoids the documented FULL_ON ↔ PWM transition glitch
 * on some silicon revs.
 *
 * Hardware notes:
 *   - 7-bit I²C address range: 0x40..0x7F (6 strap pins A0..A5).
 *   - VDD: 2.3..5.5 V. Outputs are at VDD level (push-pull) — drive
 *     MOSFET gates directly when VDD = 3V3 or 5V.
 *   - OE/ pin must be LOW for outputs to be active. The chip will ACK
 *     I²C and accept register writes with OE/ HIGH, but outputs stay
 *     tri-stated.
 *   - Internal 25 MHz oscillator. External clock available on EXTCLK pin.
 *
 * Bring-up + latch-up recovery procedure documented in:
 *   tests/hw/hubfx_pca9685_hwtest/README.md
 *
 * Reference implementations cross-checked:
 *   - NXP UM10851 PCA9685 datasheet, Rev. 4 (16 April 2015)
 *   - Linux kernel drivers/pwm/pwm-pca9685.c (register definitions)
 */

#ifndef PCA9685_H
#define PCA9685_H

#include "../power/i2c_device.h"
#include "pwm_output.h"

// ============================================================================
// I²C address namespace
// ============================================================================

namespace PCA9685Address {
    constexpr uint8_t MIN = 0x40;
    constexpr uint8_t MAX = 0x7F;

    // Common configurations.
    constexpr uint8_t BASE          = 0x40;  // all strap pins LOW
    constexpr uint8_t DEFAULT_ADDR  = 0x40;
    constexpr uint8_t HUBFX_ADDR    = 0x70;  // HubFX 8-channel rev — A4..A6 HIGH

    /// I²C general-call address (used for the broadcast SWRST).
    constexpr uint8_t GENERAL_CALL  = 0x00;

    constexpr bool isValid(uint8_t addr) {
        return addr >= MIN && addr <= MAX;
    }
}

// ============================================================================
// Register map (NXP UM10851 §7.3)
// ============================================================================

namespace PCA9685Reg {
    // Control registers.
    constexpr uint8_t MODE1            = 0x00;
    constexpr uint8_t MODE2            = 0x01;
    constexpr uint8_t SUBADR1          = 0x02;
    constexpr uint8_t SUBADR2          = 0x03;
    constexpr uint8_t SUBADR3          = 0x04;
    constexpr uint8_t ALLCALLADR       = 0x05;

    // Per-channel LED registers — 4 bytes per channel, n in [0..15].
    //   LEDn_ON_L  = 0x06 + 4n
    //   LEDn_ON_H  = 0x07 + 4n
    //   LEDn_OFF_L = 0x08 + 4n
    //   LEDn_OFF_H = 0x09 + 4n
    constexpr uint8_t LED0_ON_L        = 0x06;

    // Broadcast registers — writes affect all 16 channels.
    constexpr uint8_t ALL_LED_ON_L     = 0xFA;
    constexpr uint8_t ALL_LED_ON_H     = 0xFB;
    constexpr uint8_t ALL_LED_OFF_L    = 0xFC;
    constexpr uint8_t ALL_LED_OFF_H    = 0xFD;

    constexpr uint8_t PRESCALE         = 0xFE;
    constexpr uint8_t TESTMODE         = 0xFF;

    // MODE1 bits (datasheet §7.3.1, Table 5).
    constexpr uint8_t MODE1_RESTART    = 0x80;
    constexpr uint8_t MODE1_EXTCLK     = 0x40;
    constexpr uint8_t MODE1_AI         = 0x20;  // auto-increment register pointer
    constexpr uint8_t MODE1_SLEEP      = 0x10;  // oscillator off
    constexpr uint8_t MODE1_SUB1       = 0x08;
    constexpr uint8_t MODE1_SUB2       = 0x04;
    constexpr uint8_t MODE1_SUB3       = 0x02;
    constexpr uint8_t MODE1_ALLCALL    = 0x01;  // ACK the ALL_LED-CALL address

    // MODE2 bits (datasheet §7.3.2, Table 6).
    constexpr uint8_t MODE2_INVRT      = 0x10;
    constexpr uint8_t MODE2_OCH        = 0x08;  // outputs change on ACK (0=on STOP)
    constexpr uint8_t MODE2_OUTDRV     = 0x04;  // 1 = push-pull, 0 = open-drain

    // LEDn_ON_H / LEDn_OFF_H bit 4: FULL_ON / FULL_OFF flag. We never set
    // these during normal operation — see writeChannelRaw().
    constexpr uint8_t LED_FULL         = 0x10;

    // POR defaults (Datasheet §7.3.1.1 / §7.3.2 / §7.3.5).
    constexpr uint8_t POR_MODE1        = 0x11;  // SLEEP + ALLCALL
    constexpr uint8_t POR_MODE2        = 0x04;  // OUTDRV
    constexpr uint8_t POR_PRESCALE     = 0x1E;  // ~200 Hz

    // General-call SWRST data byte (datasheet §7.1.4).
    constexpr uint8_t SWRST_BYTE       = 0x06;
}

// ============================================================================
// PCA9685 class
// ============================================================================

class PCA9685 : public I2CDevice {
public:
    /// Total channel count.
    static constexpr uint8_t NUM_PINS = 16;

    /// PCA9685 always drives hardware PWM.
    static constexpr bool HAS_HW_PWM = true;

    /// Maximum 12-bit duty value.
    static constexpr uint16_t DUTY_MAX = 4095;

    /// Chip's internal oscillator frequency.
    static constexpr uint32_t INTERNAL_OSC_HZ = 25000000;

    /// Default PWM frequency on this driver — chip's maximum, above the
    /// human flicker-fusion threshold. Override via setFrequency().
    static constexpr uint16_t DEFAULT_PWM_HZ = 1526;

    PCA9685() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Bring up the chip on the I²C bus.
     *
     * Probes the address, issues a general-call SWRST (also resets any
     * stuck chip whose address comparator is wedged), then configures
     * PRESCALE for `freqHz`, MODE2 = push-pull OUTDRV, and wakes the
     * chip with MODE1.AI = 1 so burst writes work. Parks all channels
     * at full-off.
     *
     * @param wire     Active TwoWire instance.
     * @param address  7-bit slave address (default `0x40`).
     * @param freqHz   Target PWM frequency, 24..1526 Hz (default 1526).
     */
    bool begin(TwoWire& wire,
               uint8_t  address = PCA9685Address::DEFAULT_ADDR,
               uint16_t freqHz  = DEFAULT_PWM_HZ);

    /**
     * @brief I²C general-call SWRST — datasheet §7.1.4.
     *
     * Writes 0x06 to 7-bit address 0x00. Resets every PCA9685 on the
     * bus that has ALLCALL enabled (POR default) to POR state, even if
     * the chip is silent at its assigned address. Other devices on the
     * bus ignore the transaction (general-call ACK is opt-in per chip).
     *
     * Static method — usable without an instance, e.g. for recovery at
     * boot before the driver has been constructed.
     */
    static bool broadcastReset(TwoWire& wire);

    /// I2CDevice override — confirms the chip is alive at its address.
    /// PCA9685 has no chip-ID register, so this is just an ACK probe.
    bool identify() override;

    /// Software reset of THIS device via the broadcast path.
    /// Cheaper than re-running begin() when you just want POR defaults.
    void reset();

    // ========================================================================
    // PWM frequency / mode
    // ========================================================================

    /**
     * @brief Set the PWM frequency.
     *
     * Sequence: enter SLEEP (PRESCALE only writable while SLEEP=1 per
     * datasheet §7.3.5), write PRESCALE, exit SLEEP, wait 500 µs for
     * oscillator stabilisation (§7.3.1.1), trigger RESTART.
     *
     * @param freqHz Target frequency, 24..1526 Hz. Clamped to range.
     * @return Actual frequency the chip is programmed to (may differ
     *         from request due to prescale rounding).
     */
    uint16_t setFrequency(uint16_t freqHz);

    /// Current PWM frequency, computed from the last PRESCALE write.
    uint16_t frequency() const { return _frequencyHz; }

    /// Put the oscillator to sleep (outputs stop). Returns to the wake
    /// state via wake() — the PRESCALE / channel registers retain their
    /// values across sleep.
    bool sleep();

    /// Wake the oscillator. Issues a RESTART so paused outputs resume
    /// from their previous duty.
    bool wake();

    // ========================================================================
    // Native 12-bit channel API
    // ========================================================================

    /**
     * @brief Write a 12-bit duty value to one channel.
     *
     * Burst-writes the 4-byte LEDn_ON_L..LEDn_OFF_H group (MODE1.AI=1).
     * Always uses plain PWM mode (ON=0, OFF=duty). Never sets the
     * FULL_ON / FULL_OFF flag bits.
     *
     * Duty is clamped to [1, DUTY_MAX]. At duty=DUTY_MAX the output is
     * HIGH for 4095 of 4096 ticks per cycle (visually identical to
     * FULL_ON). At duty=0 the caller's intent is "off", but writing
     * ON=OFF=0 is undefined behaviour per the datasheet, so we clamp to
     * 1 — a single-tick HIGH pulse per cycle (~160 ns at 1526 Hz, far
     * below visual perception).
     *
     * @param channel  Channel index 0..15.
     * @param duty12   12-bit duty 0..4095.
     */
    bool setChannel(uint8_t channel, uint16_t duty12);

    /**
     * @brief Write a 12-bit duty value to ALL 16 channels in one
     *        atomic I²C transaction.
     *
     * Uses the ALL_LED_* broadcast registers (0xFA..0xFD). All 16
     * outputs latch the new value at the same I²C STOP (MODE2.OCH=0),
     * so the channels stay phase-locked. Same clamping as setChannel().
     */
    bool setAllChannels(uint16_t duty12);

    // ========================================================================
    // PwmOutput concept surface
    // ========================================================================

    /**
     * @brief No-op — PCA9685 channels are always outputs. Returns true
     *        when the chip is available, false otherwise, for consistent
     *        error reporting at the call site.
     */
    bool setPinDirection(uint8_t pin, bool isInput);

    /**
     * @brief Set 8-bit brightness/duty on a channel, scaled to 12-bit.
     *
     * Adapter for the PwmOutput concept. The 0..255 input is mapped
     * linearly to [1..DUTY_MAX] via:
     *
     *   duty12 = brightness == 0 ? 1 : (brightness * 4095 + 127) / 255
     *
     * which gives an exact 0→1 (sub-visible) and 255→4095 (full PWM).
     *
     * Same call is used by the LED runtime AND by any caller emitting
     * generic PWM (servo pulse width at 50 Hz, motor duty, etc.) — only
     * the per-board PWM frequency picked at begin()/setFrequency() time
     * changes the semantic meaning of the duty value.
     *
     * @param pin        Channel index 0..15.
     * @param brightness 8-bit duty, 0=off, 255=full.
     */
    bool setLedBrightness(uint8_t pin, uint8_t brightness);

    /**
     * @brief Convenience: drive a channel HIGH (brightness 255) or
     *        LOW (brightness 0). Implemented via setLedBrightness().
     */
    bool writePin(uint8_t pin, bool high);

    // address(), isAvailable(), errorCount(), lastError() are inherited
    // from I2CDevice.

private:
    /// Compute the PRESCALE register value for a target frequency.
    /// Datasheet §7.3.5: prescale = round(25 MHz / (4096 × freq)) − 1.
    /// Clamps to the 3..255 range.
    static uint8_t computePrescale(uint16_t freqHz);

    /// 4-byte burst write at `firstReg`. Used for both per-channel and
    /// ALL_LED transactions. Auto-increment must be enabled (MODE1.AI=1)
    /// for the chip's register pointer to walk through the 4 bytes.
    bool writeBurst4(uint8_t firstReg,
                     uint8_t on_l, uint8_t on_h,
                     uint8_t off_l, uint8_t off_h);

    /// Write a 12-bit duty starting at `firstReg`. Handles the clamping
    /// to [1, DUTY_MAX] and constructs the 4 register bytes.
    bool writeChannelRaw(uint8_t firstReg, uint16_t duty12);

    uint16_t _frequencyHz = 0;
};

#endif // PCA9685_H
