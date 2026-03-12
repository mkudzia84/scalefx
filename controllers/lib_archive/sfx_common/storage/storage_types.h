/*
 * Storage Types — Shared Data Structures for Storage Modules
 *
 * Common types used by both SdCardModule and FlashModule.
 * Keeps FileEntry and other shared structures in one place so
 * StorageServer can work with either storage backend uniformly.
 */

#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <stdint.h>


/**
 * @brief File/directory entry information
 *
 * Common struct used by both SdCardModule and FlashModule
 * for directory listing callbacks and file info queries.
 */
struct FileEntry {
    char name[64];
    bool isDirectory;
    uint32_t size;   ///< File size in bytes (0 for directories)
};

#endif // STORAGE_TYPES_H
