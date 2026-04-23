/*
 * Light Program Manager — Implementation
 *
 * See light_program_manager.h for API documentation.
 */

#include "light_program_manager.h"
#include <cstring>
#include <serial/lightfx/lightfx.h>
#include "platform/sfx_platform.h"

// ============================================================================
// Lifecycle
// ============================================================================

void LightProgramManager::begin(ILedManager* leds, const Callbacks& callbacks, const char* board) {
    _leds = leds;
    _callbacks = callbacks;
    _isLightFx = LightProgramBoard::isLightFx(board);
    _activeProgram = -1;
    _configLoaded = false;
    _lastSwitchTime_ms = 0;
}

bool LightProgramManager::loadConfig(const LightProgramConfig& cfg) {
    if (!_leds) return false;

    _config = cfg;
    _configLoaded = true;
    _activeProgram = -1;

    // Apply master brightness
    _leds->setMasterBrightness(_config.masterBrightness);

    // Configure servos
    applyServoConfig();

    // Bind landing lights
    applyLandingBindings();

    return true;
}

// ============================================================================
// Program Control
// ============================================================================

void LightProgramManager::selectProgram(uint8_t index) {
    if (!_leds || !_configLoaded) return;
    if (index >= _config.programCount) return;

    const auto& prog = _config.programs[index];

    // 1. Stop all sequences and turn off all LEDs (clean slate)
    _leds->seqStop(0);
    _leds->ledOff(0);

    // 2. Load events for each channel defined in this program
    loadProgramEvents(prog);

    // 3. Apply group policies (deploy/retract landing groups per program)
    applyGroupPolicies(prog);

    _activeProgram = static_cast<int8_t>(index);
}

void LightProgramManager::resetProgram() {
    if (!_leds) return;

    _leds->seqStop(0);
    _leds->ledOff(0);

    // Retract every group that has at least one channel on this board's side.
    // Mirrors selectProgram()'s board-filtering so a reset on one board
    // doesn't disturb landing groups owned exclusively by the other board.
    if (_callbacks.onLandingRetract) {
        for (uint8_t i = 0; i < _config.landingGroupCount; i++) {
            if (boardChannelMask(_config.landingGroups[i]) == 0) continue;
            _callbacks.onLandingRetract(static_cast<uint8_t>(i + 1));
        }
    }

    _activeProgram = -1;
}

void LightProgramManager::update(uint16_t rxInput_us) {
    if (!_configLoaded || _config.input.bandCount == 0) return;

    // Find which program the input band maps to
    int8_t targetProgram = findProgramForBand(rxInput_us);
    if (targetProgram < 0 || targetProgram == _activeProgram) return;

    // Debounce: don't switch too rapidly
    uint32_t now = millis();
    if (now - _lastSwitchTime_ms < LightProgramManagerConfig::SWITCH_DEBOUNCE_MS) return;

    selectProgram(static_cast<uint8_t>(targetProgram));
    _lastSwitchTime_ms = now;
}

// ============================================================================
// Accessors
// ============================================================================

const char* LightProgramManager::programName(uint8_t index) const {
    if (index >= _config.programCount) return "";
    return _config.programs[index].name;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void LightProgramManager::applyServoConfig() {
    if (!_callbacks.onServoConfig) return;

    for (uint8_t i = 0; i < _config.servoCount; i++) {
        const auto& sv = _config.servos[i];
        if (sv.id == 0) continue;  // Skip unconfigured entries
        _callbacks.onServoConfig(sv.id, sv);
    }
}

void LightProgramManager::applyLandingBindings() {
    if (!_callbacks.onLandingBind) return;

    for (uint8_t i = 0; i < _config.landingGroupCount; i++) {
        const auto& lg = _config.landingGroups[i];
        // Only bind groups that touch this board's channel side.
        // A group with only hubfx channels is invisible to a lightfx slave,
        // and vice versa.
        if (boardChannelMask(lg) == 0) continue;
        _callbacks.onLandingBind(i, lg);
    }
}

void LightProgramManager::loadProgramEvents(
    const LightProgramConfig::Program& program
) {
    for (uint8_t i = 0; i < program.channelCount; i++) {
        const auto& chDef = program.channels[i];

        // Skip unconfigured channels and group-controlled channels
        if (chDef.channel == 0 || chDef.groupIndex != 0xFF) continue;

        // Skip channels owned by the other board — a hubfx-side def is
        // invisible to a lightfx slave's LED manager and vice versa.
        if (!channelOwnedByBoard(chDef)) continue;

        loadChannelEvents(chDef.channel, chDef);
    }
}

void LightProgramManager::loadChannelEvents(
    uint8_t ch,
    const LightProgramConfig::ChannelDef& chDef
) {
    // Clear existing sequence
    _leds->seqClear(ch);

    // Add each event
    for (uint8_t e = 0; e < chDef.eventCount; e++) {
        const auto& evt = chDef.events[e];
        uint8_t type = parseEventType(evt.type);
        if (type == 0xFF) continue;  // Skip unrecognized event types

        // Map human-readable config fields to seqAdd parameters (p1-p5)
        // based on event type.  See light_program_config.h header table.
        uint16_t p1 = 0, p2 = 0;
        uint8_t  p3 = 0, p4 = 0, p5 = 0;

        switch (type) {
            case LightFxEventType::ON:
                p1 = evt.duration_ms;
                p2 = evt.pwm_duty;
                p3 = evt.brightness;
                break;

            case LightFxEventType::OFF:
                p1 = evt.duration_ms;
                break;

            case LightFxEventType::FLASH:
                p1 = evt.cycle_ms;
                p2 = evt.duration_ms;
                p3 = evt.brightness;
                p4 = evt.duty;
                break;

            case LightFxEventType::FADE_IN:
                p1 = evt.duration_ms;
                p3 = evt.brightness;
                break;

            case LightFxEventType::FADE_OUT:
                p1 = evt.duration_ms;
                p3 = evt.brightness;
                break;

            case LightFxEventType::FADING:
                p1 = evt.cycle_ms;
                p2 = evt.duration_ms;
                p3 = evt.min_brightness;
                p4 = evt.max_brightness;
                break;

            case LightFxEventType::BEACON:
                p1 = evt.cycle_ms;
                p2 = evt.duration_ms;
                p3 = evt.flash_pct;
                p4 = evt.max_brightness;
                p5 = evt.min_brightness;
                break;

            default:
                continue;  // Unknown type — skip
        }

        _leds->seqAdd(ch, type, p1, p2, p3, p4, p5);
    }

    // Start the sequence if events were loaded
    if (chDef.eventCount > 0) {
        _leds->seqStart(ch);
    }
}

void LightProgramManager::applyGroupPolicies(const LightProgramConfig::Program& program) {
    for (uint8_t i = 0; i < _config.landingGroupCount; i++) {
        const auto& lg = _config.landingGroups[i];
        // Only act on groups that have at least one channel on this board
        if (boardChannelMask(lg) == 0) continue;

        uint8_t slot = i + 1;  // Slots are 1-based
        uint8_t policy = (i < program.groupPolicyCount)
                       ? program.groupPolicies[i]
                       : LightProgramConfig::GROUP_POLICY_OFF;

        switch (policy) {
            case LightProgramConfig::GROUP_POLICY_ON:
                if (_callbacks.onLandingDeploy) _callbacks.onLandingDeploy(slot);
                break;

            case LightProgramConfig::GROUP_POLICY_GEAR:
                // Gear-follow: deploy/retract handled by external gear signal,
                // no action on program switch (preserves current state)
                break;

            case LightProgramConfig::GROUP_POLICY_OFF:
            default:
                if (_callbacks.onLandingRetract) _callbacks.onLandingRetract(slot);
                break;
        }
    }
}

int8_t LightProgramManager::findProgramForBand(uint16_t us) const {
    for (uint8_t i = 0; i < _config.input.bandCount; i++) {
        const auto& band = _config.input.bands[i];
        if (us >= band.min_us && us <= band.max_us) {
            return static_cast<int8_t>(band.program);
        }
    }
    return -1;  // No matching band
}

uint8_t LightProgramManager::parseEventType(const char* typeName) {
    if (!typeName || typeName[0] == '\0') return 0xFF;

    if (strcmp(typeName, "on")       == 0) return LightFxEventType::ON;
    if (strcmp(typeName, "off")      == 0) return LightFxEventType::OFF;
    if (strcmp(typeName, "flash")    == 0) return LightFxEventType::FLASH;
    if (strcmp(typeName, "fade_in")  == 0) return LightFxEventType::FADE_IN;
    if (strcmp(typeName, "fade_out") == 0) return LightFxEventType::FADE_OUT;
    if (strcmp(typeName, "fading")   == 0) return LightFxEventType::FADING;
    if (strcmp(typeName, "beacon")   == 0) return LightFxEventType::BEACON;

    return 0xFF;  // Unrecognized
}
