/**
 * LightFX Pico Controller v0.9.0
 * 
 * Server controller for lighting effects - receives commands from HubFX over USB serial.
 * Controls: 8-channel LED outputs with sequences, 3 servos.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * Architecture (Chain of Responsibility):
 *   - CoreCommandServer: Handles INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE
 *   - LightFxServer: Handles LED, SERVO, POWER commands
 *   - CommandRouter: Routes packets to handlers in priority order
 * 
 * Pin Assignments:
 *   LED Channels: GPIO0-7 (8 channels, PWM capable)
 *     CH1=GP0  CH2=GP1  CH3=GP2  CH4=GP3
 *     CH5=GP4  CH6=GP5  CH7=GP6  CH8=GP7
 *   Servos:       GPIO8 (SRV1), GPIO9 (SRV2), GPIO10 (SRV3)
 *     SRV1 (GP8) is an RC PWM input in every non-slave state (IDLE /
 *     STANDALONE / DIRECT) so Studio can always visualize the live pulse
 *     while the board is being configured. In STANDALONE the pulse width
 *     is matched against the configured input bands to auto-switch the
 *     active program; in DIRECT the capture is display-only (explicit
 *     commands drive program selection). In SLAVE mode GP8 becomes a
 *     normal servo output (HubFX may route servo commands there).
 *     SRV2 / SRV3 stay regular servo outputs in every mode.
 *   Indicator LEDs: GPIO24 (connection), GPIO25 (error)
 *   Battery ADC: GPIO29 (÷6.18 divider)
 */

#include <Arduino.h>
#include <Servo.h>
#include <serial/serial.h>
#include <lightfx/server/lightfx_server.h>
#include <led/led_manager.h>
#include <led/light_program_manager.h>
#include <servo/srv_control.h>
#include <pwm/pwm_control.h>
#include <server/sfx_server.h>
#include <power/battery_monitor.h>
#include <power/battery_server.h>
#include <storage/flash.h>
#include <storage/storage_config_bridge.h>
#include <config/config_store.h>
#include <server/config_server.h>
#include <server/storage_server.h>
#include "config/lightfx_config.h"
#include "landing_light.h"

// ============================================================================
//  FIRMWARE INFO
// ============================================================================

#define FIRMWARE_VERSION "0.17.1"
#define BUILD_NUMBER 72

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

// LED Output Channels (active high, PWM capable)
const uint8_t PIN_LED_CH1 = 0;
const uint8_t PIN_LED_CH2 = 1;
const uint8_t PIN_LED_CH3 = 2;
const uint8_t PIN_LED_CH4 = 3;
const uint8_t PIN_LED_CH5 = 4;
const uint8_t PIN_LED_CH6 = 5;
const uint8_t PIN_LED_CH7 = 6;
const uint8_t PIN_LED_CH8 = 7;

// Servos
const uint8_t PIN_SERVO_1 = 8;
const uint8_t PIN_SERVO_2 = 9;
const uint8_t PIN_SERVO_3 = 10;

// Indicator LEDs
const uint8_t PIN_LED_CONN = 24;
const uint8_t PIN_LED_ERR  = 25;

// Battery voltage ADC
const uint8_t PIN_VSENSE = 29;

// Array of LED channel pins
const uint8_t LED_CHANNEL_PINS[8] = {
    PIN_LED_CH1, PIN_LED_CH2, PIN_LED_CH3, PIN_LED_CH4,
    PIN_LED_CH5, PIN_LED_CH6, PIN_LED_CH7, PIN_LED_CH8
};

// ============================================================================
//  CONSTANTS
// ============================================================================

const uint8_t LED_CHANNEL_COUNT = 8;
const uint8_t LANDING_LIGHT_COUNT = 3;

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;
const int SERVO_DEFAULT_MAX_SPEED  = 4000;
const int SERVO_DEFAULT_ACCEL      = 8000;
const int SERVO_DEFAULT_DECEL      = 8000;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
SfxServer server;
LightFxServer lightfxServer;
// Battery voltage monitor (ADC with ÷6.18 effective divider).
// Calibrated 2026-04-23 against a 7.60 V pack: ÷5.1 read 6.27 V, ÷6.1 read
// 7.50 V, ÷6.18 reads 7.60 V. Resistors are nominally 51k/10k; the 0.08
// offset absorbs ADC + resistor tolerance for a 1:1 balancer match.
// Plus generic core BATTERY_CONFIG (0xEE) handler.
using BatteryT = AdcDividerBatteryT<6180>;
BatteryT batteryMonitor;
BatteryServerT<BatteryT> batteryServer(batteryMonitor);

// Config server (handles CONFIG_RELOAD/STATUS/SAVE protocol + owns ConfigStore)
// LightFxYamlPool sizes the parser for Studio-emitted configs that include
// programs/groups/bindings — the firmware ignores those fields but the parser
// still allocates a node per key while scanning. See lightfx_config.h.
using LightFxConfigStore = ConfigStore<LightFxConfigSchema, LightFxYamlPool>;
ConfigServerT<LightFxConfigStore> configServer;

// Storage server (handles FILE_LIST/DOWNLOAD/UPLOAD for LittleFS flash — lets
// Studio read/write /lightfx.yaml directly over the wire). Only TARGET_FLASH
// is meaningful here — no SD card on LightFX Pico.
StorageServer storageServer;

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Connection state managed by IndicatorLedManager (indicators)

// LED channel manager (8 GPIO channels, PWM-capable)
LedManager<8> ledManager;

// Servos with motion profiling
ServoControl servos[3];

// Landing light sequencers (bind servo + LED channel)
LandingLight landingLights[LANDING_LIGHT_COUNT];

// Light program runtime — owns the loaded LightProgramConfig and runs program
// switching (manual via LIGHT_PROGRAM_SELECT, automatic via input bands later
// in Phase 0). LED-sequence loading lives here; servo / landing-bind hardware
// access is delegated through callbacks wired in setup().
LightProgramManager progMgr;

// Battery — monitor is always on; defaults: LiPo, cell count auto-detected.
// BATTERY_CONFIG only adjusts chemistry/cells; the auto-cutoff reaction
// (soft-disable all 8 LED channels on low voltage) is YAML-configured plus
// runtime-toggleable via BATTERY_AUTO_CUTOFF (0x5E).
bool autoCutoffOnLowVoltage = true;   // Safer default — real prop is battery-powered
bool lowVoltageTriggered    = false;  // Set when cutoff fires (persists until reset)

// SRV1 (GP8) is dual-role: servo output by default, RC PWM input when the
// board is in STANDALONE state. Transitions are detected by polling
// server.core().boardState() in loop(); see updateSrv1Role().
PwmInput  srv1Input;
uint16_t  liveInput_us     = 0;     // Last captured RC pulse (0 = no signal)
uint8_t   lastBoardState   = 0xFF;  // Sentinel: force first-tick role sync

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void performSafeShutdown();
void performSafeInit();
void setServoPulse(uint8_t servo_id, int pulse_us);
void setupLightFxCallbacks();
void setupLightProgramManager();
void updateLandingLights();
static void applyConfig(const LightFxConfig& cfg);
static void disableAllLedChannels();
static void updateSrv1Role();

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void performSafeShutdown() {
    SFX_LOG_INFO("Shutdown — LEDs off, servos center, landing lights off");

    // Shutdown landing lights first (turns off LEDs)
    for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
        landingLights[i].shutdown();
    }

    // Stop all LED sequences, turn off LEDs, reset master brightness
    ledManager.shutdown();

    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }

    // Battery monitor itself keeps running — only the auto-cutoff reaction is
    // disarmed on shutdown so it can't fire while the host is gone.
    autoCutoffOnLowVoltage = false;
    lowVoltageTriggered    = false;
}

// Soft-disable all 8 LED channels (battery low-voltage cutoff). Channels stay
// disabled until LED_RESET / LED_ENABLE re-arms them, so the user gets an
// explicit acknowledgement that battery levels are back to safe.
static void disableAllLedChannels() {
    for (uint8_t ch = 1; ch <= LED_CHANNEL_COUNT; ch++) {
        ledManager.enableChannel(ch, false);
    }
}

// Re-role SRV1 (GP8) when the board state changes:
//   IDLE / STANDALONE / DIRECT → RC PWM input (always captured so the GUI can
//                                see the live pulse while the user configures
//                                the board; program auto-select only fires in
//                                STANDALONE — see the loop() gate).
//   SLAVE                      → servo output (HubFX may route servo commands
//                                to this channel).
// Polling the state each loop is cheap — transitions fire detach/attach which
// under the hood just writes a PWM slice register, and the role rarely changes
// in practice (INIT packet or post-shutdown revert).
//
// Reattaching preserves the stored min/max/motion-profile so YAML-applied or
// CLI-configured limits survive the role flip — wiping them back to defaults
// silently narrowed the travel range and was the cause of "servo won't go
// below 1600µs" after an init cycle.
static void updateSrv1Role() {
    uint8_t state = server.core().boardState();
    if (state == lastBoardState) return;

    const bool wantInput = (state != BoardState::SLAVE);
    if (wantInput) {
        servos[0].end();
        srv1Input.begin(PwmInputType::Pwm, PIN_SERVO_1);
        liveInput_us = 0;
        SFX_LOG_INFO("SRV1 (GP8): RC PWM input (%s)", BoardState::getName(state));
    } else {
        if (srv1Input.isEnabled()) srv1Input.end();
        // Preserve existing limits/profile — end()/setters kept the stored
        // values; begin() resets _positionUs/_targetUs but re-reads the
        // _minUs/_maxUs already in the instance.
        const int keepMin  = servos[0].minLimit();
        const int keepMax  = servos[0].maxLimit();
        const int startUs  = constrain((int)SERVO_DEFAULT_US, keepMin, keepMax);
        servos[0].begin(PIN_SERVO_1, keepMin, keepMax, startUs);
        liveInput_us = 0;
        SFX_LOG_INFO("SRV1 (GP8): servo output (%s) [%d-%dµs]",
                     BoardState::getName(state), keepMin, keepMax);
    }
    lastBoardState = state;
}

/**
 * @brief Apply loaded YAML config to hardware.
 *
 * Two-stage:
 *   1. Device-only fields (battery monitoring) applied directly.
 *   2. LightProgramConfig section handed to LightProgramManager, which
 *      configures servos + binds landing groups via callbacks. Program
 *      activation is separate — selectProgram() is only invoked when
 *      LIGHT_PROGRAM_SELECT (or the input layer in Phase 0) asks for it.
 *
 * Called once on boot after flash-load, and again on `config.reload`.
 */
static void applyConfig(const LightFxConfig& cfg) {
    SFX_LOG_INFO("Applying config to hardware");

    // ---- Battery monitoring (device-only — outside program scope) ----
    BatteryChemistry chem = parseBatteryChemistry(cfg.battery.chemistry);
    batteryMonitor.setChemistry(chem);
    batteryMonitor.setCellCount(cfg.battery.cellCount);  // 0 = auto-detect
    autoCutoffOnLowVoltage = cfg.battery.autoCutoff;

    SFX_LOG_INFO("Battery: chem=%s cells=%s autoCutoff=%u",
                 cfg.battery.chemistry,
                 cfg.battery.cellCount == 0 ? "auto" : "fixed",
                 cfg.battery.autoCutoff);

    // ---- Light program (master brightness + servos + landing bindings) ----
    if (progMgr.loadConfig(cfg.lightProgram)) {
        SFX_LOG_INFO("LightProgram loaded: master=%u%% servos=%u landingGroups=%u programs=%u",
                     cfg.lightProgram.masterBrightness,
                     cfg.lightProgram.servoCount,
                     cfg.lightProgram.landingGroupCount,
                     cfg.lightProgram.programCount);
    }
}

void performSafeInit() {
    SFX_LOG_INFO("Init — safe reset");

    // Init resets everything to a known safe state
    performSafeShutdown();
    
    // Re-enable all LED channels
    ledManager.init();
}

// ============================================================================
//  SERVO CONTROL
// ============================================================================

void setServoPulse(uint8_t servo_id, int pulse_us) {
    if (servo_id < 1 || servo_id > 3) return;
    servos[servo_id - 1].setTarget(pulse_us);
}

void updateServos() {
    for (int i = 0; i < 3; i++) {
        servos[i].update();
    }
}

void updateLandingLights() {
    for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
        landingLights[i].update();
    }
}

// ============================================================================
//  LIGHTFX CALLBACKS SETUP
// ============================================================================

void setupLightFxCallbacks() {
    // SERVO_SET callback (servo ID validated by LightFxSpec before dispatch)
    lightfxServer.onServoSet([](uint8_t id, int pulseUs) -> uint8_t {
        servos[id - 1].setTarget(pulseUs);
        return LightFxError::OK;
    });
    
    // SERVO_SETTINGS callback (servo ID validated by LightFxSpec before dispatch).
    // Fields are all unsigned in the wire format, so the old `>= 0` checks were
    // always-true no-ops — call the setters unconditionally. ServoControl's
    // setMaxSpeed/setAcceleration/setDeceleration already treat 0 as "keep
    // current"; setLimits rejects minUs>=maxUs.
    lightfxServer.onServoSettings([](const LightFxServoConfig& cfg) -> uint8_t {
        ServoControl& servo = servos[cfg.servoId - 1];

        // setLimits will snap _positionUs into the new range and immediately
        // write the clamped pulse to the servo — so narrowing the range from
        // the calibration dialog instantly jumps the servo to the nearest
        // valid endpoint. Expected behavior; called out to explain any
        // perceived "stuck at min/max" after Save.
        servo.setLimits(cfg.minUs, cfg.maxUs);
        servo.setMaxSpeed(cfg.maxSpeedUsPerSec);
        servo.setAcceleration(cfg.maxAccelUsPerSec2);
        servo.setDeceleration(cfg.maxDecelUsPerSec2);
        servo.setReversed(cfg.reversed);
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_BIND callback
    // Wire format: [slot:u8][servoId:u8][channelMask:u8][brightness:u8]
    //   servoId=0: no servo (LED-only group)
    //   channelMask: bitmask, bit0=ch1 .. bit7=ch8
    lightfxServer.onLandingLightBind([](uint8_t slot, uint8_t servoId, uint8_t channelMask,
                                        uint8_t brightness) -> uint8_t {
        // Decode channelMask to LedControl* array
        LedControl* leds[LandingLight::MAX_LEDS];
        uint8_t ledCount = 0;
        for (uint8_t bit = 0; bit < LED_CHANNEL_COUNT; bit++) {
            if (channelMask & (1 << bit)) {
                leds[ledCount++] = &ledManager.channel(bit);
            }
        }
        if (ledCount == 0) return LightFxError::INVALID_CHANNEL;

        // Servo pointer (nullptr if servoId == 0)
        ServoControl* servo = (servoId > 0) ? &servos[servoId - 1] : nullptr;

        landingLights[slot - 1].setSlot(slot);
        landingLights[slot - 1].configure(servo, leds, ledCount, brightness);

        // Register progress callback (emits LANDING_LIGHT_STATUS packets)
        landingLights[slot - 1].onProgress([](const LightFxLandingLightStatus& status) {
            lightfxServer.sendLandingLightStatus(status);
        });
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_UNBIND callback
    lightfxServer.onLandingLightUnbind([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                landingLights[i].unconfigure();
            }
        } else {
            landingLights[slot - 1].unconfigure();
        }
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_DEPLOY callback
    lightfxServer.onLandingLightDeploy([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                if (landingLights[i].isConfigured()) landingLights[i].deploy();
            }
        } else {
            if (!landingLights[slot - 1].isConfigured()) return LightFxError::INVALID_SLOT;
            landingLights[slot - 1].deploy();
        }
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_RETRACT callback
    lightfxServer.onLandingLightRetract([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                if (landingLights[i].isConfigured()) landingLights[i].retract();
            }
        } else {
            if (!landingLights[slot - 1].isConfigured()) return LightFxError::INVALID_SLOT;
            landingLights[slot - 1].retract();
        }
        return LightFxError::OK;
    });

    // BATTERY_AUTO_CUTOFF callback (runtime toggle of low-voltage LED cutoff)
    lightfxServer.onBatteryAutoCutoff([](bool enabled) -> uint8_t {
        autoCutoffOnLowVoltage = enabled;
        if (!enabled) lowVoltageTriggered = false;  // Re-arm
        SFX_LOG_INFO("Battery auto-cutoff %s", enabled ? "ENABLED" : "DISABLED");
        return LightFxError::OK;
    });

    // LIGHT_PROGRAM_SELECT — activate program N (0-based) from /lightfx.yaml.
    // Rejected with INVALID_PROGRAM if no config has been loaded yet or the
    // index is past programCount(); Studio / hub treat that as "this slave
    // has no program N defined".
    lightfxServer.onLightProgramSelect([](uint8_t programIndex) -> uint8_t {
        if (!progMgr.isConfigLoaded()) return LightFxError::INVALID_PROGRAM;
        if (programIndex >= progMgr.programCount()) return LightFxError::INVALID_PROGRAM;
        progMgr.selectProgram(programIndex);
        SFX_LOG_INFO("LightProgram → %u (%s)", programIndex, progMgr.programName(programIndex));
        return LightFxError::OK;
    });

    // LIGHT_PROGRAM_RESET — stop sequences, retract all groups, no active program.
    lightfxServer.onLightProgramReset([]() -> uint8_t {
        progMgr.resetProgram();
        SFX_LOG_INFO("LightProgram reset");
        return LightFxError::OK;
    });
}

// ============================================================================
//  LIGHT PROGRAM MANAGER WIRING
// ============================================================================

// Wires the manager's hardware-agnostic callbacks to local servos / landing
// lights. Called once in setup() before any config load. The progress
// callback for landing lights is registered here so deploy/retract progress
// flows back to the host even when bindings come from the YAML rather than
// an explicit LANDING_LIGHT_BIND packet.
void setupLightProgramManager() {
    LightProgramManager::Callbacks cb;

    cb.onServoConfig = [](uint8_t servoId, const LightProgramConfig::ServoSetup& s) {
        if (servoId < 1 || servoId > 3) return;
        ServoControl& sv = servos[servoId - 1];
        sv.setLimits(s.min_us, s.max_us);
        sv.setMaxSpeed(s.speed);
        sv.setAcceleration(s.accel);
        sv.setDeceleration(s.decel);
        sv.setReversed(s.reversed);
        sv.setTarget(s.default_us);
    };

    cb.onLandingBind = [](uint8_t groupIndex, const LightProgramConfig::LandingGroup& g) {
        const uint8_t slot = static_cast<uint8_t>(groupIndex + 1);
        if (slot < 1 || slot > LANDING_LIGHT_COUNT) return;

        LedControl* leds[LandingLight::MAX_LEDS];
        uint8_t ledCount = 0;
        for (uint8_t bit = 0; bit < 8 && ledCount < LandingLight::MAX_LEDS; ++bit) {
            if (g.lightfxChannelMask & (1u << bit)) leds[ledCount++] = &ledManager.channel(bit);
        }
        if (ledCount == 0) return;  // Group has no lightfx-side channels

        // Slave only owns the servo when the group says so.
        ServoControl* servo = nullptr;
        if (g.servoId != 0 && LightProgramBoard::isLightFx(g.servoBoard) &&
            g.servoId >= 1 && g.servoId <= 3) {
            servo = &servos[g.servoId - 1];
        }

        LandingLight& lg = landingLights[slot - 1];
        lg.setSlot(slot);
        lg.configure(servo, leds, ledCount, g.brightness);
        lg.onProgress([](const LightFxLandingLightStatus& status) {
            lightfxServer.sendLandingLightStatus(status);
        });
    };

    cb.onLandingUnbind  = [](uint8_t slot) {
        if (slot >= 1 && slot <= LANDING_LIGHT_COUNT) landingLights[slot - 1].unconfigure();
    };
    cb.onLandingDeploy  = [](uint8_t slot) {
        if (slot >= 1 && slot <= LANDING_LIGHT_COUNT &&
            landingLights[slot - 1].isConfigured()) {
            landingLights[slot - 1].deploy();
        }
    };
    cb.onLandingRetract = [](uint8_t slot) {
        if (slot >= 1 && slot <= LANDING_LIGHT_COUNT &&
            landingLights[slot - 1].isConfigured()) {
            landingLights[slot - 1].retract();
        }
    };

    progMgr.begin(&ledManager, cb, LightProgramBoard::LIGHTFX);
}

/** @brief Initialize flash and load config (if present). */
static void initFlashAndConfig() {
    FlashModule& flash = FlashModule::instance();
    if (flash.begin()) {
        FlashStorageInfo info;
        flash.getStorageInfo(info);
        SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                     (unsigned long)info.usedBytes, (unsigned long)info.totalBytes);

        // Wire config store to flash I/O
        wireConfigStore<FlashModule>(configServer.store());

        // Callback fires after protocol-triggered config reload
        configServer.onReloaded([](const LightFxConfig& cfg) {
            SFX_LOG_INFO("Config reloaded — applying to hardware");
            applyConfig(cfg);
        });

        // Try loading config (silent if file doesn't exist)
        if (configServer.loadConfig()) {
            applyConfig(configServer.store().data());
            server.markConfigLoaded();  // IDLE → STANDALONE
        }
        // If file doesn't exist, defaults are used — that's fine
    } else {
        SFX_LOG_WARN("Flash init failed — running with defaults");
    }
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    // Battery ADC
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE);

    // Low-voltage cutoff: when the pack drops below the chemistry's per-cell
    // low threshold (e.g. 3.2 V/cell on LiPo), soft-disable all 8 LED channels
    // to avoid drawing the pack any further. The flag persists until
    // LED_RESET / LED_ENABLE re-arms each channel.
    batteryMonitor.onLowVoltage([](uint16_t voltage_mV, uint8_t cellCount) {
        if (autoCutoffOnLowVoltage && server.indicators().isConnected()) {
            SFX_LOG_WARN("Battery LOW (%umV / %u cells) — disabling LED channels",
                         voltage_mV, cellCount);
            lowVoltageTriggered = true;
            disableAllLedChannels();
        }
    });

    server.begin("LightFX", FIRMWARE_VERSION, BUILD_NUMBER, PIN_LED_CONN, PIN_LED_ERR);
    server.onInit([](uint8_t mode, uint8_t flags) {
        (void)flags;  // LightFX accepts both SLAVE and DIRECT
        SFX_LOG_INFO("INIT mode=%s", InitMode::getName(mode));
        performSafeInit();
    });
    server.onShutdown([]() { performSafeShutdown(); });

    // Wire the light-program manager BEFORE the first config load so the
    // servo/landing-bind callbacks fire while applying the YAML. The hardware
    // (LED manager + servos) is initialized below — at config-apply time the
    // manager only stores values, no I/O happens until the first loop().
    setupLightProgramManager();

    // Initialize flash storage and load config (standalone mode)
    initFlashAndConfig();

    // Initialize LED channels via LedManager
    ledManager.begin(LED_CHANNEL_PINS, false, true);
    
    // Initialize servos.
    // ServoControl::begin signature is (pin, minUs, maxUs, initialUs) — the
    // three-argument form `begin(pin, 1500)` was a latent bug: 1500 landed in
    // the *minUs* slot, leaving servos clamped to [1500, 2500] at boot. Pass
    // all four explicitly so the defaults are obvious in the source.
    const uint8_t servo_pins[3] = {PIN_SERVO_1, PIN_SERVO_2, PIN_SERVO_3};
    for (int i = 0; i < 3; i++) {
        servos[i].begin(servo_pins[i],
                        ServoControlConfig::DEFAULT_MIN_US,
                        ServoControlConfig::DEFAULT_MAX_US,
                        SERVO_DEFAULT_US);
        servos[i].setMotionProfile(SERVO_DEFAULT_MAX_SPEED, SERVO_DEFAULT_ACCEL, SERVO_DEFAULT_DECEL);
    }
    
    // Initialize LightFxServer
    lightfxServer.begin(&Serial);
    lightfxServer.setLedManager(&ledManager);
    setupLightFxCallbacks();
    
    // STATUS: Append LightFX module data to core STATUS response.
    //
    // Wire format (64 bytes):
    //   byte 0..7   [ledBrightness:u8×8]
    //   byte 8      [ledSeqFlags:u8]
    //   byte 9..14  [servo0:u16][servo1:u16][servo2:u16]  — live position
    //   byte 15..17 [landingLightStates:u8×3]
    //   byte 18     [masterBrightness_pct:u8]
    //   byte 19     [ledEnabledFlags:u8]       (bit per channel, 1=enabled)
    //   byte 20..21 [batteryVoltage_mV:u16]
    //   byte 22     [batteryCells:u8]
    //   byte 23     [batteryPct:u8]
    //   byte 24     [batteryFlags:u8]          bit0=autoCutoff bit1=lowTriggered
    //   byte 25..63 [servoConfig:13×3]         min:u16 max:u16 target:u16
    //                                          speed:u16 accel:u16 decel:u16 rev:u8
    //
    // Rule 11: the servo config block was appended in 0.17.0; older decoders
    // stop at byte 25 and ignore the tail.
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 25) return 0;
        const bool haveRoomForCfg = (maxLen >= 64);

        // LED channel brightness (8 bytes)
        for (uint8_t i = 0; i < 8; i++) {
            buf[i] = ledManager.channel(i).brightness();
        }

        // LED sequence playing flags (1 byte, bit per channel)
        buf[8] = ledManager.seqPlayingFlags();

        // Servo positions (6 bytes). In STANDALONE mode SRV1 (GP8) is re-roled
        // as an RC PWM input, so report the last captured pulse instead of
        // the (detached) servo's cached commanded position.
        uint16_t srv0 = srv1Input.isEnabled() ? liveInput_us
                                              : (uint16_t)servos[0].position();
        CoreProtocol::putU16LE(&buf[9],  srv0);
        CoreProtocol::putU16LE(&buf[11], (uint16_t)servos[1].position());
        CoreProtocol::putU16LE(&buf[13], (uint16_t)servos[2].position());

        // Landing light states (3 bytes)
        for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
            buf[15 + i] = (uint8_t)landingLights[i].state();
        }

        // Master brightness percentage (1 byte)
        buf[18] = ledManager.masterBrightness();

        // LED enabled flags (1 byte, bit per channel, 1=enabled)
        buf[19] = ledManager.enabledFlags();

        // Battery (4 bytes)
        CoreProtocol::putU16LE(&buf[20], (uint16_t)batteryMonitor.voltage_mV());
        buf[22] = batteryMonitor.cellCount();
        buf[23] = batteryMonitor.percentage();

        // Battery flags (1 byte)
        uint8_t flags = 0;
        if (autoCutoffOnLowVoltage) flags |= 0x01;
        if (lowVoltageTriggered)    flags |= 0x02;
        buf[24] = flags;

        // Per-servo configuration block (Rule 11 extension, 13×3 = 39 bytes).
        // Only emitted when the core has room; legacy hubs / tight buffers drop
        // the tail and old decoders treat the payload as a 25-byte prefix.
        if (haveRoomForCfg) {
            size_t off = 25;
            for (uint8_t i = 0; i < 3; i++) {
                const auto& s = servos[i];
                CoreProtocol::putU16LE(&buf[off + 0],  (uint16_t)s.minLimit());
                CoreProtocol::putU16LE(&buf[off + 2],  (uint16_t)s.maxLimit());
                CoreProtocol::putU16LE(&buf[off + 4],  (uint16_t)s.target());
                CoreProtocol::putU16LE(&buf[off + 6],  (uint16_t)s.maxSpeed());
                CoreProtocol::putU16LE(&buf[off + 8],  (uint16_t)s.acceleration());
                CoreProtocol::putU16LE(&buf[off + 10], (uint16_t)s.deceleration());
                buf[off + 12] = s.isReversed() ? 1 : 0;
                off += 13;
            }
            return 64;
        }
        return 25;
    });
    
    // Set status broadcast source for verbose mode (STATUS_UPDATE packets)
    server.core().setStatusBroadcastSource(StatusUpdateSource::LIGHTFX);

    // Register config server (handles CONFIG_RELOAD/STATUS/SAVE)
    configServer.begin(&Serial);
    server.addModuleHandler(&configServer);

    // Generic battery handler (core BATTERY_CONFIG 0xEE) — register before
    // the module handler so battery commands resolve quickly.
    batteryServer.begin(&Serial);
    server.addModuleHandler(&batteryServer);

    // Finalize router (core handler + LightFX handler)
    server.addModuleHandler(&lightfxServer);

    // Register StorageServer (FILE_* protocol for /lightfx.yaml I/O).
    // FlashModule is already mounted by initFlashAndConfig().
    storageServer.begin(&Serial);
    storageServer.onTransferStart([]() { server.core().setTransferActive(true); });
    storageServer.onTransferEnd  ([]() { server.core().setTransferActive(false); });
    server.addModuleHandler(&storageServer);

    // Advertise interfaces — Pico LightFX has flash + config but no SD slot,
    // no audio/USB host/engine. Used by clients (CLI, Studio file manager)
    // to gate UI without speculative status probes.
    server.core().addCapability(CoreCapability::FLASH | CoreCapability::CONFIG);
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    // Update battery monitor
    batteryMonitor.update();

    // Process protocol, connection timeout, indicators
    server.loop();

    // Clean up stuck file uploads (aborted transfer → delete partial, free buffer)
    storageServer.checkUploadTimeout();

    // Sync SRV1 role (servo output vs RC PWM input) with current board state.
    // No-op when the state hasn't changed since last tick.
    updateSrv1Role();

    // Sample SRV1 whenever it's captured (any non-slave state) so the live
    // pulse shows up in STATUS and Studio can visualize it. Only STANDALONE
    // feeds the pulse into progMgr — in DIRECT the user is driving the board
    // manually and shouldn't get surprise program switches from RC wiggle.
    if (srv1Input.isEnabled()) {
        int us = srv1Input.update();
        if (us > 0) {
            liveInput_us = (uint16_t)us;
            if (server.core().boardState() == BoardState::STANDALONE) {
                progMgr.update(liveInput_us);
            }
        }
    }

    // Update LED sequences
    ledManager.update();

    // Update servos
    updateServos();

    // Update landing light sequencers
    updateLandingLights();

    busy_wait_ms(1);
}
