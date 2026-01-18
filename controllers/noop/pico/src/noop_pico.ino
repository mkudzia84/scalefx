/**
 * NoOp Pico Controller v0.2.0
 * 
 * Minimal controller that only implements the base protocol layer:
 * - INIT / INIT_READY handshake
 * - SHUTDOWN
 * - REBOOT
 * - BOOTSEL
 * - KEEPALIVE
 * - STATUS_REQ
 * 
 * This is useful for:
 * - Testing serial protocol without hardware
 * - Template for new controllers
 * - Protocol development and debugging
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * Architecture:
 *   - CoreCommandHandler: Handles all system commands
 *   - CommandRouter: Routes packets (no module-specific handlers)
 */

#include <Arduino.h>
#include <pico/unique_id.h>
#include <serial.h>

// Firmware version
#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER 1

// ============================================================================
//  CONSTANTS
// ============================================================================

const uint32_t SERIAL_BAUD = 115200;
const uint8_t LED_PIN = 25;  // Onboard LED

// Connection timeout
const unsigned long CONNECTION_TIMEOUT_MS = 15000;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Serial protocol handlers
CommandRouter commandRouter;
CoreCommandHandler coreHandler;

// Device identification
char deviceName[24];

// ============================================================================
//  STATE VARIABLES
// ============================================================================

bool watchdog_triggered = false;
uint32_t commandCount = 0;

// ============================================================================
//  HELPER FUNCTIONS
// ============================================================================

void buildDeviceName() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(deviceName, sizeof(deviceName), "NoOp-%02X%02X", 
             id.id[6], id.id[7]);
}

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
//  CONNECTION MANAGEMENT
// ============================================================================

void checkConnectionStatus() {
    if (coreHandler.checkTimeout(CONNECTION_TIMEOUT_MS)) {
        if (!watchdog_triggered) {
            watchdog_triggered = true;
            setLed(false);
        }
    }
}

void performSafeInit() {
    watchdog_triggered = false;
    commandCount = 0;
}

void performSafeShutdown() {
    setLed(false);
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize USB serial
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < 3000) delay(10);
    
    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    setLed(false);
    
    // Build unique device name
    buildDeviceName();
    
    // ========================================================================
    // Initialize CoreCommandHandler (system commands)
    // ========================================================================
    coreHandler.begin(&Serial);
    coreHandler.setBoardInfo(deviceName, FIRMWARE_VERSION, "RP2040",
                              F_CPU / 1000000, rp2040.getFreeHeap(), BUILD_NUMBER);
    
    coreHandler.onInit([]() {
        performSafeInit();
        blinkLed(2);
    });
    
    coreHandler.onShutdown([]() {
        performSafeShutdown();
    });
    
    coreHandler.onReboot([]() {
        performSafeShutdown();
        delay(100);
        rp2040.reboot();
    });
    
    coreHandler.onBootsel([]() {
        performSafeShutdown();
        delay(100);
        rp2040.rebootToBootloader();
    });
    
    // ========================================================================
    // Initialize CommandRouter
    // ========================================================================
    commandRouter.begin(&Serial, [](uint8_t code, uint8_t type) {
        coreHandler.sendNack(code);
    });
    
    // No module-specific handlers for NoOp
    // CoreCommandHandler is called directly, not via router
    
    // Ready indication
    blinkLed(3, 50);
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process serial commands
    commandRouter.process();
    
    // Update activity timestamp
    if (commandRouter.lastActivityMs() > coreHandler.lastActivityMs()) {
        coreHandler.updateActivity();
    }
    
    // Check connection timeout
    checkConnectionStatus();
    
    // LED heartbeat when initialized
    static uint32_t lastBlink = 0;
    if (coreHandler.isInitialized() && millis() - lastBlink > 2000) {
        setLed(true);
        delay(10);
        setLed(false);
        lastBlink = millis();
    }
    
    delay(1);
}
