/*
 * Storage ↔ ConfigStore Bridge — Templatized File I/O Functions
 *
 * Provides ConfigStore-compatible FileReadFunc / FileWriteFunc
 * implementations for any storage module that follows the shared
 * API contract (FlashModule, SdCardModuleT<T>).
 *
 * Required storage module API:
 *   static TModule& instance();
 *   bool isInitialized() const;
 *   void lock();
 *   void unlock();
 *   uint8_t openRead(const char* path, FileHandle& file);
 *   uint8_t openWrite(const char* path, FileHandle& file, bool truncate);
 *
 * Usage:
 *   #include <storage/storage_config_bridge.h>
 *   #include <storage/flash.h>
 *
 *   // Option A: Wire both reader + writer in one call
 *   wireConfigStore<FlashModule>(configStore);
 *
 *   // Option B: Set individually (matches ConfigStore::setFileReader/Writer)
 *   configStore.setFileReader(storageReadFile<FlashModule>);
 *   configStore.setFileWriter(storageWriteFile<FlashModule>);
 *
 *   // Works with SD too:
 *   wireConfigStore<SdCardModule>(configStore);
 */

#ifndef STORAGE_CONFIG_BRIDGE_H
#define STORAGE_CONFIG_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Read a file from a storage module into a buffer.
 *
 * ConfigStore-compatible: signature matches FileReadFunc.
 * Thread-safe: acquires and releases the module's mutex.
 *
 * @tparam TModule Storage module type (FlashModule, SdCardModule, etc.)
 * @return Bytes read, or -1 on error
 */
template <typename TModule>
int storageReadFile(const char* path, char* buffer, size_t maxLen) {
    auto& mod = TModule::instance();
    if (!mod.isInitialized()) return -1;

    mod.lock();

    typename TModule::FileHandle file;
    uint8_t err = mod.openRead(path, file);
    if (err != 0) {
        mod.unlock();
        return -1;
    }

    int bytesRead = file.read((uint8_t*)buffer, maxLen);
    file.close();
    mod.unlock();
    return bytesRead;
}

/**
 * @brief Write a buffer to a file on a storage module.
 *
 * ConfigStore-compatible: signature matches FileWriteFunc.
 * Thread-safe: acquires and releases the module's mutex.
 *
 * @tparam TModule Storage module type (FlashModule, SdCardModule, etc.)
 * @return Bytes written, or -1 on error
 */
template <typename TModule>
int storageWriteFile(const char* path, const char* data, size_t len) {
    auto& mod = TModule::instance();
    if (!mod.isInitialized()) return -1;

    mod.lock();

    typename TModule::FileHandle file;
    uint8_t err = mod.openWrite(path, file, true);
    if (err != 0) {
        mod.unlock();
        return -1;
    }

    int written = file.write((const uint8_t*)data, len);
    file.close();
    mod.unlock();
    return written;
}

/**
 * @brief Wire a ConfigStore's file reader + writer to a storage module.
 *
 * Sets both callbacks in one call. The ConfigStore will use the
 * storage module's singleton for all file I/O.
 *
 * @tparam TModule Storage module type (FlashModule, SdCardModule, etc.)
 * @param store    ConfigStore (or ConfigServerT's store()) to wire
 */
template <typename TModule, typename TStore>
void wireConfigStore(TStore& store) {
    store.setFileReader(storageReadFile<TModule>);
    store.setFileWriter(storageWriteFile<TModule>);
}

#endif // STORAGE_CONFIG_BRIDGE_H
