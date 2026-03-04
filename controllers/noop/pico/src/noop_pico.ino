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
 *   - PicoServer: Common server boilerplate (serial, indicators, core protocol)
 *   - CommandRouter: Routes packets (no module-specific handlers)
 */

#include <Arduino.h>
#include <pico_server.h>

// Firmware version
#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER 1

// ============================================================================
//  CONSTANTS
// ============================================================================

const uint8_t LED_PIN = 25;  // Onboard LED

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
PicoServer server;

// ============================================================================
//  STATE VARIABLES
// ============================================================================

uint32_t commandCount = 0;

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
    commandCount = 0;
    blinkLed(2);
}

void performSafeShutdown() {
    setLed(false);
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    server.begin("NoOp", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
    // Initialize onboard LED
    pinMode(LED_PIN, OUTPUT);
    setLed(false);
    
    // Finalize router (core-only, no module handler)
    server.addModuleHandler(nullptr);
    
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
