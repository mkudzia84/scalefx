/*
 * SD Card Module - Header
 * 
 * Singleton class for SD card initialization, file operations, and debug commands.
 * Provides thread-safe SPI access via mutex for multi-core operation.
 * 
 * Usage:
 *   SdCardModule& sd = SdCardModule::instance();
 *   sd.begin(cs, sck, mosi, miso);
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <pico/mutex.h>

class SdCardModule {
public:
    /**
     * Get the singleton instance
     */
    static SdCardModule& instance() {
        static SdCardModule instance;
        return instance;
    }
    
    // Delete copy/move constructors and assignment operators
    SdCardModule(const SdCardModule&) = delete;
    SdCardModule& operator=(const SdCardModule&) = delete;
    SdCardModule(SdCardModule&&) = delete;
    SdCardModule& operator=(SdCardModule&&) = delete;
    
    /**
     * Initialize SD card with given pins
     * 
     * @param cs_pin Chip select pin
     * @param sck_pin SPI clock pin
     * @param mosi_pin SPI MOSI pin
     * @param miso_pin SPI MISO pin
     * @param speed_mhz SPI speed in MHz
     * @return true if successful, false otherwise
     */
    bool begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin, uint8_t miso_pin, uint8_t speed_mhz = 25);
    
    /**
     * Check if SD card is initialized
     */
    bool isInitialized() const { return initialized; }
    
    /**
     * Get reference to SdFat object (for direct SD access)
     */
    SdFat& getSd() { return sd; }
    
    /**
     * Lock SD card for exclusive access (blocking)
     */
    void lock() { mutex_enter_blocking(&_sdMutex); }
    
    /**
     * Try to lock SD card for exclusive access (non-blocking)
     * @return true if lock acquired, false if already locked
     */
    bool tryLock() { return mutex_try_enter(&_sdMutex, nullptr); }
    
    /**
     * Unlock SD card after access
     */
    void unlock() { mutex_exit(&_sdMutex); }
    
    /**
     * Retry initialization with different speed
     */
    bool retryInit(uint8_t speed_mhz);
    
    /**
     * List directory contents
     */
    void listDirectory(const String& path, bool jsonOutput = false);
    
    /**
     * Show directory tree from root
     */
    void showTree(bool jsonOutput = false);
    
    /**
     * Show SD card information
     */
    void showInfo(bool jsonOutput = false);
    
    /**
     * Display file contents
     */
    void showFile(const String& path);
    
    /**
     * Upload file via serial with progress reporting
     * Unified upload method for config and storage operations
     * 
     * @param path File path to write to
     * @param totalSize Expected file size in bytes
     * @param serial Serial stream to read from (typically Serial)
     * @return true if successful, false otherwise
     */
    bool uploadFile(const String& path, uint32_t totalSize, Stream& serial);
    
    /**
     * Download file via serial with progress reporting
     * Sends file as binary data with progress updates
     * 
     * @param path File path to read from
     * @param serial Serial stream to write to (typically Serial)
     * @return true if successful, false otherwise
     */
    bool downloadFile(const String& path, Stream& serial);
    
    /**
     * Remove file from SD card
     * 
     * @param path File path to remove
     * @return true if successful, false otherwise
     */
    bool removeFile(const String& path);
    
    /**
     * Create directory on SD card (recursive)
     * 
     * @param path Directory path to create
     * @return true if successful or already exists, false otherwise
     */
    bool makeDirectory(const String& path);
    
private:
    // Private constructor for singleton
    SdCardModule();
    
    SdFat sd;
    bool initialized;
    mutex_t _sdMutex;
    
    // Store pin configuration for retry
    uint8_t _cs_pin;
    uint8_t _sck_pin;
    uint8_t _mosi_pin;
    uint8_t _miso_pin;
    
    // Helper functions
    void listDirRecursive(const char* path, int level, bool jsonOutput = false);
};

#endif // SD_CARD_H
