/*
 * GearControl Client — Binary Protocol Client
 *
 * Client-side GearControl serial communication (binary COBS protocol).
 * Used by HubFX to send commands to GearControl server boards over USB.
 * Extends BusClient with GearControl-specific commands.
 *
 * Included from sfx_boards library. Protocol types in <serial/gearcontrol/gearcontrol.h>.
 */

#ifndef BOARDS_GEARCONTROL_CLIENT_H
#define BOARDS_GEARCONTROL_CLIENT_H

#include <serial/gearcontrol/gearcontrol.h>
#include <serial/client/bus_client.h>

// ============================================================================
// GearControlClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side GearControl serial communication (binary COBS protocol)
 *
 * Used by HubFX to send commands to GearControl server boards over USB.
 * Extends BusClient with GearControl-specific commands.
 */
class GearControlClient : public BusClient {
public:
    GearControlClient() = default;
    ~GearControlClient() override = default;

    GearControlClient(const GearControlClient&) = delete;
    GearControlClient& operator=(const GearControlClient&) = delete;

    // ========================================================================
    // Gear Control
    // ========================================================================

    CommandResult gearDeploy(uint8_t gearId);
    CommandResult gearRetract(uint8_t gearId);
    CommandResult gearStop(uint8_t gearId);
    CommandResult gearAll(uint8_t action);
    CommandResult gearCalibrate(uint8_t gearId, uint8_t timeout_s = 0);
    CommandResult gearCalibCancel(uint8_t gearId);
    CommandResult gearReset(uint8_t gearId);
    CommandResult gearEnable(uint8_t gearId, bool enabled);

    // ========================================================================
    // Servo Control
    // ========================================================================

    CommandResult setServoPosition(uint8_t servoId, uint16_t pulse_us);
    CommandResult setServoSettings(const GearControlServoConfig& config);

    // ========================================================================
    // Configuration
    // ========================================================================

    CommandResult setGearConfig(const GearControlGearConfig& config);
    CommandResult setYawConfig(const GearControlYawConfig& config);
    CommandResult setYawInput(uint16_t position_us);
    CommandResult setBatteryConfig(bool enabled, bool autoDeployOnLowVoltage = false);
    CommandResult setDoorMode(const GearControlDoorModeConfig& config);
    CommandResult requestI2CScan();

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onStatus(GearControlStatusCallback cb) { _statusCallback = cb; }
    void onCalibStatus(GearControlCalibStatusCallback cb) { _calibStatusCallback = cb; }
    void onGearSeqStatus(GearControlSeqStatusCallback cb) { _gearSeqStatusCallback = cb; }
    void onDoorStatus(GearControlDoorStatusCallback cb) { _doorStatusCallback = cb; }
    void onI2CScanResult(std::function<void(const I2CScanResult&)> cb) { _i2cScanResultCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    const GearControlStatus& lastStatus() const { return _lastStatus; }

    /**
     * @brief Board information returned during init (alias for BusClientBoardInfo)
     */
    using BoardInfo = BusClientBoardInfo;

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return GearControlError::getMessage(code); }

private:
    GearControlStatus _lastStatus;

    GearControlStatusCallback _statusCallback;
    GearControlCalibStatusCallback _calibStatusCallback;
    GearControlSeqStatusCallback _gearSeqStatusCallback;
    GearControlDoorStatusCallback _doorStatusCallback;
    std::function<void(const I2CScanResult&)> _i2cScanResultCallback;
};

#endif // BOARDS_GEARCONTROL_CLIENT_H
