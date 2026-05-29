// Tests for controllers/lib/sfx_serial/serial/wire.cpp
//
// Locks in the lowest-level wire format the entire ScaleFX protocol
// depends on:
//   - CRC-8 polynomial 0x07
//   - COBS encode/decode round-trip
//   - U16/U32/I16 little-endian helpers
//   - buildPacket / encodePacket / parsePacket
//
// The Go-side equivalents in tests/host/go_unit/protocol_test/ exercise
// the same algorithms against the SAME inputs — keeping these two
// suites in sync is the strongest cross-language drift detector
// available.  If the C++ CRC ever stops agreeing with the Go CRC for
// some byte, ONE of the two test suites will catch it.

#include "doctest.h"
#include "wire.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace SfxWire;

// ---- CRC-8 -----------------------------------------------------------

TEST_CASE("crc8 empty input is 0") {
    CHECK(crc8(nullptr, 0) == 0x00);
}

TEST_CASE("crc8 single byte") {
    // CRC-8 poly 0x07 of {0x01} — verified against a reference impl.
    const uint8_t one = 0x01;
    CHECK(crc8(&one, 1) == 0x07);
}

TEST_CASE("crc8 known vectors") {
    // Standard test vectors for CRC-8 / SMBus (poly 0x07, init 0x00,
    // no reflect, no XOR-out).  Source: Sunshine's CRC table generator.
    struct { std::vector<uint8_t> in; uint8_t want; } cases[] = {
        {{0x00},                                  0x00},
        {{0x00, 0x00, 0x00, 0x00},                0x00},
        {{0xFF},                                  0xF3},
        {{0x01, 0x02, 0x03, 0x04, 0x05},          0xBC},
        {{'1','2','3','4','5','6','7','8','9'},   0xF4},  // "123456789"
    };
    for (const auto& tc : cases) {
        CAPTURE(tc.in.size());
        CHECK(crc8(tc.in.data(), tc.in.size()) == tc.want);
    }
}

TEST_CASE("crc8 over packet header rebuilds correctly") {
    // What the real protocol does: CRC over [type][tag][len_lo][len_hi]
    // + payload.  The payload "AB" is 2 bytes.
    uint8_t pkt[] = {0xC0, 0x01, 0x02, 0x00, 'A', 'B'};
    uint8_t want = crc8(pkt, sizeof(pkt));
    // CRC of the same buffer twice must produce the same result —
    // stateless function, no global side effects.
    CHECK(crc8(pkt, sizeof(pkt)) == want);
}

// ---- COBS encode -----------------------------------------------------

TEST_CASE("cobsEncode empty input returns 0") {
    uint8_t out[16];
    CHECK(cobsEncode(nullptr, 0, out) == 0);
}

TEST_CASE("cobsEncode all-nonzero produces overhead=1") {
    // No zeros in input → overhead byte is the input length + 1.
    uint8_t in[] = {0x11, 0x22, 0x33};
    uint8_t out[16] = {};
    size_t n = cobsEncode(in, sizeof(in), out);
    // Encoded form: [4][0x11][0x22][0x33]
    REQUIRE(n == 4);
    CHECK(out[0] == 0x04);
    CHECK(out[1] == 0x11);
    CHECK(out[2] == 0x22);
    CHECK(out[3] == 0x33);
}

TEST_CASE("cobsEncode single zero byte") {
    uint8_t in[] = {0x00};
    uint8_t out[16] = {};
    size_t n = cobsEncode(in, sizeof(in), out);
    // {0x00} encodes to {0x01, 0x01}: one byte "code 1" telling the
    // decoder to write a zero, then a terminating "code 1".
    REQUIRE(n == 2);
    CHECK(out[0] == 0x01);
    CHECK(out[1] == 0x01);
}

TEST_CASE("cobsEncode zero in middle") {
    uint8_t in[] = {0x11, 0x00, 0x22};
    uint8_t out[16] = {};
    size_t n = cobsEncode(in, sizeof(in), out);
    // Encoded: [2][0x11][2][0x22]
    REQUIRE(n == 4);
    CHECK(out[0] == 0x02);
    CHECK(out[1] == 0x11);
    CHECK(out[2] == 0x02);
    CHECK(out[3] == 0x22);
}

// ---- COBS round-trip -------------------------------------------------

TEST_CASE("cobs round-trip preserves data") {
    struct { std::vector<uint8_t> data; } cases[] = {
        {{0x01}},
        {{0x00}},
        {{0x00, 0x00, 0x00}},
        {{0x11, 0x22, 0x33, 0x44, 0x55}},
        {{0xFF, 0x00, 0xFF, 0x00, 0xFF}},
        // 254 consecutive non-zero bytes — exercises the COBS 0xFF
        // chain-break (encoder splits into multiple blocks).
        std::vector<uint8_t>(254, 0xAA),
        // 255 consecutive non-zero bytes — same edge case + 1.
        std::vector<uint8_t>(255, 0xAB),
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const auto& in = cases[i].data;
        CAPTURE(i);
        CAPTURE(in.size());

        uint8_t encoded[600] = {};
        size_t enc_n = cobsEncode(in.data(), in.size(), encoded);
        REQUIRE(enc_n > 0);
        // Encoded form must contain no zero bytes (that's the whole
        // point of COBS — zero becomes the frame delimiter).
        for (size_t j = 0; j < enc_n; ++j) {
            CHECK(encoded[j] != 0x00);
        }

        uint8_t decoded[600] = {};
        size_t dec_n = cobsDecode(encoded, enc_n, decoded, sizeof(decoded));
        REQUIRE(dec_n == in.size());
        CHECK(memcmp(decoded, in.data(), in.size()) == 0);
    }
}

TEST_CASE("cobsDecode rejects zero byte in encoded stream") {
    // A zero in encoded data is supposed to be the frame delimiter
    // (stripped before decode).  If one slips through, decode must
    // fail rather than producing garbage.
    uint8_t bad[] = {0x02, 0x11, 0x00, 0x22};
    uint8_t out[16] = {};
    CHECK(cobsDecode(bad, sizeof(bad), out, sizeof(out)) == 0);
}

TEST_CASE("cobsDecode respects maxOutput bound") {
    uint8_t in[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t encoded[16] = {};
    size_t enc_n = cobsEncode(in, sizeof(in), encoded);

    uint8_t out[2] = {};  // intentionally too small
    CHECK(cobsDecode(encoded, enc_n, out, sizeof(out)) == 0);
}

// ---- U16LE / U32LE / I16LE helpers ----------------------------------

TEST_CASE("putU16LE / getU16LE round-trip") {
    uint8_t buf[2];
    for (uint32_t v : {0u, 1u, 0x00FFu, 0x0100u, 0xBEEFu, 0xFFFFu}) {
        CAPTURE(v);
        putU16LE(buf, (uint16_t)v);
        CHECK(getU16LE(buf) == (uint16_t)v);
    }
}

TEST_CASE("putU16LE byte order is little-endian") {
    uint8_t buf[2];
    putU16LE(buf, 0xBEEF);
    CHECK(buf[0] == 0xEF);
    CHECK(buf[1] == 0xBE);
}

TEST_CASE("putU32LE / getU32LE round-trip") {
    uint8_t buf[4];
    for (uint64_t v : {0u, 1u, 0x12345678u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
        CAPTURE(v);
        putU32LE(buf, (uint32_t)v);
        CHECK(getU32LE(buf) == (uint32_t)v);
    }
}

TEST_CASE("putU32LE byte order is little-endian") {
    uint8_t buf[4];
    putU32LE(buf, 0xDEADBEEF);
    CHECK(buf[0] == 0xEF);
    CHECK(buf[1] == 0xBE);
    CHECK(buf[2] == 0xAD);
    CHECK(buf[3] == 0xDE);
}

TEST_CASE("putI16LE handles negative values via two's complement") {
    uint8_t buf[2];
    putI16LE(buf, -1);
    CHECK(getI16LE(buf) == -1);
    CHECK(buf[0] == 0xFF);
    CHECK(buf[1] == 0xFF);

    putI16LE(buf, -32768);
    CHECK(getI16LE(buf) == -32768);

    putI16LE(buf, 32767);
    CHECK(getI16LE(buf) == 32767);
}

// ---- buildPacket -----------------------------------------------------

TEST_CASE("buildPacket basic layout") {
    uint8_t payload[] = {'A', 'B'};
    uint8_t out[16] = {};

    size_t n = buildPacket(out, 0xC0, 0x01, payload, sizeof(payload));
    // header (4) + payload (2) + crc (1) = 7 bytes
    REQUIRE(n == 7);
    CHECK(out[0] == 0xC0);    // type
    CHECK(out[1] == 0x01);    // tag
    CHECK(out[2] == 0x02);    // len low
    CHECK(out[3] == 0x00);    // len high
    CHECK(out[4] == 'A');
    CHECK(out[5] == 'B');
    CHECK(out[6] == crc8(out, 6));
}

TEST_CASE("buildPacket with empty payload") {
    uint8_t out[16] = {};
    size_t n = buildPacket(out, 0xAA, 0x00, nullptr, 0);
    REQUIRE(n == 5);  // header + crc only
    CHECK(out[0] == 0xAA);
    CHECK(out[1] == 0x00);
    CHECK(out[2] == 0x00);
    CHECK(out[3] == 0x00);
    CHECK(out[4] == crc8(out, 4));
}

TEST_CASE("buildPacket rejects oversize payload") {
    std::vector<uint8_t> huge(MAX_PAYLOAD_SIZE + 1, 0xAA);
    uint8_t out[MAX_PACKET_SIZE + 16] = {};
    CHECK(buildPacket(out, 0xC0, 0x01, huge.data(), huge.size()) == 0);
}

TEST_CASE("buildPacket length is little-endian u16") {
    // Use a payload whose length doesn't fit in u8 so we can verify
    // the u16LE encoding picks up correct high byte.
    std::vector<uint8_t> p(300, 0x55);
    uint8_t out[MAX_PACKET_SIZE] = {};
    size_t n = buildPacket(out, 0x80, 0x42, p.data(), p.size());
    REQUIRE(n == 4 + 300 + 1);
    CHECK(out[2] == 0x2C);  // 300 & 0xFF
    CHECK(out[3] == 0x01);  // 300 >> 8
}

// ---- encodePacket round-trip with parsePacket -----------------------

TEST_CASE("encodePacket -> COBS-decode -> parsePacket round-trip") {
    uint8_t payload[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x00, 0xAB};

    uint8_t encoded[COBS_BUFFER_SIZE] = {};
    size_t enc_n = encodePacket(encoded, 0xC0, 0x42, payload, sizeof(payload));
    REQUIRE(enc_n > 0);
    // Last byte is the frame delimiter (0x00).
    CHECK(encoded[enc_n - 1] == FRAME_DELIMITER);
    // No other zeros allowed in the encoded stream.
    for (size_t i = 0; i < enc_n - 1; ++i) {
        CHECK(encoded[i] != 0x00);
    }

    uint8_t decoded[MAX_PACKET_SIZE] = {};
    size_t dec_n = cobsDecode(encoded, enc_n - 1, decoded, sizeof(decoded));
    REQUIRE(dec_n > 0);

    uint8_t got_type, got_tag;
    const uint8_t* got_payload = nullptr;
    size_t got_payload_len = 0;

    bool ok = parsePacket(decoded, dec_n, &got_type, &got_tag,
                          &got_payload, &got_payload_len);
    REQUIRE(ok);
    CHECK(got_type == 0xC0);
    CHECK(got_tag == 0x42);
    REQUIRE(got_payload_len == sizeof(payload));
    CHECK(memcmp(got_payload, payload, sizeof(payload)) == 0);
}

TEST_CASE("parsePacket rejects too-short input") {
    uint8_t buf[4] = {0xC0, 0x01, 0x00, 0x00};
    uint8_t type, tag;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    CHECK_FALSE(parsePacket(buf, sizeof(buf), &type, &tag, &payload, &payload_len));
}

TEST_CASE("parsePacket rejects bad CRC") {
    uint8_t payload[] = {'A', 'B'};
    uint8_t raw[8] = {};
    size_t n = buildPacket(raw, 0xC0, 0x01, payload, sizeof(payload));
    REQUIRE(n == 7);

    // Corrupt the CRC.
    raw[n - 1] ^= 0xFF;

    uint8_t type, tag;
    const uint8_t* p = nullptr;
    size_t plen = 0;
    CHECK_FALSE(parsePacket(raw, n, &type, &tag, &p, &plen));
}

TEST_CASE("parsePacket rejects length-field mismatch") {
    // A packet that claims 10 bytes of payload but is only sized for 2.
    // Even if the CRC happened to match, the size check fires first.
    uint8_t raw[7] = {0xC0, 0x01, 10, 0x00, 'A', 'B', 0x00};
    uint8_t type, tag;
    const uint8_t* p = nullptr;
    size_t plen = 0;
    CHECK_FALSE(parsePacket(raw, sizeof(raw), &type, &tag, &p, &plen));
}

// ---- Cross-language consistency check -------------------------------

// Sanity: the wire-format constants the Go side mirrors in
// tests/host/go_unit/storage_protocol_test must agree with the C++
// canonical values.  We can't import Go from C++, but we can pin the
// C++ values explicitly so a header drift here trips ONE side.
TEST_CASE("wire constants are protocol-locked") {
    CHECK(HEADER_SIZE == 4);
    CHECK(PICO_MAX_PAYLOAD == 512);
    CHECK(ESP32_MAX_PAYLOAD == 2048);
    CHECK(FRAME_DELIMITER == 0x00);
    CHECK(TAG_ASYNC == 0x00);
}
