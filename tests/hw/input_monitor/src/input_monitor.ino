/**
 * Input Monitor - HubFX IN_1 (GPIO5) bring-up rig.
 *
 * Experiment firmware to watch a raw inbound RC signal on the IN_1
 * header before trusting the production input path.  Phased:
 *
 *   MONITOR_MODE 0 (PWM)  - one standard RC PWM channel: a HIGH pulse
 *       1000-2000us wide, repeating ~50-333 Hz.  Times the pulse
 *       (rising->falling) in an ISR; prints width + rate + a bar.
 *       Goal: prove the signal is wired and readable at all.
 *   MONITOR_MODE 1 (PPM)  - composite sum-signal -> N channels.
 *       Auto-detects channel count (pulses between sync gaps) AND frame
 *       width (sync-to-sync period).  Prints every channel + frame stats.
 *   MONITOR_MODE 2 (SBUS) - later: 100000 8E2 inverted UART frame.
 *
 * Output is plain ASCII, CRLF-terminated (Serial @ 115200, UART0 via the
 * CH343 bridge) so PlatformIO's monitor / any terminal renders it clean.
 *
 * Wiring: signal -> GPIO5 (IN_1), ground -> board GND.  Common ground
 * with the receiver is required.  RC logic is 3.3-5 V; GPIO5 is 3.3 V -
 * keep a 5 V receiver output < 3.6 V if unsure (most modern Rx are 3.3 V).
 *
 * NO external libraries - Arduino core + esp_timer only.
 */

#include <Arduino.h>
#include <esp_timer.h>

// ── Common configuration ────────────────────────────────────────────
static constexpr int      IN_PIN      = 5;       // IN_1 header on HubFX
static constexpr int      PIN_LED     = 48;      // on-board status LED
static constexpr uint32_t PRINT_MS    = 100;     // serial cadence
static constexpr uint32_t SIGNAL_TIMEOUT_MS = 200;  // no frame/edge => lost

#ifndef MONITOR_MODE
#define MONITOR_MODE 0
#endif

#if MONITOR_MODE > 3
#  error "Modes: 0 = PWM, 1 = PPM, 2 = SBUS, 3 = JETI EX Bus."
#endif

// LED heartbeat: fast blink while searching, slow when locked.
static void heartbeat(bool signal) {
    static uint32_t ledToggle = 0;
    const uint32_t now = millis();
    if (now - ledToggle > (signal ? 800u : 150u)) {
        ledToggle = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }
}

// ════════════════════════════════════════════════════════════════════
#if MONITOR_MODE == 0   //  PWM - single-channel pulse-width monitor
// ════════════════════════════════════════════════════════════════════

static constexpr uint32_t PWM_MIN_US = 800;      // valid-pulse window
static constexpr uint32_t PWM_MAX_US = 2200;

// volatile is fine: ISR writes, loop() reads for display only, single-core.
static volatile uint32_t g_pulseUs    = 0;   // last HIGH pulse width (us)
static volatile uint32_t g_pulseCount = 0;   // completed pulses (for rate)
static volatile uint32_t g_lastEdgeUs = 0;   // timestamp of last falling edge

static uint32_t isr_riseUs = 0;              // ISR-local: current rising edge

// Both-edge ISR: rising stamps start, falling => pulse = now - start.
static void IRAM_ATTR pwmISR() {
    const uint32_t now = (uint32_t)esp_timer_get_time();
    if (digitalRead(IN_PIN)) {
        isr_riseUs = now;                    // rising edge
    } else if (isr_riseUs) {
        g_pulseUs    = now - isr_riseUs;     // falling edge -> pulse width
        g_pulseCount = g_pulseCount + 1;
        g_lastEdgeUs = now;
    }
}

static void modeSetup() {
    Serial.println("  MODE: PWM (single channel)");
    Serial.printf("  Valid pulse window : %lu-%lu us\r\n",
                  (unsigned long)PWM_MIN_US, (unsigned long)PWM_MAX_US);
    Serial.println("  Feed a single RC PWM channel (servo signal) to GPIO5 + GND.");
    Serial.println();

    pinMode(IN_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(IN_PIN), pwmISR, CHANGE);
    Serial.printf("[mon] ISR attached on GPIO%d (CHANGE) - waiting...\r\n\r\n", IN_PIN);
}

static void modeLoop() {
    const uint32_t now = millis();

    const uint32_t pulse    = g_pulseUs;
    const uint32_t count    = g_pulseCount;
    const uint32_t lastEdge = g_lastEdgeUs;

    const uint32_t nowUs  = (uint32_t)esp_timer_get_time();
    const uint32_t ageMs  = lastEdge ? (nowUs - lastEdge) / 1000u : 0xFFFFFFFFu;
    const bool     signal = (lastEdge != 0) && (ageMs < SIGNAL_TIMEOUT_MS);
    heartbeat(signal);

    static uint32_t lastPrintMs = 0;
    if (now - lastPrintMs < PRINT_MS) { vTaskDelay(pdMS_TO_TICKS(1)); return; }
    lastPrintMs = now;

    static uint32_t lastCount = 0;
    const uint32_t dCount = count - lastCount;
    lastCount = count;
    const float hz = (float)dCount * 1000.0f / (float)PRINT_MS;

    static bool hadSignal = false;
    if (!signal) {
        if (hadSignal) Serial.println("[mon] *** SIGNAL LOST ***");
        hadSignal = false;
        static uint32_t lastWait = 0;
        if (now - lastWait > 2000) {
            lastWait = now;
            Serial.printf("[mon] no signal on GPIO%d (last pulse %lu us)\r\n",
                          IN_PIN, (unsigned long)pulse);
        }
        return;
    }
    if (!hadSignal) Serial.println("[mon] *** SIGNAL DETECTED ***");
    hadSignal = true;

    const bool valid = (pulse >= PWM_MIN_US && pulse <= PWM_MAX_US);

    // Position bar: map 1000..2000 us across 20 cells (clamped).
    char bar[24];
    long span = (long)pulse - 1000;
    if (span < 0) span = 0;
    if (span > 1000) span = 1000;
    const int cell = (int)((span * 19) / 1000);   // 0..19
    for (int i = 0; i < 20; i++) bar[i] = (i == cell) ? '|' : ((i == 10) ? ':' : '.');
    bar[20] = '\0';

    Serial.printf("PWM GPIO%d | %4lu us  %s | %3.0f Hz | [%s] | age=%lums\r\n",
                  IN_PIN, (unsigned long)pulse, valid ? "OK" : "??",
                  hz, bar, (unsigned long)ageMs);
}

// ════════════════════════════════════════════════════════════════════
#elif MONITOR_MODE == 1   //  PPM - composite sum-signal decoder
// ════════════════════════════════════════════════════════════════════

// How many channels you EXPECT (flagged in the readout); the decoder
// still auto-detects the actual count regardless of this value.
#ifndef PPM_EXPECTED_CH
#define PPM_EXPECTED_CH 9
#endif

static constexpr int      PPM_MAX_CHANNELS = 24;     // decode capacity (project-wide PPM max)
static constexpr uint32_t PPM_SYNC_US      = 3000;   // gap > this => new frame
static constexpr uint32_t PPM_MIN_US       = 750;    // valid channel-slot range
static constexpr uint32_t PPM_MAX_US       = 2250;
static constexpr bool     PPM_INVERT       = false;  // true = active-low (FrSky)

// ISR-published frame state (written in ISR at frame commit, read in loop).
static volatile uint16_t g_chan[PPM_MAX_CHANNELS];   // last-frame channel us
static volatile uint8_t  g_chanCount   = 0;          // channels in last frame
static volatile uint32_t g_frameCount  = 0;          // total frames committed
static volatile uint32_t g_lastFrameUs = 0;          // commit timestamp
static volatile uint32_t g_framePerUs  = 0;          // sync-to-sync period (frame width)
static volatile uint32_t g_errCount    = 0;          // out-of-range pulses

// ISR working state.
static uint32_t isr_lastEdgeUs = 0;
static uint32_t isr_prevSyncUs = 0;
static uint8_t  isr_ch         = 0;
static uint16_t isr_tmp[PPM_MAX_CHANNELS];

// Active-edge ISR: each channel slot = time between consecutive edges; a
// gap > PPM_SYNC_US is the frame boundary that commits the accumulated set.
static void IRAM_ATTR ppmISR() {
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const uint32_t gap = now - isr_lastEdgeUs;
    isr_lastEdgeUs = now;

    if (gap >= PPM_SYNC_US) {                 // sync gap -> commit frame
        if (isr_ch > 0 && isr_ch <= PPM_MAX_CHANNELS) {
            for (uint8_t i = 0; i < isr_ch; i++) g_chan[i] = isr_tmp[i];
            g_chanCount  = isr_ch;
            g_frameCount = g_frameCount + 1;
            if (isr_prevSyncUs) g_framePerUs = now - isr_prevSyncUs;
            g_lastFrameUs = now;
        }
        isr_prevSyncUs = now;
        isr_ch = 0;
    } else if (gap >= PPM_MIN_US && gap <= PPM_MAX_US) {   // channel slot
        if (isr_ch < PPM_MAX_CHANNELS) isr_tmp[isr_ch++] = (uint16_t)gap;
    } else {                                  // glitch / noise
        g_errCount = g_errCount + 1;
    }
}

static void modeSetup() {
    Serial.println("  MODE: PPM (composite sum-signal, auto channel/frame detect)");
    Serial.printf("  Expected channels : %d (auto-detected at runtime)\r\n", PPM_EXPECTED_CH);
    Serial.printf("  Decode capacity   : %d channels\r\n", PPM_MAX_CHANNELS);
    Serial.printf("  Sync gap          : > %lu us = frame boundary\r\n", (unsigned long)PPM_SYNC_US);
    Serial.printf("  Valid slot range  : %lu-%lu us\r\n",
                  (unsigned long)PPM_MIN_US, (unsigned long)PPM_MAX_US);
    Serial.printf("  Polarity          : %s\r\n", PPM_INVERT ? "INVERTED (rising edge)" : "NORMAL (falling edge)");
    Serial.println("  Feed a composite PPM/CPPM stream to GPIO5 + GND.");
    Serial.println();

    for (int i = 0; i < PPM_MAX_CHANNELS; i++) { g_chan[i] = 1500; isr_tmp[i] = 1500; }

    pinMode(IN_PIN, INPUT_PULLUP);
    const int edge = PPM_INVERT ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(IN_PIN), ppmISR, edge);
    Serial.printf("[ppm] ISR attached on GPIO%d (%s) - waiting...\r\n\r\n",
                  IN_PIN, PPM_INVERT ? "RISING" : "FALLING");
}

static void modeLoop() {
    const uint32_t now = millis();

    const uint32_t lastFrame = g_lastFrameUs;
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();
    const uint32_t ageMs = lastFrame ? (nowUs - lastFrame) / 1000u : 0xFFFFFFFFu;
    const bool     signal = (lastFrame != 0) && (ageMs < SIGNAL_TIMEOUT_MS);
    heartbeat(signal);

    static uint32_t lastPrintMs = 0;
    if (now - lastPrintMs < PRINT_MS) { vTaskDelay(pdMS_TO_TICKS(1)); return; }
    lastPrintMs = now;

    const uint8_t  channels = g_chanCount;
    const uint32_t frames   = g_frameCount;
    const uint32_t errors   = g_errCount;
    const uint32_t framePer = g_framePerUs;

    static uint32_t lastFrames = 0;
    const uint32_t dFrames = frames - lastFrames;
    lastFrames = frames;
    const float fps = (float)dFrames * 1000.0f / (float)PRINT_MS;

    static bool hadSignal = false;
    if (!signal) {
        if (hadSignal) Serial.println("[ppm] *** SIGNAL LOST ***");
        hadSignal = false;
        static uint32_t lastWait = 0;
        if (now - lastWait > 2000) {
            lastWait = now;
            Serial.printf("[ppm] no signal on GPIO%d (errors %lu)\r\n",
                          IN_PIN, (unsigned long)errors);
        }
        return;
    }
    if (!hadSignal) {
        Serial.printf("[ppm] *** SIGNAL DETECTED *** auto-detected %d channels, "
                      "frame %.1f ms (%.0f Hz)%s\r\n",
                      channels, framePer / 1000.0f,
                      framePer ? 1000000.0f / framePer : 0.0f,
                      channels == PPM_EXPECTED_CH ? " [matches expected]" : " [!= expected]");
    }
    hadSignal = true;

    // Channel values: "1500 1499 1001 ...".
    char vals[160];
    int pos = 0;
    for (uint8_t i = 0; i < channels && i < PPM_MAX_CHANNELS; i++) {
        pos += snprintf(vals + pos, sizeof(vals) - pos, i ? " %4u" : "%4u", g_chan[i]);
        if (pos >= (int)sizeof(vals)) break;
    }

    // PPM [9ch] | 1500 1499 ... | frame=22.5ms 44Hz fps=44 age=2ms err=0
    Serial.printf("PPM [%dch%s] | %s | frame=%.1fms %.0fHz fps=%.0f age=%lums err=%lu\r\n",
                  channels, channels == PPM_EXPECTED_CH ? "" : "?", vals,
                  framePer / 1000.0f, framePer ? 1000000.0f / framePer : 0.0f,
                  fps, (unsigned long)ageMs, (unsigned long)errors);
}

// ════════════════════════════════════════════════════════════════════
#elif MONITOR_MODE == 2   //  SBUS - inverted UART frame decoder
// ════════════════════════════════════════════════════════════════════

// SBUS is a UART protocol (NOT edge-timed like PWM/PPM): 100000 baud,
// 8E2, signal INVERTED.  We claim a real hardware UART peripheral
// (Serial1 = UART1) with RX on IN_1/GPIO5 and let the silicon do the
// framing + inversion - exactly how the production InputPort SBUS mode
// works (Rule 31: an InputPort consumes a UART peripheral for SBUS).
//
// Frame: 25 bytes = [0x0F header][22 ch bytes: 16 ch x 11 bit][flags][footer].
// Channel raw range ~172..1811 maps to ~1000..2000 us.

static constexpr uint32_t SBUS_BAUD     = 100000;
static constexpr uint8_t  SBUS_HEADER   = 0x0F;
static constexpr int      SBUS_FRAME_SZ = 25;
static constexpr int      SBUS_CHANNELS = 16;   // 11-bit channels in the frame
// Flag-byte bits (frame[23]).
static constexpr uint8_t  SBUS_FLAG_CH17      = 0x01;
static constexpr uint8_t  SBUS_FLAG_CH18      = 0x02;
static constexpr uint8_t  SBUS_FLAG_FRAME_LOST = 0x04;
static constexpr uint8_t  SBUS_FLAG_FAILSAFE   = 0x08;

// Parser state (loop-only, no ISR - the UART driver buffers RX for us).
static uint8_t  sbus_buf[SBUS_FRAME_SZ];
static int      sbus_idx = 0;

// Published frame state.
static uint16_t g_chan[SBUS_CHANNELS];   // raw 11-bit channel values
static uint8_t  g_flags       = 0;
static uint32_t g_frameCount  = 0;
static uint32_t g_lastFrameMs = 0;
static uint32_t g_errCount    = 0;       // footer/sync mismatches

// SBUS raw (172..1811) -> microseconds (~1000..2000), the common linear map.
static inline int sbusToUs(uint16_t raw) { return (int)((raw * 5) / 8) + 880; }

// Unpack 16 x 11-bit little-endian channels from frame[1..22].
static void sbusUnpack(const uint8_t* f) {
    g_chan[0]  = (uint16_t)((f[1]       | f[2]  << 8) & 0x07FF);
    g_chan[1]  = (uint16_t)((f[2]  >> 3 | f[3]  << 5) & 0x07FF);
    g_chan[2]  = (uint16_t)((f[3]  >> 6 | f[4]  << 2 | f[5] << 10) & 0x07FF);
    g_chan[3]  = (uint16_t)((f[5]  >> 1 | f[6]  << 7) & 0x07FF);
    g_chan[4]  = (uint16_t)((f[6]  >> 4 | f[7]  << 4) & 0x07FF);
    g_chan[5]  = (uint16_t)((f[7]  >> 7 | f[8]  << 1 | f[9] << 9) & 0x07FF);
    g_chan[6]  = (uint16_t)((f[9]  >> 2 | f[10] << 6) & 0x07FF);
    g_chan[7]  = (uint16_t)((f[10] >> 5 | f[11] << 3) & 0x07FF);
    g_chan[8]  = (uint16_t)((f[12]      | f[13] << 8) & 0x07FF);
    g_chan[9]  = (uint16_t)((f[13] >> 3 | f[14] << 5) & 0x07FF);
    g_chan[10] = (uint16_t)((f[14] >> 6 | f[15] << 2 | f[16] << 10) & 0x07FF);
    g_chan[11] = (uint16_t)((f[16] >> 1 | f[17] << 7) & 0x07FF);
    g_chan[12] = (uint16_t)((f[17] >> 4 | f[18] << 4) & 0x07FF);
    g_chan[13] = (uint16_t)((f[18] >> 7 | f[19] << 1 | f[20] << 9) & 0x07FF);
    g_chan[14] = (uint16_t)((f[20] >> 2 | f[21] << 6) & 0x07FF);
    g_chan[15] = (uint16_t)((f[21] >> 5 | f[22] << 3) & 0x07FF);
}

static void modeSetup() {
    Serial.println("  MODE: SBUS (inverted UART, hardware peripheral)");
    Serial.printf("  UART : Serial1 @ %lu baud 8E2 inverted, RX = GPIO%d\r\n",
                  (unsigned long)SBUS_BAUD, IN_PIN);
    Serial.printf("  Frame: %d bytes, %d channels (11-bit), header 0x%02X\r\n",
                  SBUS_FRAME_SZ, SBUS_CHANNELS, SBUS_HEADER);
    Serial.println("  Feed an SBUS stream to GPIO5 + GND (no external inverter needed).");
    Serial.println();

    for (int i = 0; i < SBUS_CHANNELS; i++) g_chan[i] = 992;  // center raw

    // Claim UART1: RX on IN_PIN, no TX, invert = true (SBUS is inverted).
    Serial1.begin(SBUS_BAUD, SERIAL_8E2, IN_PIN, -1, true);
    Serial.printf("[sbus] UART1 RX bound to GPIO%d (inverted) - waiting...\r\n\r\n", IN_PIN);
}

static void modeLoop() {
    const uint32_t now = millis();

    // Drain the UART RX buffer; resync on header + footer signature.
    while (Serial1.available()) {
        const uint8_t b = (uint8_t)Serial1.read();
        if (sbus_idx == 0) {
            if (b != SBUS_HEADER) continue;     // hunt for frame start
            sbus_buf[sbus_idx++] = b;
        } else {
            sbus_buf[sbus_idx++] = b;
            if (sbus_idx >= SBUS_FRAME_SZ) {
                sbus_idx = 0;
                // Footer low nibble must be 0 (0x00 / 0x04 / 0x14 / 0x24 / 0x34).
                if (sbus_buf[0] == SBUS_HEADER && (sbus_buf[24] & 0x0F) == 0x00) {
                    sbusUnpack(sbus_buf);
                    g_flags       = sbus_buf[23];
                    g_frameCount += 1;
                    g_lastFrameMs = now;
                } else {
                    g_errCount += 1;            // bad framing -> resync
                }
            }
        }
    }

    const uint32_t ageMs  = g_lastFrameMs ? (now - g_lastFrameMs) : 0xFFFFFFFFu;
    const bool     signal = (g_lastFrameMs != 0) && (ageMs < SIGNAL_TIMEOUT_MS);
    heartbeat(signal);

    static uint32_t lastPrintMs = 0;
    if (now - lastPrintMs < PRINT_MS) { vTaskDelay(pdMS_TO_TICKS(1)); return; }
    lastPrintMs = now;

    static uint32_t lastFrames = 0;
    const uint32_t dFrames = g_frameCount - lastFrames;
    lastFrames = g_frameCount;
    const float fps = (float)dFrames * 1000.0f / (float)PRINT_MS;

    static bool hadSignal = false;
    if (!signal) {
        if (hadSignal) Serial.println("[sbus] *** SIGNAL LOST ***");
        hadSignal = false;
        static uint32_t lastWait = 0;
        if (now - lastWait > 2000) {
            lastWait = now;
            Serial.printf("[sbus] no frames on GPIO%d (errors %lu)\r\n",
                          IN_PIN, (unsigned long)g_errCount);
        }
        return;
    }
    if (!hadSignal) Serial.printf("[sbus] *** SIGNAL DETECTED *** %d channels\r\n", SBUS_CHANNELS);
    hadSignal = true;

    // All channels on one line, in microseconds.
    char vals[160];
    int pos = 0;
    for (int i = 0; i < SBUS_CHANNELS; i++) {
        pos += snprintf(vals + pos, sizeof(vals) - pos, i ? " %4d" : "%4d", sbusToUs(g_chan[i]));
        if (pos >= (int)sizeof(vals)) break;
    }

    // Flag string: FL = frame-lost, FS = failsafe, d17/d18 = digital ch17/18.
    char flagstr[24];
    snprintf(flagstr, sizeof(flagstr), "%s%s%s%s",
             (g_flags & SBUS_FLAG_FRAME_LOST) ? "FL " : "",
             (g_flags & SBUS_FLAG_FAILSAFE)   ? "FS " : "",
             (g_flags & SBUS_FLAG_CH17)       ? "d17 " : "",
             (g_flags & SBUS_FLAG_CH18)       ? "d18 " : "");
    if (flagstr[0] == '\0') { flagstr[0] = 'O'; flagstr[1] = 'K'; flagstr[2] = '\0'; }

    // SBUS [16ch] | 1500 1499 ... | flags=OK fps=140 age=2ms err=0
    Serial.printf("SBUS [%dch] | %s | flags=%s fps=%.0f age=%lums err=%lu\r\n",
                  SBUS_CHANNELS, vals, flagstr, fps,
                  (unsigned long)ageMs, (unsigned long)g_errCount);
}

// ════════════════════════════════════════════════════════════════════
#elif MONITOR_MODE == 3   //  JETI EX Bus - half-duplex UART, listen-only
// ════════════════════════════════════════════════════════════════════

// Jeti EX Bus is a UART protocol (like SBUS, but NOT inverted): 125000 or
// 250000 baud, 8N1, half-duplex on a single TTL wire.  This mode is
// LISTEN-ONLY - the receiver (master) continuously transmits channel-data
// packets whether or not a device responds, so we can decode servo
// channels without driving the bus.  (Talking BACK to the radio -
// telemetry / device discovery - needs the half-duplex responder; see
// README "What's needed for radio discovery".)
//
// EX Bus packet:
//   [0] 0x3E master / 0x3D slave   [1] 0x01 resp-allowed / 0x03 data-only
//   [2] length (whole packet)      [3] packet id (counter)
//   [4] data id: 0x31 channels, 0x3A EX telemetry, 0x3B JetiBox
//   [5] data-block length          [6..] data    [n-2,n-1] CRC16 (LE)
// Channel block (id 0x31): N x 16-bit LE, units of 1/8 us -> us = raw/8.

#ifndef JETI_BAUD
#define JETI_BAUD 125000          // try 250000 if no lock
#endif

static constexpr uint8_t  JETI_HDR_MASTER = 0x3E;
static constexpr uint8_t  JETI_HDR_SLAVE  = 0x3D;
static constexpr uint8_t  JETI_ID_CHANNELS = 0x31;
static constexpr int      JETI_MAX_PKT     = 64;   // channel pkt ~40 B
static constexpr int      JETI_MAX_CH      = 24;

// Jeti EX Bus CRC16 (CCITT reflected, poly 0x8408, init 0).
static uint16_t jetiCrc16(const uint8_t* p, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

// Parser state (loop-only; UART driver buffers RX).
static uint8_t  jeti_buf[JETI_MAX_PKT];
static int      jeti_idx = 0;
static int      jeti_need = 0;     // expected total length once header seen

// Published frame state.
static uint16_t g_chan[JETI_MAX_CH];
static uint8_t  g_chanCount   = 0;
static uint32_t g_frameCount  = 0;
static uint32_t g_lastFrameMs = 0;
static uint32_t g_crcErr      = 0;
static uint32_t g_rawBytes    = 0;   // total bytes seen on the UART (diag)

static void jetiHandlePacket(const uint8_t* p, int len, uint32_t nowMs) {
    const uint16_t want = (uint16_t)(p[len - 2] | (p[len - 1] << 8));
    if (jetiCrc16(p, len - 2) != want) { g_crcErr++; return; }
    if (p[4] != JETI_ID_CHANNELS) return;             // ignore telemetry/JetiBox
    const int sub = p[5];
    int n = sub / 2;
    if (n > JETI_MAX_CH) n = JETI_MAX_CH;
    for (int i = 0; i < n; i++) {
        const uint16_t raw = (uint16_t)(p[6 + i * 2] | (p[7 + i * 2] << 8));
        g_chan[i] = (uint16_t)(raw / 8);              // 1/8 us -> us
    }
    g_chanCount   = (uint8_t)n;
    g_frameCount += 1;
    g_lastFrameMs = nowMs;
}

static void modeSetup() {
    Serial.println("  MODE: JETI EX Bus (half-duplex UART, listen-only)");
    Serial.printf("  UART : Serial1 @ %lu baud 8N1 (non-inverted), RX = GPIO%d\r\n",
                  (unsigned long)JETI_BAUD, IN_PIN);
    Serial.println("  Decodes channel-data packets (data id 0x31); telemetry/JetiBox ignored.");
    Serial.println("  Set the Rx pin to 'EX Bus'. If no lock, rebuild with -DJETI_BAUD=250000.");
    Serial.println();

    for (int i = 0; i < JETI_MAX_CH; i++) g_chan[i] = 1500;

    Serial1.begin(JETI_BAUD, SERIAL_8N1, IN_PIN, -1, false);
    Serial.printf("[jeti] UART1 RX bound to GPIO%d - waiting...\r\n\r\n", IN_PIN);
}

static void modeLoop() {
    const uint32_t now = millis();

    while (Serial1.available()) {
        const uint8_t b = (uint8_t)Serial1.read();
        g_rawBytes++;
        if (jeti_idx == 0) {                          // [0] hunt for header
            if (b != JETI_HDR_MASTER && b != JETI_HDR_SLAVE) continue;
            jeti_buf[jeti_idx++] = b;
        } else if (jeti_idx == 1) {                   // [1] resp/data byte
            if (b != 0x01 && b != 0x03) { jeti_idx = 0; continue; }
            jeti_buf[jeti_idx++] = b;
        } else if (jeti_idx == 2) {                   // [2] length byte
            if (b < 8 || b > JETI_MAX_PKT) { jeti_idx = 0; continue; }
            jeti_need = b;
            jeti_buf[jeti_idx++] = b;
        } else {                                      // [3..] body + CRC
            jeti_buf[jeti_idx++] = b;
            if (jeti_idx >= jeti_need) {
                jetiHandlePacket(jeti_buf, jeti_need, now);
                jeti_idx = 0;
            }
        }
    }

    const uint32_t ageMs  = g_lastFrameMs ? (now - g_lastFrameMs) : 0xFFFFFFFFu;
    const bool     signal = (g_lastFrameMs != 0) && (ageMs < SIGNAL_TIMEOUT_MS);
    heartbeat(signal);

    static uint32_t lastPrintMs = 0;
    if (now - lastPrintMs < PRINT_MS) { vTaskDelay(pdMS_TO_TICKS(1)); return; }
    lastPrintMs = now;

    static uint32_t lastFrames = 0;
    const uint32_t dFrames = g_frameCount - lastFrames;
    lastFrames = g_frameCount;
    const float fps = (float)dFrames * 1000.0f / (float)PRINT_MS;

    static bool hadSignal = false;
    if (!signal) {
        if (hadSignal) Serial.println("[jeti] *** SIGNAL LOST ***");
        hadSignal = false;
        static uint32_t lastWait = 0;
        if (now - lastWait > 2000) {
            lastWait = now;
            Serial.printf("[jeti] no channel packets on GPIO%d @ %lu baud "
                          "(rawBytes %lu, crcErr %lu) - check Rx 'EX Bus' mode / baud\r\n",
                          IN_PIN, (unsigned long)JETI_BAUD,
                          (unsigned long)g_rawBytes, (unsigned long)g_crcErr);
        }
        return;
    }
    if (!hadSignal) Serial.printf("[jeti] *** SIGNAL DETECTED *** %d channels\r\n", g_chanCount);
    hadSignal = true;

    char vals[160];
    int pos = 0;
    for (uint8_t i = 0; i < g_chanCount && i < JETI_MAX_CH; i++) {
        pos += snprintf(vals + pos, sizeof(vals) - pos, i ? " %4u" : "%4u", g_chan[i]);
        if (pos >= (int)sizeof(vals)) break;
    }

    // JETI [12ch] | 1500 1499 ... | fps=50 age=2ms crcErr=0
    Serial.printf("JETI [%dch] | %s | fps=%.0f age=%lums crcErr=%lu\r\n",
                  g_chanCount, vals, fps, (unsigned long)ageMs, (unsigned long)g_crcErr);
}

#endif  // MONITOR_MODE

// ── Setup / loop (mode-agnostic shell) ──────────────────────────────
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 2000)) delay(10);

    Serial.println();
    Serial.println("================================================================");
    Serial.println("  Input Monitor - IN_1 (GPIO5)");
    Serial.println("================================================================");
    Serial.printf("  CPU %u MHz | free heap %lu B\r\n",
                  ESP.getCpuFreqMHz(), (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Input pin : GPIO%d (IN_1)\r\n", IN_PIN);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    modeSetup();
}

void loop() {
    modeLoop();
}
