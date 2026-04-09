/*
 * GearControl Server — Binary Protocol Command Handler
 *
 * Server-side GearControl serial communication (binary COBS protocol).
 * Used by GearControl Pico to receive commands from HubFX client.
 * Extends BusServer for use with SfxServer + CommandRouter.
 *
 * Included from sfx_boards library. Protocol types in <serial/gearcontrol/gearcontrol.h>.
 */

#ifndef BOARDS_GEARCONTROL_SERVER_H
#define BOARDS_GEARCONTROL_SERVER_H

#include <serial/gearcontrol/gearcontrol.h>
#include <serial/core/bus_server.h>

// ============================================================================
// GearControlServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side GearControl serial communication (binary COBS protocol)
 *
 * Used by GearControl Pico to receive commands from HubFX client.
 * Extends BusServer with GearControl-specific command dispatch.
 */
class GearControlServer : public BusServer {
public:
    GearControlServer() = default;
    ~GearControlServer() override = default;

    GearControlServer(const GearControlServer&) = delete;
    GearControlServer& operator=(const GearControlServer&) = delete;

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================

    const char* handlerName() const override { return "GearControlServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================

    int sendCalibStatus(const GearControlCalibStatus& status);
    int sendGearSeqStatus(const GearControlSeqStatus& status);
    int sendDoorStatus(const GearControlDoorStatus& status);

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onGearDeploy(GearControlGearDeployCallback cb) { _gearDeployCallback = cb; }
    void onGearRetract(GearControlGearRetractCallback cb) { _gearRetractCallback = cb; }
    void onGearStop(GearControlGearStopCallback cb) { _gearStopCallback = cb; }
    void onGearAll(GearControlGearAllCallback cb) { _gearAllCallback = cb; }
    void onServoSet(GearControlServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(GearControlServoSettingsCallback cb) { _servoSettingsCallback = cb; }
    void onGearConfig(GearControlGearConfigCallback cb) { _gearConfigCallback = cb; }
    void onYawConfig(GearControlYawConfigCallback cb) { _yawConfigCallback = cb; }
    void onYawInput(GearControlYawInputCallback cb) { _yawInputCallback = cb; }
    void onGearCalibrate(GearControlGearCalibrateCallback cb) { _gearCalibrateCallback = cb; }
    void onGearCalibCancel(GearControlCalibCancelCallback cb) { _gearCalibCancelCallback = cb; }
    void onBatteryConfig(GearControlBatteryConfigCallback cb) { _batteryConfigCallback = cb; }
    void onDoorMode(GearControlDoorModeCallback cb) { _doorModeCallback = cb; }
    void onGearReset(GearControlGearResetCallback cb) { _gearResetCallback = cb; }
    void onGearEnable(GearControlGearEnableCallback cb) { _gearEnableCallback = cb; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0x60; }
    uint8_t moduleRangeHigh() const override { return 0x7F; }
    const char* getModuleErrorMessage(uint8_t code) override { return GearControlError::getMessage(code); }

private:
    uint8_t _calibTag = 0;   // Tag from GEAR_CALIBRATE request, echoed in CALIB_STATUS responses
    uint8_t _gearTag[3] = {};  // Per-gear tags from GEAR_DEPLOY/RETRACT, echoed in GEAR_SEQ_STATUS

    GearControlGearDeployCallback _gearDeployCallback;
    GearControlGearRetractCallback _gearRetractCallback;
    GearControlGearStopCallback _gearStopCallback;
    GearControlGearAllCallback _gearAllCallback;
    GearControlServoSetCallback _servoSetCallback;
    GearControlServoSettingsCallback _servoSettingsCallback;
    GearControlGearConfigCallback _gearConfigCallback;
    GearControlYawConfigCallback _yawConfigCallback;
    GearControlYawInputCallback _yawInputCallback;
    GearControlGearCalibrateCallback _gearCalibrateCallback;
    GearControlCalibCancelCallback _gearCalibCancelCallback;
    GearControlBatteryConfigCallback _batteryConfigCallback;
    GearControlDoorModeCallback _doorModeCallback;
    GearControlGearResetCallback _gearResetCallback;
    GearControlGearEnableCallback _gearEnableCallback;
};

#endif // BOARDS_GEARCONTROL_SERVER_H
