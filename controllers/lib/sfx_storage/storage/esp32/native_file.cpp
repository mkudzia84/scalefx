/*
 * NativeFile — implementation.
 *
 * Path semantics
 * ──────────────
 *   User paths ("/sounds/foo.wav") are joined with the mount base
 *   ("/sdcard" or "/littlefs") to form the VFS path ("/sdcard/sounds/
 *   foo.wav") that POSIX fopen() / opendir() actually see.  We store
 *   both the basePath AND the full path so directory iteration can
 *   build child paths without re-deriving them.
 *
 * Size discovery
 * ──────────────
 *   `_size` is captured at open time via fseek/ftell/fseek-back.  For
 *   write-mode opens we leave it at 0 (it changes as we write); the
 *   storage server queries `size()` only on read-mode handles.
 *
 * Directory iteration
 * ───────────────────
 *   readdir() returns one `struct dirent` per call.  The entry's `d_type`
 *   is `DT_DIR` for sub-directories and `DT_REG` for regular files on the
 *   FATFS / esp_littlefs VFS drivers.  We stat each child to get its
 *   size and to surface IS_DIRECTORY accurately.  Symlinks / DT_UNKNOWN
 *   are treated as regular files (FAT doesn't have them; LittleFS only
 *   returns reg/dir).
 *
 * Close semantics
 * ───────────────
 *   close() is idempotent.  Calling read/write/seek on a closed file
 *   returns the natural failure indicator (-1 or 0).  The destructor
 *   calls close() to guarantee the FILE* / DIR* is released even when
 *   the handle is destroyed by exception unwinding (unused here) or
 *   move-out followed by drop.
 */

#include "native_file.h"

#if SFX_PLATFORM_ESP32

#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

// ──────────────────────────────────────────────────────────────────────
//  Internal helpers
// ──────────────────────────────────────────────────────────────────────

namespace {

/// Join basePath + path into out.  basePath is "/sdcard" or "/littlefs"
/// (no trailing slash); path is the user-visible path (may or may not
/// start with /).  Result fits in `outSize` or is truncated; caller
/// guarantees outSize ≥ 192 in practice.
void joinPath(char* out, size_t outSize,
              const char* basePath, const char* path) {
    if (!path) path = "";
    const bool needsSlash = (path[0] != '/');
    snprintf(out, outSize, "%s%s%s", basePath, needsSlash ? "/" : "", path);
}

/// Strip trailing slash (but keep "/" itself).  Lets callers pass
/// either "/sounds" or "/sounds/" to openDir without ambiguity.
void rstripSlash(char* p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') {
        p[--n] = '\0';
    }
}

/// Extract basename into dst (size 64).
void basenameInto(const char* full, char* dst, size_t dstSize) {
    const char* slash = strrchr(full, '/');
    const char* base  = slash ? slash + 1 : full;
    strncpy(dst, base, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

}  // namespace


// ──────────────────────────────────────────────────────────────────────
//  Move / dtor
// ──────────────────────────────────────────────────────────────────────

NativeFile::NativeFile(NativeFile&& other) noexcept
    : _fp(other._fp), _dp(other._dp),
      _isDir(other._isDir), _size(other._size)
{
    memcpy(_name,     other._name,     sizeof(_name));
    memcpy(_basePath, other._basePath, sizeof(_basePath));
    memcpy(_fullPath, other._fullPath, sizeof(_fullPath));
    other._fp    = nullptr;
    other._dp    = nullptr;
    other._isDir = false;
    other._size  = 0;
    other._name[0]     = '\0';
    other._basePath[0] = '\0';
    other._fullPath[0] = '\0';
}

NativeFile& NativeFile::operator=(NativeFile&& other) noexcept {
    if (this != &other) {
        close();
        _fp    = other._fp;
        _dp    = other._dp;
        _isDir = other._isDir;
        _size  = other._size;
        memcpy(_name,     other._name,     sizeof(_name));
        memcpy(_basePath, other._basePath, sizeof(_basePath));
        memcpy(_fullPath, other._fullPath, sizeof(_fullPath));
        other._fp    = nullptr;
        other._dp    = nullptr;
        other._isDir = false;
        other._size  = 0;
        other._name[0]     = '\0';
        other._basePath[0] = '\0';
        other._fullPath[0] = '\0';
    }
    return *this;
}

NativeFile::~NativeFile() { close(); }

void NativeFile::close() {
    if (_fp) { fclose(_fp); _fp = nullptr; }
    if (_dp) { closedir(_dp); _dp = nullptr; }
    _isDir = false;
    _size  = 0;
}

void NativeFile::adoptFile(FILE* fp, size_t sz, const char* fullPath,
                           const char* basePath) {
    _fp     = fp;
    _dp     = nullptr;
    _isDir  = false;
    _size   = sz;
    strncpy(_fullPath, fullPath, sizeof(_fullPath) - 1);
    _fullPath[sizeof(_fullPath) - 1] = '\0';
    strncpy(_basePath, basePath, sizeof(_basePath) - 1);
    _basePath[sizeof(_basePath) - 1] = '\0';
    basenameInto(_fullPath, _name, sizeof(_name));
}

void NativeFile::adoptDir(DIR* dp, const char* fullPath, const char* basePath) {
    _fp     = nullptr;
    _dp     = dp;
    _isDir  = true;
    _size   = 0;
    strncpy(_fullPath, fullPath, sizeof(_fullPath) - 1);
    _fullPath[sizeof(_fullPath) - 1] = '\0';
    strncpy(_basePath, basePath, sizeof(_basePath) - 1);
    _basePath[sizeof(_basePath) - 1] = '\0';
    basenameInto(_fullPath, _name, sizeof(_name));
}


// ──────────────────────────────────────────────────────────────────────
//  Factories
// ──────────────────────────────────────────────────────────────────────

NativeFile NativeFile::openReadFile(const char* basePath, const char* path) {
    NativeFile f;
    char full[192];
    joinPath(full, sizeof(full), basePath, path);

    // Probe with stat() first so we can distinguish "is a directory"
    // from "not found" without opening a regular file that we'll then
    // throw away.  This mirrors how the legacy openRead path detected
    // IS_DIRECTORY (open + isDirectory() + close + return IS_DIRECTORY).
    struct stat st;
    if (stat(full, &st) != 0) return f;     // not found / not accessible

    if (S_ISDIR(st.st_mode)) {
        // Match fs::File semantics: openReadFile on a directory returns
        // a VALID handle that reports isDirectory()==true.  The caller
        // (SdCardModuleT::openRead) checks isDirectory() before any
        // read ops, then closes + returns IS_DIRECTORY.  Open the DIR*
        // so operator bool() is true.
        DIR* dp = opendir(full);
        if (!dp) return f;          // dir exists per stat but can't open → NOT_FOUND
        f.adoptDir(dp, full, basePath);
        return f;
    }

    FILE* fp = fopen(full, "rb");
    if (!fp) return f;
    f.adoptFile(fp, (size_t)st.st_size, full, basePath);
    return f;
}

NativeFile NativeFile::openWriteFile(const char* basePath, const char* path,
                                     bool truncate) {
    NativeFile f;
    char full[192];
    joinPath(full, sizeof(full), basePath, path);

    // "wb+" creates-or-truncates AND allows seek+read+write, which is
    // what the upload pre-allocation pattern (seek(end); write(1);
    // seek(0)) requires.  Plain "wb" disallows seeking for read which
    // breaks the storage server.
    // For non-truncate: "rb+" preserves content + allows seek, falling
    // back to "wb+" if the file doesn't exist yet (matches the legacy
    // Arduino "a"-mode semantics of "create-if-missing, preserve-else").
    FILE* fp = nullptr;
    if (truncate) {
        fp = fopen(full, "wb+");
    } else {
        fp = fopen(full, "rb+");
        if (!fp) fp = fopen(full, "wb+");
    }
    if (!fp) return f;

    // For writes _size starts at the current file length; subsequent
    // writes don't auto-update it (storage server doesn't query size on
    // a write handle in our codebase).  Stat the file post-open.
    struct stat st;
    size_t sz = 0;
    if (stat(full, &st) == 0) sz = (size_t)st.st_size;

    f.adoptFile(fp, sz, full, basePath);
    return f;
}

NativeFile NativeFile::openDir(const char* basePath, const char* path) {
    NativeFile f;
    char full[192];
    joinPath(full, sizeof(full), basePath, path);
    rstripSlash(full);

    DIR* dp = opendir(full);
    if (!dp) return f;
    f.adoptDir(dp, full, basePath);
    return f;
}


// ──────────────────────────────────────────────────────────────────────
//  Directory iteration
// ──────────────────────────────────────────────────────────────────────

NativeFile NativeFile::openNextFile() {
    NativeFile result;
    if (!_dp) return result;

    // readdir() returns NULL at end-of-stream (or on error).  Skip the
    // "." and ".." entries — VFS-FAT exposes them; esp_littlefs does
    // not, but defensive-skip costs nothing.
    while (true) {
        struct dirent* de = readdir(_dp);
        if (!de) return result;  // end-of-dir → invalid file

        const char* name = de->d_name;
        if (name[0] == '.' && (name[1] == '\0' ||
                              (name[1] == '.' && name[2] == '\0'))) {
            continue;  // skip "." and ".."
        }

        // Build child path: _fullPath + '/' + name
        char childFull[192];
        const size_t parentLen = strlen(_fullPath);
        const bool   needSlash = (parentLen > 0 &&
                                  _fullPath[parentLen - 1] != '/');
        snprintf(childFull, sizeof(childFull), "%s%s%s",
                 _fullPath, needSlash ? "/" : "", name);

        // Stat to determine type + size.  d_type may not be reliable
        // across FATFS / esp_littlefs; stat is the safe call.
        struct stat st;
        if (stat(childFull, &st) != 0) {
            // Entry vanished between readdir + stat (concurrent
            // delete?) — skip and continue.
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            DIR* sub = opendir(childFull);
            if (sub) result.adoptDir(sub, childFull, _basePath);
            else {
                // Couldn't open sub-dir — return an "exists but unopenable"
                // entry with isDir true so the caller can still see the
                // name + skip it.  This mirrors fs::File semantics where
                // the listing tolerates partial failures.
                result._isDir = true;
                result._size  = 0;
                strncpy(result._fullPath, childFull, sizeof(result._fullPath) - 1);
                result._fullPath[sizeof(result._fullPath) - 1] = '\0';
                strncpy(result._basePath, _basePath, sizeof(result._basePath) - 1);
                result._basePath[sizeof(result._basePath) - 1] = '\0';
                basenameInto(result._fullPath, result._name, sizeof(result._name));
            }
        } else {
            // Regular file — open in read mode so size() + the boolean
            // "valid" check work.  The caller of openNextFile() reads
            // size + name + isDirectory and then closes immediately;
            // it doesn't read content from the iterator entry.
            FILE* fp = fopen(childFull, "rb");
            if (fp) {
                result.adoptFile(fp, (size_t)st.st_size, childFull, _basePath);
            } else {
                // Same partial-failure tolerance as the dir branch above.
                result._isDir = false;
                result._size  = (size_t)st.st_size;
                strncpy(result._fullPath, childFull, sizeof(result._fullPath) - 1);
                result._fullPath[sizeof(result._fullPath) - 1] = '\0';
                strncpy(result._basePath, _basePath, sizeof(result._basePath) - 1);
                result._basePath[sizeof(result._basePath) - 1] = '\0';
                basenameInto(result._fullPath, result._name, sizeof(result._name));
            }
        }
        return result;
    }
}

#endif  // SFX_PLATFORM_ESP32
