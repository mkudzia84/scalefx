/*
 * GunFX Client — Binary Protocol Client
 *
 * Client-side GunFX serial communication (binary COBS protocol).
 * Used by HubFX to send commands to GunFX server boards over USB.
 * Extends BusClient with GunFX-specific commands.
 *
 * Included from sfx_boards library. Protocol types in <serial/gunfx/gunfx.h>.
 */

#ifndef BOARDS_GUNFX_CLIENT_H
#define BOARDS_GUNFX_CLIENT_H

#include <serial/gunfx/gunfx.h>
#include <serial/client/bus_client.h>

// ============================================================================
// GunFxClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to GunFX server boards over USB.
 * Extends BusClient with GunFX-specific commands.
 */
class GunFxClient : public BusClient {
public:
    // ========================================================================
    // Trigger Control
    // ========================================================================
    
    CommandResult triggerOn(uint16_t rpm);
    CommandResult triggerOff(uint16_t fanDelayMs = 0);

    // ========================================================================
    // Servo Control
    // ========================================================================
    
    CommandResult setServoPosition(uint8_t servoId, uint16_t pulseUs);
    CommandResult setServoConfig(const GunFxServoConfig& config);
    CommandResult setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs);

    // ========================================================================
    // Smoke Control
    // ========================================================================
    
    CommandResult setSmokeHeater(bool on);
    CommandResult setSmokeSettings(const GunFxSmokeConfig& config);
    CommandResult smokeReset();
    CommandResult smokeCurrentLimit(uint8_t channel, uint16_t limit_mA);

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onStatus(GunFxStatusCallback cb) { _statusCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    const GunFxStatus& lastStatus() const { return _lastStatus; }

    /**
     * @brief Board information returned during init (alias for BusClientBoardInfo)
     */
    using BoardInfo = BusClientBoardInfo;

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return GunFxError::getMessage(code); }

private:
    GunFxStatus _lastStatus;
    GunFxStatusCallback _statusCallback;
};

#endif // BOARDS_GUNFX_CLIENT_H
