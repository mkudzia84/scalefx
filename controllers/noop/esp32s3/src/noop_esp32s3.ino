/**
 * NoOp ESP32-S3 Controller v0.1.0
 * 
 * Minimal controller that implements the base protocol layer plus direct
 * servo control for hardware testing:
 * - INIT / INIT_READY handshake
 * - IDENTIFY (0xFE) — board type discovery without state change
 * - SHUTDOWN / REBOOT
 * - KEEPALIVE / STATUS_REQ
 * - SERVO_SET (0x64) — direct servo positioning (same as GearControl)
 * - I2C_SCAN — I2C bus diagnostics (via SfxServer)
 * - LOG_MESSAGE / DIAG_HISTORY — diagnostic log retrieval
 * 
 * This is useful for:
 * - Testing serial protocol on ESP32-S3 hardware
 * - Verifying COBS/CRC communication at 6Mbps via UART0
 * - Testing servos on ESP32-S3 boards
 * - Protocol development and debugging
 * - Template for new ESP32-S3 controllers
 * 
 * Hardware: ESP32-S3-DevKitC-1 (WROOM-1 N8R8)
 * Protocol: Binary COBS with CRC-8 over UART0 (USB-UART bridge) at 6Mbps
 * 
 * GPIO Pin Mapping:
 *   GP1-3:   Servos 0-2
 *   GP6-7:   Servos 3-4
 *   GP8:     I2C SDA
 *   GP9:     I2C SCL
 *   GP48:    Onboard RGB LED (connection indicator)
 *
 * Servo Mapping:
 *   0: GP1   1: GP2   2: GP3   3: GP6
 *   4: GP7
 *
 * Architecture:
 *   - SfxServer: Common server boilerplate (serial, indicators, core protocol)
 *   - NoOpServoHandler: Inline ICommandHandler for SERVO_SET
 *   - CommandRouter: Routes packets (core + servo handler)
 */

#include <Arduino.h>
#include <Wire.h>
#include <server/sfx_server.h>
#include <servo/srv_control.h>
#include <serial/serial.h>
#include <platform/diag_log.h>

// Firmware version
#define FIRMWARE_VERSION "0.1.0"
#define BUILD_NUMBER 4

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

const uint8_t NUM_SERVOS = 5;
const uint8_t SERVO_PINS[NUM_SERVOS] = { 1, 2, 3, 6, 7 };

// I2C pins
const uint8_t PIN_SDA = 8;
const uint8_t PIN_SCL = 9;

// Indicator LEDs
#define PIN_LED_CONNECTION  48   // Onboard RGB LED (connection status)
#define PIN_LED_ERROR       -1   // Disabled (no external error LED)

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

SfxServer server;
ServoControl servos[NUM_SERVOS];

// ============================================================================
//  SERVO COMMAND HANDLER
// ============================================================================

/**
 * Minimal ICommandHandler that handles SERVO_SET (0x64) packets.
 * Reuses GearControl's packet type so the same CLI commands work.
 * 
 * Payload: [servo_id:u8][pulse_us:u16LE]
 */
class NoOpServoHandler : public ICommandHandler {
public:
    void begin(Stream* stream, const char* deviceName) {
        _serial = stream;
        _deviceName = deviceName;
    }

    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override {
        if (type != GearControlPacket::SERVO_SET) {
            return CommandHandleResult::NotMyCommand;
        }

        if (len < 3) {
            sendNack(SerialError::MISSING_PARAMETER);
            return CommandHandleResult::Handled;
        }

        uint8_t servoId = payload[0];
        uint16_t pulse_us = CoreProtocol::getU16LE(&payload[1]);

        if (servoId >= NUM_SERVOS) {
            sendNack(SerialError::INVALID_ID);
            return CommandHandleResult::Handled;
        }
        if (pulse_us < 500 || pulse_us > 2500) {
            sendNack(SerialError::PARAM_OUT_OF_RANGE);
            return CommandHandleResult::Handled;
        }

        servos[servoId].setPositionImmediate(pulse_us);
        sendAck();
        return CommandHandleResult::Handled;
    }

    const char* handlerName() const override { return "NoOpServo"; }

private:
    void sendAck() {
        uint8_t buffer[CoreProtocol::COBS_BUFFER_SIZE];
        size_t encodedLen = CoreProtocol::encodePacket(buffer, CorePacket::ACK, _currentTag, nullptr, 0);
        if (encodedLen > 0 && _serial) {
            _serial->write(buffer, encodedLen);
            _serial->write(CoreProtocol::FRAME_DELIMITER);
        }
    }

    void sendNack(uint8_t errorCode) {
        uint8_t payload[2] = { errorCode, 0 };
        uint8_t buffer[CoreProtocol::COBS_BUFFER_SIZE];
        size_t encodedLen = CoreProtocol::encodePacket(buffer, CorePacket::NACK, _currentTag, payload, 1);
        if (encodedLen > 0 && _serial) {
            _serial->write(buffer, encodedLen);
            _serial->write(CoreProtocol::FRAME_DELIMITER);
        }
    }

    Stream* _serial = nullptr;
    const char* _deviceName = nullptr;
};

// ============================================================================
//  CALLBACKS
// ============================================================================

void performSafeInit() {
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].setPositionImmediate(1500);
    }
    SFX_LOG_INFO("NoOp init complete — %d servos centered", NUM_SERVOS);
}

void performSafeShutdown() {
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].setPositionImmediate(1500);
    }
    SFX_LOG_INFO("NoOp shutdown — servos centered");
}

// ============================================================================
//  SETUP
// ============================================================================

NoOpServoHandler servoHandler;

void setup() {
    // SfxServer handles serial init (UART0 @ 6Mbps), device naming,
    // indicator LEDs, CoreCommandServer, and DiagLog initialization
    server.begin("NoOp", FIRMWARE_VERSION, BUILD_NUMBER,
                 PIN_LED_CONNECTION, PIN_LED_ERROR);

    // Redirect ESP-IDF log output into DiagLog (prevents UART corruption)
    DiagLog::instance().captureEspLog();

    // Log reset reason for diagnostics
    {
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reasonStr = "UNKNOWN";
        switch (reason) {
            case ESP_RST_POWERON:   reasonStr = "POWER_ON";   break;
            case ESP_RST_EXT:       reasonStr = "EXTERNAL";   break;
            case ESP_RST_SW:        reasonStr = "SOFTWARE";   break;
            case ESP_RST_PANIC:     reasonStr = "PANIC";      break;
            case ESP_RST_INT_WDT:   reasonStr = "INT_WDT";    break;
            case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT";   break;
            case ESP_RST_WDT:       reasonStr = "WDT";        break;
            case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT";   break;
            default: break;
        }
        SFX_LOG_INFO("Boot reason: %s", reasonStr);
        if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PANIC ||
            reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT) {
            SFX_LOG_WARN("Abnormal reset detected: %s", reasonStr);
        }
    }

    // Initialize I2C (for I2C scan diagnostics)
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    server.enableI2CScan(Wire);

    // Initialize servos
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].begin(SERVO_PINS[i]);
        servos[i].setPositionImmediate(1500);  // Center
    }

    // Initialize servo command handler
    servoHandler.begin(&Serial, server.deviceName());

    // STATUS: Report servo positions
    // Wire format (10 bytes): [servo0_us:u16LE] ... [servo4_us:u16LE]
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < NUM_SERVOS * 2) return 0;
        for (uint8_t i = 0; i < NUM_SERVOS; i++) {
            CoreProtocol::putU16LE(&buf[i * 2], (uint16_t)servos[i].position());
        }
        return NUM_SERVOS * 2;
    });

    // Finalize command router (core + servo handler)
    server.addModuleHandler(&servoHandler);

    // Disable connection timeout — NoOp can operate standalone
    server.setConnectionTimeoutEnabled(false);

    SFX_LOG_INFO("NoOp ESP32-S3 v%s (build %d) — setup complete", FIRMWARE_VERSION, BUILD_NUMBER);
    SFX_LOG_INFO("Free heap: %u bytes, PSRAM: %u bytes",
                 ESP.getFreeHeap(), ESP.getFreePsram());
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process protocol, connection timeout, indicators
    server.loop();
    vTaskDelay(1);  // Yield to FreeRTOS scheduler
}
