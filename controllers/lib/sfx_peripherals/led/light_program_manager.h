/*
 * Light Program Manager — Runtime LED Program Orchestrator
 *
 * Manages LED channel programs loaded from LightProgramConfig.  Handles
 * program selection (manual or input-driven), LED sequence loading,
 * servo configuration, and landing light coordination.
 *
 * Hardware-agnostic: uses ILedManager* for LED operations and a
 * callback struct for servo/landing light operations.  This allows
 * the same manager to work with:
 *   - LightFX standalone (direct hardware via LedManager + ServoControl)
 *   - HubFX master (remote operations via LightFxClient over USB)
 *
 * Usage (LightFX standalone):
 *
 *   LightProgramManager progMgr;
 *
 *   LightProgramManager::Callbacks cb;
 *   cb.onServoConfig = [](uint8_t id, const auto& cfg) {
 *       servos[id - 1].setLimits(cfg.min_us, cfg.max_us);
 *       servos[id - 1].setMotionProfile(cfg.speed, cfg.accel, cfg.decel);
 *       servos[id - 1].setReversed(cfg.reversed);
 *       servos[id - 1].setTarget(cfg.default_us);
 *   };
 *   cb.onLandingBind = [](const auto& lb) {
 *       landingLights[lb.slot - 1].configure(
 *           &servos[lb.servoId - 1],
 *           &ledManager.channel(lb.ledChannel - 1),
 *           lb.brightness);
 *   };
 *   cb.onLandingDeploy  = [](uint8_t slot) { landingLights[slot - 1].deploy(); };
 *   cb.onLandingRetract  = [](uint8_t slot) { landingLights[slot - 1].retract(); };
 *   cb.onLandingUnbind  = [](uint8_t slot) { landingLights[slot - 1].unconfigure(); };
 *
 *   progMgr.begin(&ledManager, cb);
 *   progMgr.loadConfig(config);        // Applies servo settings, binds landing lights
 *   progMgr.selectProgram(0);          // Load first program's LED sequences
 *
 *   // In loop():
 *   progMgr.update(rxInput_us);        // Auto-switch programs based on PWM input bands
 *
 * Usage (HubFX remote):
 *
 *   // Create ILedManager adapter wrapping LightFxClient
 *   RemoteLedAdapter remoteLeds(&lightFxClient);
 *
 *   LightProgramManager::Callbacks cb;
 *   cb.onServoConfig = [&client](uint8_t id, const auto& cfg) {
 *       LightFxServoConfig scfg{...};
 *       client.servoSettings(scfg);
 *   };
 *   // ... etc.
 *
 *   progMgr.begin(&remoteLeds, cb);
 *   progMgr.loadConfig(config);
 */

#ifndef LIGHT_PROGRAM_MANAGER_H
#define LIGHT_PROGRAM_MANAGER_H

#include <Arduino.h>
#include <functional>
#include "led_manager.h"
#include <config/light_program_config.h>

// ============================================================================
// Configuration Constants
// ============================================================================

namespace LightProgramManagerConfig {
    /// Minimum time between automatic program switches (ms)
    constexpr uint32_t SWITCH_DEBOUNCE_MS = 500;
}

// ============================================================================
// LightProgramManager Class
// ============================================================================

/**
 * @brief Runtime orchestrator for LED channel programs.
 *
 * Decouples program logic from hardware access via ILedManager and callbacks.
 * The integration layer (firmware or hub controller) provides the hardware
 * bindings at initialization time.
 */
class LightProgramManager {
public:

    // ========================================================================
    // Callback Types
    // ========================================================================

    /**
     * @brief Hardware abstraction callbacks for servo and landing light operations.
     *
     * Set the callbacks that match your hardware context before calling begin().
     * Unset callbacks are silently skipped.
     */
    struct Callbacks {
        /// Called once per configured servo when loadConfig() applies servo settings.
        /// @param servoId  1-based servo ID
        /// @param cfg      Full servo configuration from YAML
        using ServoConfigFn = std::function<void(
            uint8_t servoId, const LightProgramConfig::ServoSetup& cfg)>;

        /// Called once per landing group binding when loadConfig() applies bindings.
        /// @param groupIndex  0-based group index
        /// @param group       Full landing group configuration from YAML
        using LandingBindFn = std::function<void(
            uint8_t groupIndex, const LightProgramConfig::LandingGroup& group)>;

        /// Called for landing light deploy/retract/unbind actions.
        /// @param slot  1-based landing light slot
        using LandingActionFn = std::function<void(uint8_t slot)>;

        ServoConfigFn   onServoConfig;
        LandingBindFn   onLandingBind;
        LandingActionFn onLandingUnbind;
        LandingActionFn onLandingDeploy;
        LandingActionFn onLandingRetract;
    };

    // ========================================================================
    // Lifecycle
    // ========================================================================

    LightProgramManager() = default;

    /**
     * @brief Initialize the program manager.
     *
     * @param leds      LED manager for sequence operations (must outlive this object)
     * @param callbacks Hardware callbacks for servo/landing light operations
     */
    void begin(ILedManager* leds, const Callbacks& callbacks);

    /**
     * @brief Load configuration and apply hardware settings.
     *
     * Applies master brightness, configures servos, and binds landing lights
     * according to the loaded config.  Does NOT select a program — call
     * selectProgram() or let update() handle input-driven selection.
     *
     * @param cfg  Configuration loaded from YAML
     * @return true if config was loaded successfully
     */
    bool loadConfig(const LightProgramConfig& cfg);

    // ========================================================================
    // Program Control
    // ========================================================================

    /**
     * @brief Select and activate a program by index.
     *
     * Stops all running sequences, loads the program's channel events into
     * the LED manager, and deploys/retracts landing lights as specified.
     *
     * @param index  Program index (0-based, must be < programCount())
     */
    void selectProgram(uint8_t index);

    /**
     * @brief Update input-driven program selection.
     *
     * Reads the current RX input value and switches programs when the
     * input enters a different band.  Includes debouncing to prevent
     * rapid switching from noisy signals.
     *
     * No-op if no input bands are configured (manual selection only).
     *
     * @param rxInput_us  Current RX channel pulse width (µs)
     */
    void update(uint16_t rxInput_us);

    // ========================================================================
    // Accessors
    // ========================================================================

    /** @brief Index of the currently active program (-1 = none) */
    int8_t activeProgram() const { return _activeProgram; }

    /** @brief Number of programs in the loaded config */
    uint8_t programCount() const { return _config.programCount; }

    /** @brief Name of a program by index (empty string if invalid) */
    const char* programName(uint8_t index) const;

    /** @brief Whether a config has been loaded */
    bool isConfigLoaded() const { return _configLoaded; }

    /** @brief Direct access to the loaded config */
    const LightProgramConfig& config() const { return _config; }

private:
    ILedManager*        _leds = nullptr;
    Callbacks           _callbacks;
    LightProgramConfig  _config;
    int8_t              _activeProgram = -1;
    bool                _configLoaded  = false;
    uint32_t            _lastSwitchTime_ms = 0;

    // ---- Internal helpers ----

    /// Apply servo configuration from loaded config
    void applyServoConfig();

    /// Apply landing light bindings from loaded config
    void applyLandingBindings();

    /// Load all channel events for a program into the LED manager
    void loadProgramEvents(const LightProgramConfig::Program& program);

    /// Load events for a single channel
    void loadChannelEvents(uint8_t ch,
                           const LightProgramConfig::ChannelDef& chDef);

    /// Apply landing group policies from a program (deploy/retract per group)
    void applyGroupPolicies(const LightProgramConfig::Program& program);

    /// Find the program index matching a PWM band (-1 if none)
    int8_t findProgramForBand(uint16_t us) const;

    /// Convert event type string to LightFxEventType constant
    /// @return event type value, or 0xFF if unrecognized
    static uint8_t parseEventType(const char* typeName);
};

#endif // LIGHT_PROGRAM_MANAGER_H
