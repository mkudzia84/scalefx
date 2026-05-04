/**
 * PPM Test — Composite PPM Signal Decoder
 *
 * Reads a standard composite PPM signal from a single GPIO pin, decodes
 * individual RC channel pulse widths, and prints them to Serial in a
 * human-readable format.
 *
 * PPM frame format (standard RC):
 *   ┌─┐         ┌─┐         ┌─┐              ┌─┐
 *   │ │  ch1    │ │  ch2    │ │  ch3    ...  │ │  sync (>3ms)
 *   ┘ └─────────┘ └─────────┘ └─────────     ┘ └──────────────
 *     ← 1-2ms →  ← 1-2ms →
 *   ← ─────────────── 20ms frame ─────────────────────── →
 *
 * Each channel is encoded as the time between consecutive falling edges.
 * A sync gap (>3ms) marks the start of a new frame.
 *
 * Configuration (compile-time):
 *   PPM_PIN          — GPIO for PPM input (default: GPIO5 = IN_1 on HubFX)
 *   PPM_MAX_CHANNELS — Maximum channels to decode (default: 12)
 *   PPM_SYNC_US      — Minimum sync gap in µs (default: 3000)
 *   PPM_MIN_US       — Minimum valid pulse (default: 750)
 *   PPM_MAX_US       — Maximum valid pulse (default: 2250)
 *   PPM_INVERT       — Invert signal polarity (default: false)
 *
 * Output (Serial 115200):
 *   PPM [8ch] | 1500 1500 1000 1500 1500 1500 1878 1024 | fps=50 age=2ms
 *
 * Hardware: Any ESP32-S3 GPIO pin. Uses hardware ISR + esp_timer for µs timing.
 *
 * NO external libraries — only Arduino core + ESP-IDF headers.
 */

#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>

// ============================================================================
//  CONFIGURATION
// ============================================================================

/// GPIO pin for PPM input (default: GPIO5 = IN_1 header on HubFX board)
static constexpr int PPM_PIN = 5;

/// Maximum number of channels to decode
static constexpr int PPM_MAX_CHANNELS = 12;

/// Minimum sync gap duration (µs) — longer than this = new frame
static constexpr uint32_t PPM_SYNC_US = 3000;

/// Minimum valid channel pulse width (µs)
static constexpr uint32_t PPM_MIN_US = 750;

/// Maximum valid channel pulse width (µs)
static constexpr uint32_t PPM_MAX_US = 2250;

/// Invert signal polarity (true = active-low PPM, e.g. FrSky)
static constexpr bool PPM_INVERT = false;

/// Serial output interval (ms)
static constexpr uint32_t PRINT_INTERVAL_MS = 100;

/// LED pin for heartbeat
static constexpr int PIN_LED = 48;

// ============================================================================
//  PPM DECODER STATE
// ============================================================================

// ISR-updated state (written from ISR, read from loop)
static volatile uint32_t ppmChannels[PPM_MAX_CHANNELS];  // Current channel values (µs)
static volatile uint8_t  ppmChannelCount = 0;             // Channels in last frame
static volatile uint32_t ppmFrameCount = 0;               // Total frames decoded
static volatile uint32_t ppmLastFrameTime_us = 0;         // Timestamp of last complete frame
static volatile uint32_t ppmErrorCount = 0;                // Invalid pulse count

// ISR working state (only accessed from ISR)
static uint32_t isr_lastEdgeTime_us = 0;
static uint8_t  isr_currentChannel = 0;
static uint32_t isr_tempChannels[PPM_MAX_CHANNELS];

// ============================================================================
//  PPM ISR
// ============================================================================

/**
 * @brief ISR triggered on the active edge of the PPM signal.
 *
 * Measures the time between consecutive edges. A gap > PPM_SYNC_US
 * marks the end of a frame (and start of a new one). Shorter gaps
 * are channel pulse widths.
 */
static void IRAM_ATTR ppmISR() {
    uint32_t now_us = (uint32_t)esp_timer_get_time();  // µs timestamp
    uint32_t pulse_us = now_us - isr_lastEdgeTime_us;
    isr_lastEdgeTime_us = now_us;

    if (pulse_us >= PPM_SYNC_US) {
        // Sync gap detected — commit the frame
        if (isr_currentChannel > 0 && isr_currentChannel <= PPM_MAX_CHANNELS) {
            // Copy temp channels to output buffer
            for (uint8_t i = 0; i < isr_currentChannel; i++) {
                ppmChannels[i] = isr_tempChannels[i];
            }
            ppmChannelCount = isr_currentChannel;
            ppmFrameCount++;
            ppmLastFrameTime_us = now_us;
        }
        isr_currentChannel = 0;
    } else if (pulse_us >= PPM_MIN_US && pulse_us <= PPM_MAX_US) {
        // Valid channel pulse
        if (isr_currentChannel < PPM_MAX_CHANNELS) {
            isr_tempChannels[isr_currentChannel] = pulse_us;
            isr_currentChannel++;
        }
    } else {
        // Invalid pulse (noise, glitch)
        ppmErrorCount++;
    }
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);

    // Wait for serial
    uint32_t start = millis();
    while (!Serial && (millis() - start < 3000)) {
        delay(10);
    }

    Serial.println();
    Serial.println("================================================================");
    Serial.println("  PPM Test — Composite PPM Signal Decoder");
    Serial.println("  Standalone test firmware (no external libraries)");
    Serial.println("================================================================");
    Serial.println();

    // System info
    Serial.printf("  CPU:           %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("  Free heap:     %lu bytes\n", ESP.getFreeHeap());
    Serial.println();

    // Configuration
    Serial.println("--- PPM Configuration ---");
    Serial.printf("  Input pin:     GPIO%d\n", PPM_PIN);
    Serial.printf("  Max channels:  %d\n", PPM_MAX_CHANNELS);
    Serial.printf("  Sync gap:      %lu µs\n", PPM_SYNC_US);
    Serial.printf("  Valid range:   %lu–%lu µs\n", PPM_MIN_US, PPM_MAX_US);
    Serial.printf("  Polarity:      %s\n", PPM_INVERT ? "INVERTED" : "NORMAL");
    Serial.printf("  Output rate:   every %lu ms\n", PRINT_INTERVAL_MS);
    Serial.println();

    // LED heartbeat
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // Configure PPM input pin
    pinMode(PPM_PIN, INPUT_PULLUP);

    // Initialize channel buffer to center position
    for (int i = 0; i < PPM_MAX_CHANNELS; i++) {
        ppmChannels[i] = 1500;
        isr_tempChannels[i] = 1500;
    }

    // Attach interrupt
    // Standard PPM: pulse starts HIGH, channel width = time between falling edges
    // Inverted PPM: pulse starts LOW, channel width = time between rising edges
    int edge = PPM_INVERT ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(PPM_PIN), ppmISR, edge);

    Serial.printf("[PPM] ISR attached to GPIO%d (%s edge)\n",
                  PPM_PIN, PPM_INVERT ? "RISING" : "FALLING");
    Serial.println("[PPM] Waiting for PPM signal...");
    Serial.println();
}

// ============================================================================
//  LOOP — Serial output
// ============================================================================

static uint32_t lastPrintTime_ms = 0;
static uint32_t lastFrameSnapshot = 0;
static uint32_t lastPrintFrameCount = 0;
static bool     signalDetected = false;

void loop() {
    uint32_t now = millis();

    // LED heartbeat (fast blink when no signal, slow when receiving)
    static uint32_t ledToggle = 0;
    uint32_t ledRate = signalDetected ? 1000 : 200;
    if (now - ledToggle > ledRate) {
        ledToggle = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    if (now - lastPrintTime_ms < PRINT_INTERVAL_MS) {
        vTaskDelay(pdMS_TO_TICKS(1));
        return;
    }
    lastPrintTime_ms = now;

    // Snapshot ISR state (not perfectly atomic but good enough for display)
    uint8_t  channels  = ppmChannelCount;
    uint32_t frames    = ppmFrameCount;
    uint32_t errors    = ppmErrorCount;
    uint32_t lastFrame = ppmLastFrameTime_us;

    // Calculate frame rate (frames per second)
    uint32_t deltaFrames = frames - lastPrintFrameCount;
    float fps = (float)deltaFrames / ((float)PRINT_INTERVAL_MS / 1000.0f);
    lastPrintFrameCount = frames;

    // Signal age (time since last complete frame)
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t age_ms = (lastFrame > 0) ? (now_us - lastFrame) / 1000 : 9999;

    // Detect signal presence
    bool hadSignal = signalDetected;
    signalDetected = (age_ms < 200);  // Signal present if frame within last 200ms

    if (!signalDetected) {
        if (hadSignal) {
            Serial.println("[PPM] *** SIGNAL LOST ***");
        }
        // Print waiting message periodically
        static uint32_t lastWaitMsg = 0;
        if (now - lastWaitMsg > 2000) {
            lastWaitMsg = now;
            Serial.printf("[PPM] No signal on GPIO%d (errors: %lu)\n", PPM_PIN, errors);
        }
        return;
    }

    if (!hadSignal && signalDetected) {
        Serial.printf("[PPM] *** SIGNAL DETECTED — %d channels ***\n", channels);
    }

    // Build channel values string
    char valBuf[256];
    int pos = 0;
    for (uint8_t i = 0; i < channels && i < PPM_MAX_CHANNELS; i++) {
        uint32_t val = ppmChannels[i];
        if (i > 0) {
            pos += snprintf(valBuf + pos, sizeof(valBuf) - pos, " ");
        }
        pos += snprintf(valBuf + pos, sizeof(valBuf) - pos, "%4lu", val);
    }

    // Print formatted output
    // PPM [8ch] | 1500 1500 1000 1500 1500 1500 1878 1024 | fps=50 age=2ms err=0
    Serial.printf("PPM [%dch] | %s | fps=%.0f age=%lums err=%lu\n",
                  channels, valBuf, fps, age_ms, errors);
}
