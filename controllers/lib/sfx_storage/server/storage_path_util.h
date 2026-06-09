/*
 * storage_path_util.h — stateless storage helpers (no template, no state).
 *
 * These four functions touch no member state — they parse wire payloads, map
 * driver error codes, validate paths, and name targets.  Extracted from
 * StorageServicePolicy so the file-ops half and the UploadEngine half share
 * ONE copy (and so they're unit-testable without instantiating the policy).
 *
 * Bodies moved verbatim from the former StorageServicePolicy methods.
 */

#ifndef SFX_STORAGE_PATH_UTIL_H
#define SFX_STORAGE_PATH_UTIL_H

#include <cstdint>
#include <cstddef>
#include <cstring>

#include <serial/core/core.h>                  // SerialError
#include <serial/storage/storage_protocol.h>   // StorageWire / StorageError
#include <storage/flash.h>                      // FlashError (== SdError codes)

namespace sfx_storage {

/// Human-readable target name for logs.
inline const char* targetName(StorageWire::StorageTarget target) {
    return (target == StorageWire::TARGET_FLASH) ? "flash" : "sd";
}

/// Map a FlashError / SdError driver code (they share values 0-6) onto the
/// wire SerialError / StorageError space.
inline uint8_t mapStorageError(uint8_t err) {
    switch (err) {
        case FlashError::OK:              return SerialError::OK;
        case FlashError::NOT_INITIALIZED: return SerialError::NOT_INITIALIZED;
        case FlashError::NOT_FOUND:       return StorageError::FILE_NOT_FOUND;
        case FlashError::IO_ERROR:        return StorageError::FILE_IO_ERROR;
        case FlashError::IS_DIRECTORY:    return StorageError::FILE_IO_ERROR;
        case FlashError::ALREADY_EXISTS:  return StorageError::FILE_ALREADY_EXISTS;
        case FlashError::LIMIT_EXCEEDED:  return SerialError::UNKNOWN;
        default:                          return SerialError::UNKNOWN;
    }
}

/// Validate a path: must be absolute and contain no `..` traversal component.
inline bool isValidPath(const char* path) {
    if (!path || path[0] != '/') return false;

    // Reject path traversal: ".." as a path component
    const char* p = path;
    while ((p = strstr(p, "..")) != nullptr) {
        bool preceded = (p == path || *(p - 1) == '/');
        bool followed = (*(p + 2) == '\0' || *(p + 2) == '/');
        if (preceded && followed) return false;
        p += 2;
    }

    return true;
}

/// Parse the common `[pathLen:u8][path][target:u8?][flags:u8?]` payload shape.
/// Returns SerialError::OK and fills path/target(/flags) on success, else a
/// wire error code.  `target` defaults to TARGET_SD; `flagsOut` is optional.
inline uint8_t extractPathAndTarget(
        const uint8_t* payload, size_t len,
        char* path, size_t pathBufSize, StorageWire::StorageTarget& target,
        uint8_t* flagsOut = nullptr) {

    if (flagsOut) *flagsOut = 0;

    if (len < 1) return SerialError::MISSING_PARAMETER;

    uint8_t pathLen = payload[0];
    if (pathLen == 0 || (size_t)(1 + pathLen) > len) return SerialError::MISSING_PARAMETER;
    if (pathLen >= pathBufSize) return SerialError::PARAM_TOO_LONG;

    // Reject embedded null bytes in path data
    if (memchr(&payload[1], 0, pathLen) != nullptr) return SerialError::INVALID_PARAM;

    memcpy(path, &payload[1], pathLen);
    path[pathLen] = '\0';

    // Validate path format (no traversal, must start with '/')
    if (!isValidPath(path)) return SerialError::INVALID_PARAM;

    // Optional target byte after the path
    if (len > (size_t)(1 + pathLen)) {
        uint8_t rawTarget = payload[1 + pathLen];
        if (rawTarget > StorageWire::TARGET_FLASH) return SerialError::INVALID_PARAM;
        target = static_cast<StorageWire::StorageTarget>(rawTarget);
    } else {
        target = StorageWire::TARGET_SD;  // default
    }

    // Optional flags byte after the target (append-only extension — Rule 11)
    if (flagsOut && len > (size_t)(2 + pathLen)) {
        *flagsOut = payload[2 + pathLen];
    }

    return SerialError::OK;
}

}  // namespace sfx_storage

#endif  // SFX_STORAGE_PATH_UTIL_H
