/**
 * Jeti EX Bus telemetry RESPONDER — bench rig.
 *
 * Validates the half-duplex turnaround the production telemetry concentrator
 * needs: on IN_1 (GPIO5) it READS RC channel data from a Jeti receiver AND
 * RESPONDS to the receiver's telemetry polls with two EX sensors:
 *     id 1  HubFx:Random   — a random number (changes every response)
 *     id 2  HubFx:Version   — firmware version * 100 (e.g. 216 = v2.16)
 *
 * Why this rig exists: the input_monitor bench found that ESP32
 * UART_MODE_RS485_HALF_DUPLEX *breaks RX*.  The working approach (per the
 * Sepp62/JetiExBus ESP32 library) is the GPIO matrix: the pin is RX-only by
 * default (pinMatrixOutDetach), and UART TX is attached (pinMatrixOutAttach)
 * ONLY for the duration of a response, then detached back to RX.
 *
 * It also uses the CORRECT EX Bus frame layout (with the type byte at [1]),
 * which the production decoder needed fixing for and the production responder
 * still gets wrong — this rig is the reference.
 *
 *   Master poll  : [0x3E][0x01][len][pktId][0x3A][0x00]                [crc16]
 *   Slave reply  : [0x3B][0x01][len][pktId][0x3A][subLen][EX data]     [crc16]
 *   EX data      : [0x9F sep][type|len][USN16LE][LSN16LE][res][payload][crc8]
 *                  (per Jeti "EX Bus protocol v1.21" spec; docs/EX_Bus_protocol_v1.21_EN.pdf)
 *                  type bits: 0b01<<6 = data (values), 0b00 = text (labels)
 *                  typeLen: bits7-6 = 00 data / 01 text, bits5-0 = length
 *
 * Console (channels + diag) on UART0 @ 115200 (CH343).  NO ScaleFX libs.
 */

#include <Arduino.h>
#include <esp_timer.h>
#include "soc/gpio_sig_map.h"   // U1TXD_OUT_IDX — UART1 TX matrix signal

// ── Config ──────────────────────────────────────────────────────────
static constexpr int      IN_PIN   = 5;        // IN_1 — Jeti EX Bus wire
static constexpr int      PIN_LED  = 48;
static constexpr uint32_t PRINT_MS = 200;
#ifndef JETI_BAUD
#define JETI_BAUD 125000
#endif
static constexpr uint16_t FW_VERSION_X100 = 216;   // HubFx:Version → 2.16

// EX Bus frame bytes.
static constexpr uint8_t HDR_MASTER0 = 0x3E;   // master, response allowed at [1]==0x01
static constexpr uint8_t HDR_MASTER1 = 0x3D;
static constexpr uint8_t HDR_SLAVE   = 0x3B;   // our reply header
static constexpr uint8_t DATA_CHANNEL   = 0x31;
static constexpr uint8_t DATA_TELEMETRY = 0x3A;
static constexpr uint8_t EX_SEP         = 0x9F;   // EX telemetry separator (0xNF, NOT 0x7E)
static constexpr uint8_t MAX_FRAME      = 64;
static constexpr uint8_t MIN_FRAME      = 8;

HardwareSerial JetiSerial(1);   // UART1

// ── Sensors (id, label, unit, value) ────────────────────────────────
struct Sensor { uint8_t id; const char* label; const char* unit; uint8_t dp; int32_t value; };
static Sensor sensors[] = {
    { 1, "Random",  "",  0, 0 },               // dp=0 → integer
    { 2, "Version", "",  2, FW_VERSION_X100 },  // dp=2 → 216 shows as "2.16"
};
static constexpr uint8_t SENSOR_COUNT = sizeof(sensors) / sizeof(sensors[0]);
static const char* DEVICE_NAME = "HubFx";
// EX device identity — USN (manufacturer) + LSN (serial); must be identical
// across the device-name text, sensor-label texts and value messages so the
// radio links them.  LSN low byte = sensor group (0 for ids < 16).
static constexpr uint16_t USN_ID    = 0xA400; // manufacturer (arbitrary)
static constexpr uint8_t  LSN_HI    = 0x01;   // serial high; low byte = group

// ── Channel decode state (printed to console) ───────────────────────
static volatile uint16_t g_chan[24];
static volatile uint8_t  g_chanCount = 0;
static volatile uint32_t g_lastChanMs = 0;

// ── Diagnostics ─────────────────────────────────────────────────────
static uint32_t g_rxBytes = 0, g_frames = 0, g_chanFrames = 0,
                g_telemReq = 0, g_telemTx = 0, g_crcErr = 0;

// ── CRC ─────────────────────────────────────────────────────────────
// Jeti EX Bus frame CRC16 — reflected CCITT (poly 0x8408, init 0).
static uint16_t crc16(const uint8_t* p, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}
// EX telemetry data CRC8 — poly 0x07, init 0.
static uint8_t crc8(const uint8_t* p, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

// ── Half-duplex (GPIO matrix) ───────────────────────────────────────
// Default RX-only: TX detached from the pin, RX matrix stays attached.
static inline void jetiTxOn()  { pinMatrixOutAttach(IN_PIN, U1TXD_OUT_IDX, false, false); }
static inline void jetiTxOff() { pinMatrixOutDetach(IN_PIN, false, false); pinMode(IN_PIN, INPUT_PULLUP); }

// ── EX data builders → write into exBuf, return length ──────────────
// Per the spec, every EX message shares a 7-byte head:
//   [0] 0x9F sep · [1] type|len · [2,3] USN · [4,5] LSN · [6] reserved
// then data (sensor descriptors + values) or text (id + label/unit), CRC8 last.
// `len` (low 6 bits of [1]) counts bytes [1..lastdata] — sep and CRC8 excluded.
static uint8_t exHead(uint8_t* b, uint8_t group /*=id&0xF0*/) {
    b[0] = EX_SEP;
    // b[1] = type|len filled by caller after the body is laid down.
    b[2] = USN_ID & 0xFF; b[3] = USN_ID >> 8;          // USN (manufacturer)
    b[4] = group;         b[5] = LSN_HI;               // LSN (serial), low = group
    b[6] = 0x00;                                       // reserved
    return 7;
}

// Encode one Int14 sensor entry: [id<<4 | type=1][value_lo][sign|dp|mag_hi].
static uint8_t encodeInt14(uint8_t* buf, uint8_t id, uint8_t dp, int32_t value) {
    uint8_t sign = 0; uint32_t mag = (uint32_t)value;
    if (value < 0) { sign = 1; mag = (uint32_t)(-value); }
    buf[0] = (uint8_t)(((id & 0x0F) << 4) | 0x01);            // sensor id | type 1 (Int14)
    buf[1] = (uint8_t)(mag & 0xFF);                           // value LOW byte first
    buf[2] = (uint8_t)((sign << 7) | ((dp & 0x03) << 5) | ((mag >> 8) & 0x1F));
    return 3;
}

// Build an EX DATA message (sensor value) into exBuf; returns length.
static uint8_t buildExData(uint8_t* exBuf, const Sensor& s) {
    uint8_t pos = exHead(exBuf, s.id & 0xF0);
    pos += encodeInt14(&exBuf[pos], s.id, s.dp, s.value);
    exBuf[1] = (uint8_t)(0x40 | (pos - 1));            // type 0b01 (data) | len[1..pos-1]
    exBuf[pos] = crc8(&exBuf[1], pos - 1); pos++;
    return pos;
}

// Build an EX TEXT message (label+unit, or device name when id==0).
static uint8_t buildExText(uint8_t* exBuf, uint8_t id, const char* label, const char* unit) {
    uint8_t pos = exHead(exBuf, id & 0xF0);
    exBuf[pos++] = (uint8_t)(id & 0x0F);              // [7] sensor id (0 = device name)
    uint8_t llen = label ? (uint8_t)strlen(label) : 0; if (llen > 20) llen = 20;
    uint8_t ulen = unit  ? (uint8_t)strlen(unit)  : 0; if (ulen > 7)  ulen = 7;
    exBuf[pos++] = (uint8_t)((llen << 3) | (ulen & 0x07));  // [8] label/unit lengths
    for (uint8_t i = 0; i < llen; i++) exBuf[pos++] = (uint8_t)label[i];
    for (uint8_t i = 0; i < ulen; i++) exBuf[pos++] = (uint8_t)unit[i];
    exBuf[1] = (uint8_t)(0x00 | (pos - 1));           // type 0b00 (text) | len[1..pos-1]
    exBuf[pos] = crc8(&exBuf[1], pos - 1); pos++;
    return pos;
}

// Wrap an EX data/text payload in an EX Bus slave response frame + TX it
// half-duplex.  [0x3B][0x01][totalLen][pktId][0x3A][subLen][payload][crc16].
static void sendResponse(uint8_t pktId, const uint8_t* payload, uint8_t payloadLen) {
    uint8_t frame[80];
    uint8_t total = (uint8_t)(6 + payloadLen + 2);   // header(6) + payload + crc16(2)
    frame[0] = HDR_SLAVE;
    frame[1] = 0x01;                                  // type byte — REQUIRED (the bug the prod responder misses)
    frame[2] = total;
    frame[3] = pktId;
    frame[4] = DATA_TELEMETRY;
    frame[5] = payloadLen;
    memcpy(&frame[6], payload, payloadLen);
    uint16_t c = crc16(frame, total - 2);
    frame[total - 2] = c & 0xFF;
    frame[total - 1] = c >> 8;

    jetiTxOn();
    JetiSerial.write(frame, total);
    JetiSerial.flush();                 // wait for the TX FIFO + shift register
    delayMicroseconds(20);              // guard for the final stop bit
    jetiTxOff();
    // Discard our own transmission echoed back on the shared wire.
    while (JetiSerial.available()) JetiSerial.read();
    g_telemTx++;
}

// Rotate telemetry: device name + sensor labels periodically, values between.
static void respondTelemetry(uint8_t pktId) {
    static uint8_t turn = 0;
    uint8_t exBuf[48], len;
    switch (turn % 8) {
        case 0: len = buildExText(exBuf, 0, DEVICE_NAME, "");                 break;
        case 2: len = buildExText(exBuf, sensors[0].id, sensors[0].label, sensors[0].unit); break;
        case 4: len = buildExText(exBuf, sensors[1].id, sensors[1].label, sensors[1].unit); break;
        default: len = buildExData(exBuf, sensors[turn & 1]);                  break;
    }
    turn++;
    sendResponse(pktId, exBuf, len);
}

// ── EX Bus frame parser (RX) ────────────────────────────────────────
static uint8_t  fbuf[MAX_FRAME];
static int      fidx = 0, fneed = 0;
static enum { IDLE, TYPE, LEN, BODY } fstate = IDLE;

static void processFrame() {
    const uint16_t want = (uint16_t)(fbuf[fneed - 2] | (fbuf[fneed - 1] << 8));
    if (crc16(fbuf, fneed - 2) != want) { g_crcErr++; return; }
    g_frames++;
    const uint8_t typeByte = fbuf[1];
    const uint8_t dataId   = fbuf[4];
    const uint8_t subLen   = fbuf[5];
    if (dataId == DATA_CHANNEL) {
        g_chanFrames++;
        uint8_t n = subLen / 2; if (n > 24) n = 24;
        for (uint8_t i = 0; i < n; i++)
            g_chan[i] = (uint16_t)((fbuf[6 + i * 2] | (fbuf[7 + i * 2] << 8)) / 8);  // 1/8us → us
        g_chanCount = n;
        g_lastChanMs = millis();
    } else if (dataId == DATA_TELEMETRY && typeByte == 0x01) {
        // Master is polling us for telemetry and allows a response.
        g_telemReq++;
        respondTelemetry(fbuf[3] /*pktId*/);
    }
}

static void pumpRx() {
    while (JetiSerial.available()) {
        const uint8_t b = (uint8_t)JetiSerial.read();
        g_rxBytes++;
        switch (fstate) {
        case IDLE:
            if (b == HDR_MASTER0 || b == HDR_MASTER1 || b == HDR_SLAVE) {
                fbuf[0] = b; fidx = 1; fstate = TYPE;
            }
            break;
        case TYPE: fbuf[1] = b; fidx = 2; fstate = LEN; break;
        case LEN:
            fbuf[2] = b; fneed = b; fidx = 3;
            if (fneed < MIN_FRAME || fneed > MAX_FRAME) { g_crcErr++; fstate = IDLE; }
            else fstate = BODY;
            break;
        case BODY:
            if (fidx < MAX_FRAME) fbuf[fidx] = b;
            fidx++;
            if (fidx >= fneed) { processFrame(); fstate = IDLE; }
            break;
        }
    }
}

// ── Setup / loop ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 2000)) delay(10);
    Serial.println();
    Serial.println("================================================================");
    Serial.println("  Jeti EX Bus telemetry RESPONDER - IN_1 (GPIO5), UART1");
    Serial.println("================================================================");
    Serial.printf("  Baud %d 8N1 | half-duplex via GPIO matrix (RX-only default)\r\n", JETI_BAUD);
    Serial.printf("  Sensors: id1 HubFx:Random, id2 HubFx:Version=%u\r\n", FW_VERSION_X100);
    Serial.println("  Set the Rx pin to 'EX Bus'. Channels printed; telemetry auto-replies.");
    Serial.println();

    pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);

    // Bring UART1 up with tx=rx=IN_PIN, then detach TX → RX-only idle.
    JetiSerial.begin(JETI_BAUD, SERIAL_8N1, IN_PIN, IN_PIN, false);
    jetiTxOff();

    Serial.println("[jtx] UART1 up, RX-only - waiting for EX Bus...\r\n");
}

void loop() {
    pumpRx();
    sensors[0].value = (int32_t)(esp_random() & 0x1FFF);   // Random: 0..8191

    static uint32_t lastPrint = 0, ledMs = 0;
    const uint32_t now = millis();
    const bool sig = g_lastChanMs && (now - g_lastChanMs < 500);
    if (now - ledMs > (sig ? 800u : 150u)) { ledMs = now; digitalWrite(PIN_LED, !digitalRead(PIN_LED)); }

    if (now - lastPrint < PRINT_MS) { vTaskDelay(pdMS_TO_TICKS(1)); return; }
    lastPrint = now;

    if (!sig) {
        static uint32_t lastWait = 0;
        if (now - lastWait > 2000) {
            lastWait = now;
            Serial.printf("[jtx] no EX Bus (rxB=%lu frames=%lu crcErr=%lu)\r\n",
                          (unsigned long)g_rxBytes, (unsigned long)g_frames, (unsigned long)g_crcErr);
        }
        return;
    }

    char vals[160]; int p = 0;
    for (uint8_t i = 0; i < g_chanCount && i < 24; i++)
        p += snprintf(vals + p, sizeof(vals) - p, i ? " %4u" : "%4u", g_chan[i]);
    Serial.printf("CH[%2u] | %s | telemReq=%lu telemTx=%lu rnd=%ld crcErr=%lu\r\n",
                  g_chanCount, vals,
                  (unsigned long)g_telemReq, (unsigned long)g_telemTx,
                  (long)sensors[0].value, (unsigned long)g_crcErr);
}
