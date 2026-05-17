/*
 * GunFX Server — Binary Protocol Command Handler
 *
 * Server-side GunFX serial communication (binary COBS protocol).
 * Used by GunFX Pico to receive commands from HubFX client.
 * Extends BusServer for use with SfxServer + CommandRouter.
 *
 * Included from sfx_boards library. Protocol types in <serial/gunfx/gunfx.h>.
 */

#ifndef BOARDS_GUNFX_SERVER_H
#define BOARDS_GUNFX_SERVER_H

#include <serial/gunfx/gunfx.h>
#include <serial/core/bus_server.h>

// ============================================================================
// GunFxServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by GunFX Pico to receive commands from HubFX client.
 * Extends BusServer for use with SfxServer + CommandRouter.
 */
class GunFxServer : public BusServer {
public:
    const char* handlerName() const override { return "GunFxServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendStatus(const GunFxStatus& status);

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onTriggerOn(GunFxTriggerOnCallback cb) { _triggerOnCallback = cb; }
    void onTriggerOff(GunFxTriggerOffCallback cb) { _triggerOffCallback = cb; }
    void onServoSet(GunFxServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(GunFxServoSettingsCallback cb) { _servoSettingsCallback = cb; }
    void onRecoilJerk(GunFxRecoilJerkCallback cb) { _recoilJerkCallback = cb; }
    void onSmokeHeat(GunFxSmokeHeatCallback cb) { _smokeHeatCallback = cb; }
    void onSmokeSettings(GunFxSmokeSettingsCallback cb) { _smokeSettingsCallback = cb; }
    void onSmokeReset(GunFxSmokeResetCallback cb) { _smokeResetCallback = cb; }
    void onSmokeCurrentLimit(GunFxSmokeCurrentLimitCallback cb) { _smokeCurrentLimitCallback = cb; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0x01; }
    uint8_t moduleRangeHigh() const override { return 0x2F; }
    const char* getModuleErrorMessage(uint8_t code) override { return GunFxError::getMessage(code); }

private:
    GunFxTriggerOnCallback _triggerOnCallback;
    GunFxTriggerOffCallback _triggerOffCallback;
    GunFxServoSetCallback _servoSetCallback;
    GunFxServoSettingsCallback _servoSettingsCallback;
    GunFxRecoilJerkCallback _recoilJerkCallback;
    GunFxSmokeHeatCallback _smokeHeatCallback;
    GunFxSmokeSettingsCallback _smokeSettingsCallback;
    GunFxSmokeResetCallback _smokeResetCallback;
    GunFxSmokeCurrentLimitCallback _smokeCurrentLimitCallback;
};

#endif // BOARDS_GUNFX_SERVER_H
