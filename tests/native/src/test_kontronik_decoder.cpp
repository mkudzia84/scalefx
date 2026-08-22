/*
 * test_kontronik_decoder.cpp — Kontronik KODL/KODI frame decode.
 *
 *   Locks in the DEVICE-DEPENDENT frame length + CRC region (spec V5
 *   column marks + the 2026-08-12 bench raw capture): KOSMIK/KOLIBRI =
 *   KODL 40 B / KODI 44 B, CRC over frame-minus-trailer; JIVE PRO =
 *   KODL 38 B (no Reserved pair, standard offsets) with the CRC covering
 *   only the FIRST 32 bytes (state + timing uncovered — firmware quirk
 *   verified bit-perfect against two live captured frames).  The bench
 *   regression: a JIVE PRO streamed clean magic but the fixed-40 decoder
 *   rejected 100 % of frames (errs == rxB byte-for-byte).
 */

#include "doctest.h"
#include "kontronik_decoder.h"

#include <cstdint>
#include <cstring>

using sfx_peripherals::EscFeed;
using sfx_peripherals::EscTelemData;
using sfx_peripherals::KontronikDecoder;
using sfx_peripherals::esc_detail::crc32r;

namespace {

// Build a KODL live frame of the given total length (38 = JIVE PRO,
// 40 = KOSMIK/KOLIBRI) with plausible field values and a valid LE CRC.
// Field offsets are identical in both variants; the JIVE PRO's CRC covers
// only the first 32 bytes (its state/timing bytes are uncovered).
size_t buildKodl(uint8_t* f, size_t total) {
    std::memset(f, 0, total);
    f[0] = 'K'; f[1] = 'O'; f[2] = 'D'; f[3] = 'L';
    const uint32_t rpm = 12345;
    f[4] = (uint8_t)rpm; f[5] = (uint8_t)(rpm >> 8); f[6] = 0; f[7] = 0;
    f[8]  = 0x99; f[9]  = 0x09;   // 0x0999 * 10 mV = 24.57 V
    f[10] = 0x2A; f[11] = 0x00;   // 4.2 A in 0.1 A units
    f[16] = 0xE8; f[17] = 0x03;   // 1000 mAh
    f[24] = 42;                   // throttle %
    f[26] = 55;                   // ESC temp
    f[32] = 12;                   // state = MotorOff (uncovered on JIVE PRO)
    const uint32_t crc = crc32r(f, (total == 38) ? 32 : total - 4);
    f[total - 4] = (uint8_t)crc;
    f[total - 3] = (uint8_t)(crc >> 8);
    f[total - 2] = (uint8_t)(crc >> 16);
    f[total - 1] = (uint8_t)(crc >> 24);
    return total;
}

// Build a KODI info frame (41 = JIVE PRO, 44 = KOSMIK/KOLIBRI).
// deviceIdx per spec: 1=KOSMIK, 2=KOLIBRI, 3=JIVEPro.  The JIVE PRO's
// CRC covers only the first 36 bytes (measured from a live capture).
size_t buildKodi(uint8_t* f, size_t total, uint8_t deviceIdx) {
    std::memset(f, 0, total);
    f[0] = 'K'; f[1] = 'O'; f[2] = 'D'; f[3] = 'I';
    const uint16_t devVar = (uint16_t)(deviceIdx << 10);
    f[4] = (uint8_t)devVar; f[5] = (uint8_t)(devVar >> 8);
    f[6] = 14; f[7] = 1;          // fw 1.14 (sub, main)
    const uint32_t crc = crc32r(f, (total == 41) ? 36 : total - 4);
    f[total - 4] = (uint8_t)crc;
    f[total - 3] = (uint8_t)(crc >> 8);
    f[total - 2] = (uint8_t)(crc >> 16);
    f[total - 1] = (uint8_t)(crc >> 24);
    return total;
}

// Feed a buffer through the decoder; return the LAST non-None event.
EscFeed feedAll(KontronikDecoder& dec, const uint8_t* p, size_t n, EscTelemData& d,
                int* errCount = nullptr) {
    EscFeed last = EscFeed::None;
    for (size_t i = 0; i < n; ++i) {
        const EscFeed ev = dec.feed(p[i], d);
        if (ev == EscFeed::Error && errCount) ++(*errCount);
        if (ev != EscFeed::None) last = ev;
    }
    return last;
}

}  // namespace

TEST_CASE("KODL 40 B (KOSMIK/KOLIBRI) decodes — the 2026-07-14 bench baseline") {
    KontronikDecoder dec;
    EscTelemData d{};
    uint8_t f[48];
    const size_t n = buildKodl(f, 40);
    CHECK(feedAll(dec, f, n, d) == EscFeed::Live);
    CHECK(d.rpm == 12345);
    CHECK(d.voltage_mV == 24570);
    CHECK(d.current_cA == 420);
    CHECK(d.capacity_mAh == 1000);
    CHECK(d.throttlePct == 42);
    CHECK(d.tempEsc_C == 55);
}

TEST_CASE("KODL 38 B (JIVE PRO — short CRC region) decodes with standard offsets") {
    KontronikDecoder dec;
    EscTelemData d{};
    uint8_t f[48];
    const size_t n = buildKodl(f, 38);
    CHECK(feedAll(dec, f, n, d) == EscFeed::Live);
    CHECK(d.rpm == 12345);
    CHECK(d.voltage_mV == 24570);
    CHECK(d.throttlePct == 42);
    CHECK(d.tempEsc_C == 55);
}

TEST_CASE("the real captured JIVE PRO KODI frame decodes — 41 B, CRC over 36") {
    static const uint8_t kInfo[41] = {
        0x4B,0x4F,0x44,0x49, 0x78,0x0C, 0x0E,0x01, 0x00,0x00,0x00,0x00,
        0x06, 0x00,0x00,0x00,0x00, 0x6E,0x09, 0x00,0x00, 0x00,0x00, 0x00,
        0x00,0x00,0x00,0x00, 0x00, 0xDF,0x15, 0x00, 0x00, 0x1B, 0x1E,
        0x1C, 0x21, 0x90,0x4A,0xE8,0xA9};
    KontronikDecoder dec;
    EscTelemData d{};
    CHECK(feedAll(dec, kInfo, sizeof kInfo, d) == EscFeed::Info);
    CHECK(std::strcmp(d.deviceName, "JIVE PRO") == 0);
    CHECK(d.fwVersion == ((1u << 8) | 14));
}

TEST_CASE("the real captured JIVE PRO frame decodes bit-for-bit") {
    // Two live frames captured 2026-08-12 (raw 48 B snapshot) — the CRC
    // trailer only validates over the first 32 bytes.
    static const uint8_t kFrame[38] = {
        0x4B,0x4F,0x44,0x4C, 0x00,0x00,0x00,0x00, 0x8A,0x09, 0x00,0x00,
        0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x58,0x1F, 0xE9,0x03,
        0x00,0x00, 0x1D,0x20, 0x00,0x00,0x02,0x00, 0x0C,0x00,
        0x39,0x23,0x4C,0x14};
    KontronikDecoder dec;
    EscTelemData d{};
    CHECK(feedAll(dec, kFrame, sizeof kFrame, d) == EscFeed::Live);
    CHECK(d.voltage_mV == 24420);         // 24.42 V pack
    CHECK(d.becVoltage_mV == 8024);       // 8.02 V BEC
    CHECK(d.tempEsc_C == 29);
    CHECK(d.tempBec_C == 32);
    CHECK(d.faults == 0x00020000u);       // ProgAllow only (benign, masked)
}

TEST_CASE("back-to-back JIVE PRO frames stay locked — no inter-frame error churn") {
    KontronikDecoder dec;
    EscTelemData d{};
    uint8_t f[48];
    const size_t n = buildKodl(f, 38);
    int errs = 0, lives = 0;
    for (int k = 0; k < 5; ++k) {
        if (feedAll(dec, f, n, d, &errs) == EscFeed::Live) ++lives;
    }
    CHECK(lives == 5);
    CHECK(errs == 0);
}

TEST_CASE("KODI 44 B / 41 B decode the device name from the high-6-bit index") {
    uint8_t f[48];

    KontronikDecoder dec1;
    EscTelemData d1{};
    buildKodi(f, 44, /*deviceIdx=*/2);
    CHECK(feedAll(dec1, f, 44, d1) == EscFeed::Info);
    CHECK(std::strcmp(d1.deviceName, "KOLIBRI") == 0);
    CHECK(d1.fwVersion == ((1u << 8) | 14));

    KontronikDecoder dec2;
    EscTelemData d2{};
    buildKodi(f, 41, /*deviceIdx=*/3);
    CHECK(feedAll(dec2, f, 41, d2) == EscFeed::Info);
    CHECK(std::strcmp(d2.deviceName, "JIVE PRO") == 0);
}

TEST_CASE("mid-stream attach onto a JIVE PRO stream locks and decodes — the pinned-window regression") {
    // The device attaches mid-frame: the decoder sees the TAIL of one frame
    // first, then continuous back-to-back frames.  The old `_len == cand`
    // candidate test never re-fired once the window pinned full at the max
    // candidate — this is the exact 2026-08-12 frames=0/errs=rxB signature.
    KontronikDecoder dec;
    EscTelemData d{};
    uint8_t f[48];
    const size_t n = buildKodl(f, 38);
    int errs = 0, lives = 0;
    // Half a frame's tail (no magic), as seen at attach time...
    for (size_t i = 20; i < n; ++i) {
        EscTelemData scratch{};
        dec.feed(f[i], scratch);
    }
    // ...then a continuous stream of whole frames.
    for (int k = 0; k < 4; ++k) {
        if (feedAll(dec, f, n, d, &errs) == EscFeed::Live) ++lives;
    }
    CHECK(lives >= 3);           // first frame may be eaten by the re-lock
    CHECK(d.rpm == 12345);
    CHECK(errs <= 1);
}

TEST_CASE("corrupted CRC is rejected, then the stream re-locks on the next frame") {
    KontronikDecoder dec;
    EscTelemData d{};
    uint8_t bad[48], good[48];
    const size_t n = buildKodl(bad, 40);
    bad[39] ^= 0xFF;                 // trash the CRC trailer
    buildKodl(good, 40);
    int errs = 0;
    CHECK(feedAll(dec, bad, n, d, &errs) != EscFeed::Live);
    CHECK(errs >= 1);
    // A fresh valid frame right after must still decode (shift-resync).
    CHECK(feedAll(dec, good, 40, d, &errs) == EscFeed::Live);
    CHECK(d.rpm == 12345);
}
