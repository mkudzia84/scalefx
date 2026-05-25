/*
 * NativeFile — POSIX RAII file/dir handle for ESP-IDF VFS backends.
 *
 * Single move-only wrapper that mirrors the `fs::File` surface the
 * storage server + audio mixer + config store touch:
 *   - operator bool(), isDirectory(), size(), position(), available()
 *   - read(buf, n), write(buf, n), seek(pos), flush(), close()
 *   - name(), openNextFile()
 *
 * Backed by `<stdio.h>` FILE* for files and `<dirent.h>` DIR* for
 * directories.  Path resolution is delegated to ESP-IDF VFS via the
 * mount base path the policy installs:
 *   - SD card  → "/sdcard/…"
 *   - Flash    → "/littlefs/…"
 *
 * Thread safety
 * ─────────────
 *   POSIX calls under VFS-FAT (with `FF_FS_REENTRANT=1`, the IDF
 *   default) and `esp_littlefs` are natively task-safe and core-safe —
 *   the VFS layer holds a per-mount mutex transparently.  The
 *   `SdCardModule` / `FlashModule` per-instance mutex remains in place
 *   to gate mount/unmount and the upload-exclusivity contract (Rule
 *   28), not for per-call locking.
 *
 * Lifetime
 * ────────
 *   Move-only.  Destructor closes both `_fp` and `_dp` if set.  Default
 *   constructor is "invalid" (operator bool returns false).
 *
 * Build gate: ESP32 only — Pico paths use SdFat / Arduino LittleFS.
 */

#ifndef SFX_STORAGE_NATIVE_FILE_H
#define SFX_STORAGE_NATIVE_FILE_H

#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <dirent.h>

class NativeFile {
public:
    NativeFile() = default;

    NativeFile(NativeFile&& other) noexcept;
    NativeFile& operator=(NativeFile&& other) noexcept;
    NativeFile(const NativeFile&)            = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    ~NativeFile();

    // ── Factory helpers used by the policies ─────────────────────────
    //
    // `basePath` is the VFS mount root ("/sdcard" or "/littlefs"); `path`
    // is the user-visible path (e.g. "/sounds/foo.wav").  The factory
    // joins them and (depending on mode) opens with fopen() or opendir().

    /// Open `basePath + path` for reading.  Returns invalid file on
    /// failure (caller checks `bool(file)`).  If the path turns out to
    /// be a directory, the returned handle's `isDirectory()` is true
    /// AND its `_dp` is null — caller closes and returns IS_DIRECTORY.
    static NativeFile openReadFile(const char* basePath, const char* path);

    /// Open `basePath + path` for writing.  `truncate=true` opens with
    /// "wb" (truncates / creates); `false` opens with "rb+" (preserves
    /// content; caller seeks).  Creates the file if it doesn't exist.
    static NativeFile openWriteFile(const char* basePath, const char* path,
                                    bool truncate);

    /// Open `basePath + path` as a directory iterator.  Returns invalid
    /// file if the path doesn't exist or isn't a directory.
    static NativeFile openDir(const char* basePath, const char* path);


    // ── Surface that mirrors fs::File (only the methods upper layers call) ──

    explicit operator bool() const { return _fp != nullptr || _dp != nullptr; }

    bool       isDirectory() const { return _isDir; }
    size_t     size() const        { return _size; }
    size_t     position() const;
    bool       available() const;
    bool       seek(size_t pos);

    int        read(uint8_t* buf, size_t n);
    int        write(const uint8_t* buf, size_t n);

    void       flush();
    void       close();

    const char* name() const { return _name; }

    /// Directory iteration — returns invalid file at end of dir.  Caller
    /// is responsible for closing each returned entry.  Subdirectories
    /// are returned as directory-mode entries (`isDirectory()==true`)
    /// with their `_dp` open and ready for nested iteration.
    NativeFile openNextFile();

private:
    /// Take ownership of an already-opened FILE*.  Used by factory.
    void adoptFile(FILE* fp, size_t sz, const char* fullPath,
                   const char* basePath);

    /// Take ownership of an already-opened DIR*.  Used by factory.
    void adoptDir(DIR* dp, const char* fullPath, const char* basePath);

    FILE*  _fp     = nullptr;
    DIR*   _dp     = nullptr;
    bool   _isDir  = false;
    size_t _size   = 0;

    /// Basename for `name()` queries (mirrors Arduino's behavior
    /// where the storage server's policy `extractName()` extracts the
    /// trailing path component).  Capped to 63 chars + NUL.
    char   _name[64]    = {};

    /// Mount prefix ("/sdcard" or "/littlefs") + relative path.  Stored
    /// so `openNextFile()` can construct child paths for nested dirs.
    char   _basePath[16] = {};      // "/sdcard" / "/littlefs"
    char   _fullPath[192] = {};     // full VFS path (basePath + relative)
};


// ────────────────────────────────────────────────────────────────────
//  Inline definitions — small enough to live in the header
// ────────────────────────────────────────────────────────────────────

inline size_t NativeFile::position() const {
    if (!_fp) return 0;
    const long p = ftell(_fp);
    return (p < 0) ? 0 : (size_t)p;
}

inline bool NativeFile::available() const {
    if (!_fp) return false;
    return position() < _size;
}

inline bool NativeFile::seek(size_t pos) {
    if (!_fp) return false;
    return fseek(_fp, (long)pos, SEEK_SET) == 0;
}

inline int NativeFile::read(uint8_t* buf, size_t n) {
    if (!_fp || !buf) return -1;
    const size_t got = fread(buf, 1, n, _fp);
    return (int)got;
}

inline int NativeFile::write(const uint8_t* buf, size_t n) {
    if (!_fp || !buf) return -1;
    const size_t put = fwrite(buf, 1, n, _fp);
    return (int)put;
}

inline void NativeFile::flush() {
    if (_fp) fflush(_fp);
}

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_STORAGE_NATIVE_FILE_H
