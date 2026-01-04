/*
 * SD Card Module - Header
 * 
 * Handles SD card initialization, file operations, and debug commands
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

class SdCardModule {
public:
    SdCardModule();
    
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
     * Get reference to SdFat object (for audio mixer access)
     */
    SdFat& getSd() { return sd; }
    
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
     * Write data to file (receive via serial in chunks)
     * Returns true if ready for next chunk, false on error
     */
    bool writeFile(const String& path, const uint8_t* data, size_t length, bool append = true);
    
    /**
     * Close and finalize file write
     */
    bool closeFile();
    
private:
    SdFat sd;
    bool initialized;
    File32 writeFileHandle;
    
    // Store pin configuration for retry
    uint8_t _cs_pin;
    uint8_t _sck_pin;
    uint8_t _mosi_pin;
    uint8_t _miso_pin;
    
    // Helper functions
    void listDirRecursive(const char* path, int level, bool jsonOutput = false);
};

#endif // SD_CARD_H
