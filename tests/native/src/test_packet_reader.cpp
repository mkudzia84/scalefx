// Tests for controllers/lib/sfx_serial/serial/packet_reader.h
//
// Locks in the COBS frame accumulator state machine that used to be
// inlined three times (SerialBus::process, BoardServerBase::readFrames,
// BoardServer<TStream>::readFrames).  All three call sites now share
// this class, so a bug here causes silent packet loss across every
// protocol consumer.  Tests cover:
//
//   - Empty input is harmless
//   - Single complete frame emits exactly once
//   - Buffered partial frame across two feedBytes calls
//   - Multi-frame interleaving in one call
//   - Repeated/leading FRAME_DELIMITER bytes are ignored (normal at boot)
//   - Buffer overflow drops and re-syncs cleanly; framingErrors() bumps
//   - feedByte (single-byte API) matches feedBytes semantics
//   - reset() / resetStats() do what they claim
//
// What we DON'T re-test here: the wire format inside the frame
// (CRC-8, COBS encode/decode, parsePacket).  That's `test_wire.cpp`.
// PacketReader is intentionally below that layer — it just hands the
// callback bytes between delimiters.

#include "doctest.h"

#include "packet_reader.h"
#include "wire.h"

#include <cstdint>
#include <vector>

using sfx_serial::PacketReader;
using sfx_serial::PacketReaderT;

// Small alias for tests so we can stress the overflow path without a
// 2 KB buffer eating the test stack.
using TinyReader = PacketReaderT<16>;

// ---- Helpers ---------------------------------------------------------

// Build a synthetic "frame" from a payload of N non-zero bytes.  The
// reader doesn't care whether the contents are COBS-valid — its job is
// to hand the buffer to the callback exactly as received between the
// delimiter and the previous emission.
static std::vector<uint8_t> makeFrame(uint8_t fill, std::size_t n) {
    REQUIRE(fill != 0);   // 0 would be a delimiter, not buffered content
    return std::vector<uint8_t>(n, fill);
}

// ---- Empty / no-op cases --------------------------------------------

TEST_CASE("feedBytes with length 0 is a no-op") {
    PacketReader r;
    int frames = r.feedBytes(nullptr, 0, [](const uint8_t*, std::size_t) {});
    CHECK(frames == 0);
    CHECK(r.bufferedBytes() == 0);
    CHECK(r.framingErrors() == 0);
}

TEST_CASE("leading FRAME_DELIMITER alone emits nothing") {
    PacketReader r;
    uint8_t bytes[] = {0x00, 0x00, 0x00};
    int frames = r.feedBytes(bytes, 3, [](const uint8_t*, std::size_t) {
        FAIL("callback should not fire on bare delimiters");
    });
    CHECK(frames == 0);
    CHECK(r.bufferedBytes() == 0);
}

// ---- One complete frame ----------------------------------------------

TEST_CASE("complete frame fires callback exactly once") {
    PacketReader r;
    auto data = makeFrame(0xAB, 8);
    data.push_back(SfxWire::FRAME_DELIMITER);

    int callbacks = 0;
    std::vector<uint8_t> received;
    int frames = r.feedBytes(data.data(), data.size(),
        [&](const uint8_t* buf, std::size_t len) {
            ++callbacks;
            received.assign(buf, buf + len);
        });
    CHECK(frames == 1);
    CHECK(callbacks == 1);
    REQUIRE(received.size() == 8);
    for (auto b : received) CHECK(b == 0xAB);
    CHECK(r.bufferedBytes() == 0);
}

TEST_CASE("frame buffer pointer is internal — valid only inside callback") {
    PacketReader r;
    auto data = makeFrame(0x55, 4);
    data.push_back(SfxWire::FRAME_DELIMITER);

    const uint8_t* capturedPtr = nullptr;
    std::size_t capturedLen = 0;
    r.feedBytes(data.data(), data.size(),
        [&](const uint8_t* buf, std::size_t len) {
            capturedPtr = buf;
            capturedLen = len;
        });
    REQUIRE(capturedPtr != nullptr);
    REQUIRE(capturedLen == 4);
    // Don't dereference capturedPtr here — buffer may be reused for
    // subsequent frames.  We just confirm the callback DID see the
    // right length while it was running.
}

// ---- Partial frame buffered across calls -----------------------------

TEST_CASE("partial frame across two feedBytes calls emits once") {
    PacketReader r;
    auto data = makeFrame(0x11, 10);
    data.push_back(SfxWire::FRAME_DELIMITER);

    int callbacks = 0;
    // First half: no frame yet, bytes accumulated.
    int f1 = r.feedBytes(data.data(), 5, [&](const uint8_t*, std::size_t) {
        ++callbacks;
    });
    CHECK(f1 == 0);
    CHECK(callbacks == 0);
    CHECK(r.bufferedBytes() == 5);

    // Second half (the rest of the payload + the delimiter).
    int f2 = r.feedBytes(data.data() + 5, data.size() - 5,
        [&](const uint8_t* buf, std::size_t len) {
            ++callbacks;
            CHECK(len == 10);   // both halves recombined
            for (std::size_t i = 0; i < len; ++i) CHECK(buf[i] == 0x11);
        });
    CHECK(f2 == 1);
    CHECK(callbacks == 1);
    CHECK(r.bufferedBytes() == 0);
}

// ---- Multi-frame interleaving ----------------------------------------

TEST_CASE("two complete frames in one feedBytes emit twice") {
    PacketReader r;
    std::vector<uint8_t> stream;
    auto a = makeFrame(0xAA, 6);
    auto b = makeFrame(0xBB, 9);
    stream.insert(stream.end(), a.begin(), a.end());
    stream.push_back(SfxWire::FRAME_DELIMITER);
    stream.insert(stream.end(), b.begin(), b.end());
    stream.push_back(SfxWire::FRAME_DELIMITER);

    std::vector<std::vector<uint8_t>> received;
    int frames = r.feedBytes(stream.data(), stream.size(),
        [&](const uint8_t* buf, std::size_t len) {
            received.emplace_back(buf, buf + len);
        });
    CHECK(frames == 2);
    REQUIRE(received.size() == 2);
    CHECK(received[0].size() == 6);
    CHECK(received[0][0] == 0xAA);
    CHECK(received[1].size() == 9);
    CHECK(received[1][0] == 0xBB);
}

TEST_CASE("delimiter-then-frame-then-delimiter pattern is clean") {
    // A delimiter at the very start (idle wire) before the first frame
    // shouldn't cause an empty callback or any state weirdness.
    PacketReader r;
    std::vector<uint8_t> stream;
    stream.push_back(SfxWire::FRAME_DELIMITER);
    auto payload = makeFrame(0x33, 4);
    stream.insert(stream.end(), payload.begin(), payload.end());
    stream.push_back(SfxWire::FRAME_DELIMITER);

    int callbacks = 0;
    int frames = r.feedBytes(stream.data(), stream.size(),
        [&](const uint8_t* buf, std::size_t len) {
            ++callbacks;
            CHECK(len == 4);
            CHECK(buf[0] == 0x33);
        });
    CHECK(frames == 1);
    CHECK(callbacks == 1);
}

// ---- Buffer overflow -------------------------------------------------

TEST_CASE("buffer overflow drops in-progress frame + bumps framingErrors") {
    TinyReader r;   // 16-byte buffer; easy to overflow

    // Pump 32 non-zero bytes — twice the buffer.
    std::vector<uint8_t> junk(32, 0xFF);
    int callbacks = 0;
    r.feedBytes(junk.data(), junk.size(),
        [&](const uint8_t*, std::size_t) { ++callbacks; });
    CHECK(callbacks == 0);
    CHECK(r.framingErrors() == 1);   // one overflow event
    CHECK(r.bufferedBytes() < TinyReader::bufferSize());
}

TEST_CASE("after overflow, the next valid frame still parses cleanly") {
    TinyReader r;
    std::vector<uint8_t> junk(32, 0xFF);
    r.feedBytes(junk.data(), junk.size(), [](const uint8_t*, std::size_t) {});
    REQUIRE(r.framingErrors() == 1);
    // Caller's responsibility: a delimiter would still emit the residual
    // bytes from the post-overflow re-accumulation phase as a "frame".
    // That's intentional — PacketReader doesn't know that the byte stream
    // around an overflow is garbage.  Real consumers either (a) call
    // reset() after detecting overflow, or (b) accept that the next
    // delimiter emits trash that fails cobsDecode downstream.  Test the
    // explicit-reset path here:
    r.reset();
    REQUIRE(r.bufferedBytes() == 0);

    // Wire idle, then a clean small frame.
    std::vector<uint8_t> resync;
    resync.push_back(SfxWire::FRAME_DELIMITER);   // resync marker
    auto payload = makeFrame(0x99, 8);
    resync.insert(resync.end(), payload.begin(), payload.end());
    resync.push_back(SfxWire::FRAME_DELIMITER);

    int callbacks = 0;
    int frames = r.feedBytes(resync.data(), resync.size(),
        [&](const uint8_t* buf, std::size_t len) {
            ++callbacks;
            CHECK(len == 8);
            CHECK(buf[0] == 0x99);
        });
    CHECK(frames == 1);
    CHECK(callbacks == 1);
    // framingErrors stays at 1 — reset() doesn't decrement it.
    CHECK(r.framingErrors() == 1);
}

TEST_CASE("post-overflow garbage is emitted on next delimiter (caller responsibility)") {
    // Counterpart to the above — DOCUMENT the consequence of skipping
    // reset() so future readers know it's by design.  This isn't a bug
    // to fix; the reader is intentionally below the wire-format layer.
    TinyReader r;
    std::vector<uint8_t> stream(32, 0xFF);
    stream.push_back(SfxWire::FRAME_DELIMITER);  // would-be "frame" terminator

    int callbacks = 0;
    r.feedBytes(stream.data(), stream.size(),
        [&](const uint8_t*, std::size_t len) {
            ++callbacks;
            // The 15 residual junk bytes from the re-accumulation path
            // got handed to us.  cobsDecode would reject this downstream.
            CHECK(len > 0);
        });
    CHECK(r.framingErrors() == 1);  // overflow recorded
    CHECK(callbacks == 1);          // residue emitted as "frame"
}

TEST_CASE("multiple overflow events accumulate") {
    TinyReader r;
    std::vector<uint8_t> burst(32, 0xCC);
    for (int i = 0; i < 4; ++i) {
        r.feedBytes(burst.data(), burst.size(), [](const uint8_t*, std::size_t) {});
    }
    CHECK(r.framingErrors() >= 4);   // exact count depends on the
                                     // accumulation order; at LEAST 4
}

// ---- Single-byte feed API --------------------------------------------

TEST_CASE("feedByte semantics match feedBytes") {
    PacketReader r;
    auto data = makeFrame(0x22, 5);
    data.push_back(SfxWire::FRAME_DELIMITER);

    int callbacks = 0;
    bool gotFrameOnDelim = false;
    for (std::size_t i = 0; i < data.size(); ++i) {
        bool emitted = r.feedByte(data[i],
            [&](const uint8_t* buf, std::size_t len) {
                ++callbacks;
                CHECK(len == 5);
                CHECK(buf[0] == 0x22);
            });
        if (data[i] == SfxWire::FRAME_DELIMITER) {
            gotFrameOnDelim = emitted;
        } else {
            CHECK_FALSE(emitted);
        }
    }
    CHECK(callbacks == 1);
    CHECK(gotFrameOnDelim);
}

// ---- reset / resetStats ----------------------------------------------

TEST_CASE("reset discards in-flight buffer but preserves framingErrors") {
    TinyReader r;
    std::vector<uint8_t> junk(32, 0xDD);
    r.feedBytes(junk.data(), junk.size(), [](const uint8_t*, std::size_t) {});
    REQUIRE(r.framingErrors() >= 1);
    // Clear post-overflow residue so the next feed starts from a known
    // empty state.
    r.reset();
    REQUIRE(r.bufferedBytes() == 0);

    // Feed a half-frame (no delimiter at end → stays buffered).
    auto half = makeFrame(0x44, 5);
    r.feedBytes(half.data(), half.size(), [](const uint8_t*, std::size_t) {});
    REQUIRE(r.bufferedBytes() == 5);

    r.reset();
    CHECK(r.bufferedBytes() == 0);
    // framingErrors NOT zeroed by reset() — only resetStats() does that.
    CHECK(r.framingErrors() >= 1);
}

TEST_CASE("resetStats clears both buffer and error counter") {
    TinyReader r;
    std::vector<uint8_t> junk(32, 0xEE);
    r.feedBytes(junk.data(), junk.size(), [](const uint8_t*, std::size_t) {});
    REQUIRE(r.framingErrors() >= 1);

    r.resetStats();
    CHECK(r.bufferedBytes() == 0);
    CHECK(r.framingErrors() == 0);
}

// ---- Buffer-size template parameter compiles + works -----------------

TEST_CASE("default PacketReader uses SfxWire::COBS_BUFFER_SIZE") {
    CHECK(PacketReader::bufferSize() == SfxWire::COBS_BUFFER_SIZE);
}

TEST_CASE("custom-sized reader frames work end-to-end") {
    PacketReaderT<64> r;
    auto data = makeFrame(0x77, 20);
    data.push_back(SfxWire::FRAME_DELIMITER);

    int frames = r.feedBytes(data.data(), data.size(),
        [](const uint8_t* buf, std::size_t len) {
            CHECK(len == 20);
            CHECK(buf[0] == 0x77);
        });
    CHECK(frames == 1);
}

// ---- Cross-class integration: real wire format round-trips ---------

// Build a real COBS-encoded packet via wire.h, hand it to the reader,
// confirm we get back the SAME byte sequence (the reader's job is to
// hand off the COBS body unchanged — decode happens at the call site).
TEST_CASE("reader yields the exact COBS body wire.h encoded") {
    PacketReader r;

    const uint8_t payload[] = {'H', 'i'};
    uint8_t encoded[SfxWire::COBS_BUFFER_SIZE];
    std::size_t encLen = SfxWire::encodePacket(
        encoded, 0xC0, 0x42, payload, sizeof(payload));
    REQUIRE(encLen > 0);
    // encodePacket appends the frame delimiter as its final byte; the
    // body (encLen - 1 bytes) is the actual COBS-encoded packet that
    // PacketReader should hand back to the callback.

    std::vector<uint8_t> captured;
    int frames = r.feedBytes(encoded, encLen,
        [&](const uint8_t* buf, std::size_t len) {
            captured.assign(buf, buf + len);
        });
    CHECK(frames == 1);
    REQUIRE(captured.size() == encLen - 1);
    for (std::size_t i = 0; i < captured.size(); ++i) {
        CHECK(captured[i] == encoded[i]);
    }
}

// And once the call-site decodes the captured body, parsePacket
// should round-trip back to the original payload — proves
// PacketReader is wire-format-transparent.
TEST_CASE("reader → cobsDecode → parsePacket round-trip") {
    PacketReader r;
    const uint8_t payload[] = {'O', 'K'};
    uint8_t encoded[SfxWire::COBS_BUFFER_SIZE];
    std::size_t encLen = SfxWire::encodePacket(
        encoded, 0xD3, 0x07, payload, sizeof(payload));

    uint8_t gotType = 0, gotTag = 0;
    const uint8_t* gotPayload = nullptr;
    std::size_t gotLen = 0;
    bool parsedOk = false;

    r.feedBytes(encoded, encLen,
        [&](const uint8_t* frame, std::size_t frameLen) {
            uint8_t decoded[SfxWire::MAX_PACKET_SIZE];
            std::size_t decLen = SfxWire::cobsDecode(
                frame, frameLen, decoded, sizeof(decoded));
            parsedOk = SfxWire::parsePacket(
                decoded, decLen, &gotType, &gotTag, &gotPayload, &gotLen);
            if (parsedOk) {
                // copy out before the buffer goes away
                static uint8_t copy[256];
                for (std::size_t i = 0; i < gotLen; ++i) copy[i] = gotPayload[i];
                gotPayload = copy;
            }
        });

    REQUIRE(parsedOk);
    CHECK(gotType == 0xD3);
    CHECK(gotTag  == 0x07);
    REQUIRE(gotLen == sizeof(payload));
    CHECK(gotPayload[0] == 'O');
    CHECK(gotPayload[1] == 'K');
}
