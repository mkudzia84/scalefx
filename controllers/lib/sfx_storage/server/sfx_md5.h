/*
 * sfx_md5.h — tiny MD5 helper with the subset of the old Arduino MD5Builder
 * API the upload path uses (begin / add / calculate / getBytes / toString).
 *
 * ESP32 (pure ESP-IDF): the ROM MD5 (`esp_rom_md5_*`) — no extra component,
 * no Arduino.  Pico: Arduino-Pico's MD5Builder (gated; Pico still rides
 * Arduino).  Drop-in for the storage server's `_uploadMd5`.
 */

#ifndef SFX_MD5_H
#define SFX_MD5_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32

#include <esp_rom_md5.h>

namespace sfx_storage {

class SfxMd5 {
public:
    void begin() { esp_rom_md5_init(&_ctx); _done = false; }
    void add(const uint8_t* data, size_t len) {
        esp_rom_md5_update(&_ctx, data, (uint32_t)len);
    }
    void calculate() { esp_rom_md5_final(_digest, &_ctx); _done = true; }
    void getBytes(uint8_t out[16]) const {
        for (int i = 0; i < 16; ++i) out[i] = _digest[i];
    }
    std::string toString() const {
        static const char* hex = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (int i = 0; i < 16; ++i) {
            s.push_back(hex[_digest[i] >> 4]);
            s.push_back(hex[_digest[i] & 0x0F]);
        }
        return s;
    }

private:
    md5_context_t _ctx{};
    uint8_t       _digest[16] = {};
    bool          _done = false;
};

}  // namespace sfx_storage

#else  // SFX_PLATFORM_PICO — Arduino-Pico still provides MD5Builder

#include <MD5Builder.h>

namespace sfx_storage {

class SfxMd5 {
public:
    void begin() { _b.begin(); }
    void add(const uint8_t* data, size_t len) {
        _b.add(const_cast<uint8_t*>(data), len);
    }
    void calculate() { _b.calculate(); }
    void getBytes(uint8_t out[16]) { _b.getBytes(out); }
    std::string toString() { return std::string(_b.toString().c_str()); }

private:
    MD5Builder _b;
};

}  // namespace sfx_storage

#endif
#endif  // SFX_MD5_H
