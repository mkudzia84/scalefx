/*
 * Smoke Generator Module - Header
 *
 * Encapsulates the smoke generator hardware: heater relay + PWM fan.
 * Supports two fan modes:
 *   - Constant: fan runs at a fixed speed while active
 *   - Pulsing:  fan pulses to a high speed on each shot for smoke puffs,
 *               then drops to a low idle speed between shots
 *
 * The fan has a configurable spindown delay after firing stops, allowing
 * residual smoke to be pushed out before the fan turns off.
 *
 * Includes built-in current protection (requires INA226 current monitors):
 *   - Disconnect detection: flags channels drawing 0mA while active
 *   - Overcurrent protection: steps fan PWM down when current exceeds
 *     configurable limit; shuts off channel if throttling fails.
 *     Heater (relay) is shut off immediately on overcurrent.
 *
 * Usage:
 *   SmokeGenerator smoke;
 *   smoke.begin(PIN_HEATER, PIN_FAN);
 *   smoke.attachCurrentMonitor(SmokeChannel::Heater, &ina226Heater);
 *   smoke.attachCurrentMonitor(SmokeChannel::Fan,    &ina226Fan);
 *   smoke.setHeater(true);
 *
 *   // Configure fan behavior:
 *   GunFxSmokeConfig cfg;
 *   cfg.fanPulsing = true;
 *   cfg.fanSpeed = 200;
 *   smoke.configure(cfg);
 *
 *   // Start/stop fan (typically wired to muzzle flash firing):
 *   smoke.startFan();
 *   smoke.triggerPulse();   // call per shot
 *   smoke.stopFan(5000);    // spindown delay in ms
 *
 *   // In loop — update() handles fan timing + current protection:
 *   smoke.update();
 */

#ifndef SMOKE_GENERATOR_H
#define SMOKE_GENERATOR_H

#include <Arduino.h>
#include <ina226.h>
#include <serial_gunfx.h>   // GunFxSmokeConfig, SmokeErrorReason

// ============================================================================
// Constants
// ============================================================================

namespace SmokeGeneratorConfig {
    constexpr uint8_t  DEFAULT_SPEED      = 255;
    constexpr uint8_t  DEFAULT_PULSE_HIGH = 255;
    constexpr uint8_t  DEFAULT_PULSE_LOW  = 80;
    constexpr uint16_t DEFAULT_PULSE_MS   = 50;    // Fallback when RPM unknown
    constexpr uint16_t DEFAULT_SPINDOWN_MS = 5000;
    constexpr uint16_t MAX_PULSE_MS       = 10000;
    constexpr uint16_t MAX_SPINDOWN_MS    = 60000;

    // Auto pulse calculation from RPM (when fanPulseMs == 0)
    constexpr uint16_t AUTO_PULSE_MIN_MS  = 20;    // Minimum auto-calculated pulse
    constexpr uint16_t AUTO_PULSE_MAX_MS  = 250;   // Maximum auto-calculated pulse
}

namespace SmokeProtection {
    // Disconnect detection
    constexpr uint16_t ZERO_CURRENT_THRESH_mA  = 5;     // Below this = "zero" current
    constexpr uint16_t ZERO_CURRENT_TIMEOUT_ms = 1000;  // 0mA for this long = disconnected
    constexpr uint16_t STARTUP_IGNORE_ms       = 500;   // Ignore first N ms after turn-on

    // Overcurrent protection (fan PWM stepping)
    constexpr uint16_t OVERCURRENT_DEBOUNCE_ms = 100;   // Must exceed limit for this long
    constexpr uint16_t PWM_STEP_INTERVAL_ms    = 50;    // Step down PWM every N ms
    constexpr uint8_t  PWM_STEP_SIZE           = 10;    // Reduce PWM by this per step
}

// ============================================================================
// Fan Mode
// ============================================================================

enum class SmokeFanMode : uint8_t {
    Constant = 0,
    Pulsing  = 1
};

// ============================================================================
// Smoke Channel
// ============================================================================

enum class SmokeChannel : uint8_t {
    Heater = 0,
    Fan    = 1,
    COUNT  = 2
};

// ============================================================================
// Per-Channel Protection State
// ============================================================================

struct SmokeChannelProtection {
    // Common
    bool     active          = false;  // Channel is currently on
    uint32_t activeStart_ms  = 0;      // When channel was turned on

    // Disconnect detection
    bool     disconnected    = false;  // Confirmed disconnected
    uint32_t zeroStart_ms    = 0;      // When zero-current began
    bool     zeroDetected    = false;  // Currently seeing zero current

    // Overcurrent protection
    uint8_t  cappedDuty      = 255;    // PWM cap (255 = no throttle)
    bool     overCurrent     = false;  // Currently over limit (debounce)
    uint32_t overStart_ms    = 0;      // When overcurrent debounce started
    uint32_t lastStep_ms     = 0;      // Last PWM step-down time
    bool     shutoff         = false;  // Channel shut off due to overcurrent
};

// ============================================================================
// SmokeGenerator Class
// ============================================================================

/**
 * @brief Smoke generator controller (heater relay + PWM fan).
 *
 * Manages heater on/off, fan speed (constant or pulsing mode),
 * and post-firing spindown delay.
 */
class SmokeGenerator {
public:
    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the smoke generator module.
     * @param heaterPin  GPIO pin for the smoke heater relay
     * @param fanPin     GPIO pin for the smoke fan (PWM capable)
     */
    void begin(uint8_t heaterPin, uint8_t fanPin);

    // ========================================================================
    // Heater Control
    // ========================================================================

    /**
     * @brief Enable or disable the smoke heater.
     * @param on  True to enable, false to disable
     */
    void setHeater(bool on);

    bool isHeaterOn() const { return _heaterOn; }

    /**
     * @brief Get cumulative heater-on time in milliseconds.
     */
    uint32_t heaterOnTime_ms() const;

    // ========================================================================
    // Fan Control
    // ========================================================================

    /**
     * @brief Start the fan at the configured speed/mode.
     *
     * In Pulsing mode, starts at pulse_low speed.
     * In Constant mode, starts at configured speed.
     *
     * @param rpm  Current firing rate (used for auto pulse_ms calculation
     *             when fanPulseMs == 0 in config). Pass 0 if unknown.
     */
    void startFan(uint16_t rpm = 0);

    /**
     * @brief Schedule the fan to turn off after a delay.
     * @param delay_ms  Spindown delay in milliseconds (0 = immediate off)
     */
    void stopFan(uint16_t delay_ms);

    /**
     * @brief Trigger a high-speed pulse (Pulsing mode only).
     *
     * Called on each shot to create a smoke puff. Has no effect
     * in Constant mode or if the fan is not running.
     *
     * Pulse duration uses fanPulseMs from config if > 0, otherwise
     * auto-calculates from the RPM set via startFan() (50% duty cycle,
     * clamped to AUTO_PULSE_MIN_MS..AUTO_PULSE_MAX_MS).
     */
    void triggerPulse();

    /**
     * @brief Force the fan off immediately (used in shutdown).
     */
    void forceOff();

    bool isFanOn()      const { return _fanOn; }
    bool isSpinningDown() const { return _pendingOff; }
    uint8_t fanSpeed()  const { return _fanSpeed; }

    /**
     * @brief Get remaining spindown time in milliseconds.
     */
    uint16_t spindownRemaining_ms() const;

    // ========================================================================
    // Current Monitoring
    // ========================================================================

    /**
     * @brief Attach an INA226 current monitor for a channel.
     *
     * Once attached, update() reads current from the monitor and runs
     * disconnect/overcurrent detection automatically.  The caller is
     * responsible for calling INA226::update() each loop iteration to
     * refresh the cached I2C readings (same pattern as GearControl).
     *
     * @param channel  Smoke channel (Heater or Fan)
     * @param monitor  Pointer to an initialized INA226 instance
     */
    void attachCurrentMonitor(SmokeChannel channel, INA226* monitor);

    /**
     * @brief Read current motor draw from the attached INA226.
     * @param channel  Smoke channel (Heater or Fan)
     * @return Absolute current in milliamps (0 if no monitor attached)
     */
    uint16_t readCurrent_mA(SmokeChannel channel) const;

    /**
     * @brief Set the overcurrent protection limit for a channel.
     * @param channel  Smoke channel (Heater or Fan)
     * @param limit_mA  Current limit in milliamps (0=disable protection)
     */
    void setCurrentLimit(SmokeChannel channel, uint16_t limit_mA);

    /**
     * @brief Clear all protection error states and throttle caps.
     */
    void resetErrors();

    /**
     * @brief Get the error reason for a channel (SmokeErrorReason code).
     * @param channel  Smoke channel (Heater or Fan)
     */
    uint8_t errorReason(SmokeChannel channel) const;

    /**
     * @brief Get the effective PWM duty cap for a channel.
     * @param channel  Smoke channel (Heater or Fan)
     * @return 255 = no throttle, lower values = throttled
     */
    uint8_t cappedDuty(SmokeChannel channel) const;

    /**
     * @brief Check if any channel has an active error.
     */
    bool hasError() const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Apply smoke fan configuration.
     * @param cfg  Configuration struct with fan mode and parameters
     *
     * If the fan is currently running while firing, the new config
     * takes effect immediately.
     */
    void configure(const GunFxSmokeConfig& cfg);

    SmokeFanMode fanMode() const { return _fanMode; }
    const GunFxSmokeConfig& config() const { return _cfg; }

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update fan state and current protection (call every loop iteration).
     *
     * Handles spindown timeout, pulse timing, and — if INA226 monitors are
     * attached — disconnect detection and overcurrent protection.
     */
    void update();

    // ========================================================================
    // Shutdown / Reset
    // ========================================================================

    /**
     * @brief Full shutdown — heater off, fan off, reset state.
     */
    void shutdown();

private:
    // Hardware pins
    uint8_t _heaterPin = 0;
    uint8_t _fanPin    = 0;

    // Heater state
    bool     _heaterOn          = false;
    uint32_t _heaterOnStart_ms  = 0;
    uint32_t _totalHeaterOn_ms  = 0;

    // Fan state
    bool     _fanOn       = false;
    uint8_t  _fanSpeed    = 0;
    bool     _pendingOff  = false;
    uint32_t _fanOffTime_ms = 0;

    // Pulse state (Pulsing mode)
    bool     _pulseActive    = false;
    uint32_t _pulseEnd_ms    = 0;
    uint16_t _rpm            = 0;   // Current firing RPM (for auto pulse calc)

    // Configuration
    SmokeFanMode _fanMode = SmokeFanMode::Constant;
    GunFxSmokeConfig _cfg;
    bool _firingActive = false;   // Tracks whether we're in an active firing session

    // Current protection state: [0]=heater, [1]=fan
    INA226*  _currentMonitor[2]   = { nullptr, nullptr };
    SmokeChannelProtection _protection[2];
    uint8_t  _errorReason[2]      = { SmokeErrorReason::NONE, SmokeErrorReason::NONE };
    uint16_t _currentLimit_mA[2]  = { 5000, 3000 };  // Default: heater 5A, fan 3A

    // Internal helpers
    void _setFanPwm(uint8_t duty);
    void _setHeaterPin(bool on);
    void _updateProtection(SmokeChannel channel);
    uint16_t _effectivePulseMs() const;
};

#endif // SMOKE_GENERATOR_H
