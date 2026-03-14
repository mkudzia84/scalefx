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


// ============================================================================
// Safety Limits for Recursive Directory Operations
// ============================================================================

/// Maximum recursion depth for tree listing and recursive delete.
/// Prevents infinite recursion on corrupted FAT circular references.
constexpr int MAX_TREE_DEPTH   = 32;

/// Maximum entries returned per listing (tree or flat directory).
/// Prevents infinite loops on corrupted directory chains.
constexpr int MAX_TREE_ENTRIES = 10000;


#endif // STORAGE_TYPES_H
