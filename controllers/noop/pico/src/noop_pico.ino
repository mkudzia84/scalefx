/**
 * NoOp Pico Controller v0.3.0
 * 
 * Minimal controller that implements the base protocol layer plus direct
 * servo control for hardware testing:
 * - INIT / INIT_READY handshake
 * - SHUTDOWN / REBOOT / BOOTSEL
 * - KEEPALIVE / STATUS_REQ
 * - SERVO_SET (0x64) - direct servo positioning (same as GearControl)
 * - I2C_SCAN - I2C bus diagnostics (via PicoServer)
 * 
 * This is useful for:
 * - Testing serial protocol without full controller firmware
 * - Testing servos on GearControl hardware
 * - Protocol development and debugging
 * - Template for new controllers
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * GPIO Pin Mapping (matches GearControl):
 *   GP1-3:   Servos 0-2 (door servos)
 *   GP4-5:   I2C SDA/SCL
 *   GP6-9:   Servos 3-6 (door/yaw)
 *   GP13-14: Indicator LEDs (bi-color RED/GREEN)
 * 
 * Servo Mapping:
 *   0: GP1   1: GP2   2: GP3   3: GP6
 *   4: GP7   5: GP8   6: GP9
 *
 * Architecture:
 *   - PicoServer: Common server boilerplate (serial, indicators, core protocol)
 *   - NoOpServoHandler: Inline ICommandHandler for SERVO_SET
 *   - CommandRouter: Routes packets (core + servo handler)
 */

#include <Arduino.h>
#include <Wire.h>
#include <pico_server.h>
#include <srv_control.h>
#include <serial.h>

// Firmware version
#define FIRMWARE_VERSION "0.3.0"
#define BUILD_NUMBER 3

// ============================================================================
//  PIN CONFIGURATION (matches GearControl)
// ============================================================================

const uint8_t NUM_SERVOS = 7;
const uint8_t SERVO_PINS[NUM_SERVOS] = { 1, 2, 3, 6, 7, 8, 9 };

// I2C pins
const uint8_t PIN_SDA = 4;
const uint8_t PIN_SCL = 5;

// Onboard LED
const uint8_t LED_PIN = 25;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
PicoServer server;

// Servos (direct control, no motion profiling for simplicity)
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
        size_t encodedLen = CoreProtocol::encodePacket(buffer, CorePacket::ACK, nullptr, 0);
        if (encodedLen > 0 && _serial) {
            _serial->write(buffer, encodedLen);
            _serial->write(CoreProtocol::FRAME_DELIMITER);
        }
    }

    void sendNack(uint8_t errorCode) {
        uint8_t payload[2] = { errorCode, 0 };
        uint8_t buffer[CoreProtocol::COBS_BUFFER_SIZE];
        size_t encodedLen = CoreProtocol::encodePacket(buffer, CorePacket::NACK, payload, 1);
        if (encodedLen > 0 && _serial) {
            _serial->write(buffer, encodedLen);
            _serial->write(CoreProtocol::FRAME_DELIMITER);
        }
    }

    Stream* _serial = nullptr;
    const char* _deviceName = nullptr;
};

// ============================================================================
//  LED CONTROL
// ============================================================================

void setLed(bool on) {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void blinkLed(int times, int delayMs = 100) {
    for (int i = 0; i < times; i++) {
        setLed(true);
        delay(delayMs);
        setLed(false);
        if (i < times - 1) delay(delayMs);
    }
}

// ============================================================================
//  CALLBACKS
// ============================================================================

void performSafeInit() {
    // Center all servos
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].setPositionImmediate(1500);
    }
    blinkLed(2);
}

void performSafeShutdown() {
    // Center all servos
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].setPositionImmediate(1500);
    }
    setLed(false);
}

// ============================================================================
//  SETUP
// ============================================================================

// Servo command handler instance
NoOpServoHandler servoHandler;

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    server.begin("NoOp", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
    // Initialize onboard LED
    pinMode(LED_PIN, OUTPUT);
    setLed(false);

    // Initialize I2C (for I2C scan diagnostics)
    Wire.setSDA(PIN_SDA);
    Wire.setSCL(PIN_SCL);
    Wire.begin();
    Wire.setClock(400000);
    server.enableI2CScan(Wire);

    // Initialize servos on GearControl pins
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
        servos[i].begin(SERVO_PINS[i]);
        servos[i].setPositionImmediate(1500);  // Center
    }

    // Initialize servo command handler
    servoHandler.begin(&Serial, server.deviceName());

    // STATUS: Report servo positions
    // Wire format (14 bytes): [servo0_us:u16LE] ... [servo6_us:u16LE]
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < NUM_SERVOS * 2) return 0;
        for (uint8_t i = 0; i < NUM_SERVOS; i++) {
            CoreProtocol::putU16LE(&buf[i * 2], (uint16_t)servos[i].position());
        }
        return NUM_SERVOS * 2;
    });
    
    // Finalize command router (core + servo handler)
    server.addModuleHandler(&servoHandler);
    
    // Ready indication
    blinkLed(3, 50);
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process protocol, connection timeout, indicators
    server.loop();
    
    // LED heartbeat when initialized
    static uint32_t lastBlink = 0;
    if (server.core().isInitialized() && millis() - lastBlink > 2000) {
        setLed(true);
        delay(10);
        setLed(false);
        lastBlink = millis();
    }
    
    delay(1);
}
