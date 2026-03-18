/*
 * GunFX Client — Protocol Implementation
 *
 * GunFxClient command methods and STATUS response parsing.
 */

#include "gunfx_client.h"

using namespace CoreProtocol;

// ============================================================================
// GunFxClient - Module Packet Handler
// ============================================================================

void GunFxClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::STATUS:
            // New format: [counter:u32][uptime:u32][freeRam:u32][moduleData:20]
            // Module data: [flags:u8][fanSpeed:u8][fanOffMs:u16][servo0-2:u16x3]
            //              [rpm:u16][shots:u32][heaterMs:u32]
            if (len >= 32) {
                // Skip core header (12 bytes), parse module data at offset 12
                const uint8_t* mod = &payload[12];
                uint8_t flags = mod[0];
                _lastStatus.firing = (flags & 0x01) != 0;
                _lastStatus.flashActive = (flags & 0x02) != 0;
                _lastStatus.flashFading = (flags & 0x04) != 0;
                _lastStatus.heaterOn = (flags & 0x08) != 0;
                _lastStatus.fanOn = (flags & 0x10) != 0;
                _lastStatus.fanSpindown = (flags & 0x20) != 0;
                
                _lastStatus.fanSpeed = mod[1];
                _lastStatus.fanOffRemainingMs = getU16LE(&mod[2]);
                _lastStatus.servoUs[0] = getU16LE(&mod[4]);
                _lastStatus.servoUs[1] = getU16LE(&mod[6]);
                _lastStatus.servoUs[2] = getU16LE(&mod[8]);
                _lastStatus.rateOfFireRpm = getU16LE(&mod[10]);
                _lastStatus.shotsFired = getU32LE(&mod[12]);
                _lastStatus.heaterOnTimeMs = getU32LE(&mod[16]);
                
                // Core header fields
                _lastStatus.uptimeMs = getU32LE(&payload[4]);
                _lastStatus.freeRam = getU32LE(&payload[8]);
            } else if (len >= 12) {
                // Core-only status (no module data)
                _lastStatus.uptimeMs = getU32LE(&payload[4]);
                _lastStatus.freeRam = getU32LE(&payload[8]);
            }
            
            if (tag != TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_statusCallback) _statusCallback(_lastStatus);
            break;

        default:
            break;
    }
}

// ============================================================================
// GunFxClient - Trigger Control
// ============================================================================

CommandResult GunFxClient::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    putU16LE(payload, rpm);
    return sendCommand(GunFxPacket::TRIGGER_ON, payload, sizeof(payload));
}

CommandResult GunFxClient::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    putU16LE(payload, fanDelayMs);
    return sendCommand(GunFxPacket::TRIGGER_OFF, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Servo Control
// ============================================================================

CommandResult GunFxClient::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    putU16LE(&payload[1], pulseUs);
    return sendCommand(GunFxPacket::SRV_SET, payload, sizeof(payload));
}

CommandResult GunFxClient::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);
    putU16LE(&payload[3], config.maxUs);
    putU16LE(&payload[5], config.maxSpeedUsPerSec);
    putU16LE(&payload[7], config.maxAccelUsPerSec2);
    putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendCommand(GunFxPacket::SRV_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxClient::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    putU16LE(&payload[1], jerkUs);
    putU16LE(&payload[3], varianceUs);
    return sendCommand(GunFxPacket::SRV_RECOIL_JERK, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Smoke Control
// ============================================================================

CommandResult GunFxClient::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendCommand(GunFxPacket::SMOKE_HEAT, payload, sizeof(payload));
}

CommandResult GunFxClient::setSmokeSettings(const GunFxSmokeConfig& config) {
    uint8_t payload[8];
    payload[0] = config.fanPulsing ? 1 : 0;
    payload[1] = config.fanSpeed;
    payload[2] = config.fanPulseHigh;
    payload[3] = config.fanPulseLow;
    putU16LE(&payload[4], config.fanPulseMs);
    putU16LE(&payload[6], config.fanSpindownMs);
    return sendCommand(GunFxPacket::SMOKE_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxClient::smokeReset() {
    return sendCommand(GunFxPacket::SMOKE_RESET, nullptr, 0);
}

CommandResult GunFxClient::smokeCurrentLimit(uint8_t channel, uint16_t limit_mA) {
    uint8_t payload[3];
    payload[0] = channel;
    putU16LE(&payload[1], limit_mA);
    return sendCommand(GunFxPacket::SMOKE_CURRENT_LIMIT, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Status
// ============================================================================

CommandResult GunFxClient::requestStatus() {
    return sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
}
