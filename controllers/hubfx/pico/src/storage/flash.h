/*
 * Flash Module - Header
 * 
 * Handles LittleFS flash file system operations with flow control
 * to prevent USB communication deadlocks during file uploads.
 * 
 * KEY DESIGN DECISIONS (based on Pico documentation):
 * 
 * 1. FLOW CONTROL: Uses XON/XOFF protocol to prevent buffer overflow
 *    - Sends XOFF when buffer is 75% full
 *    - Sends XON when buffer drops below 25%
 *    - 512-byte chunks with acknowledgments
 * 
 * 2. NON-BLOCKING WRITES: LittleFS operations can block for 10-100ms
 *    - Use RAM buffer to receive serial data without blocking
 *    - Write to flash in controlled bursts during idle periods
 *    - Progress reporting doesn't interfere with data reception
 * 
 * 3. NO CORE1 NEEDED: Proper flow control eliminates need for second core
 *    - Core1 is reserved for audio processing
 *    - Chunked writes prevent long blocking periods
 *    - USB stack remains responsive
 * 
 * REFERENCES:
 * - Pico SDK: LittleFS operations are synchronous and can block
 * - USB CDC: 64-byte HW buffer, need software buffering
 * - Flash write: ~10ms per 256-byte page
 */

#ifndef FLASH_H
#define FLASH_H

#include <Arduino.h>
#include <LittleFS.h>

// Use LittleFS File type to avoid ambiguity with SdFat File
using LFSFile = ::File;

// Flow control characters
#define XON  0x11   // Resume transmission
#define XOFF 0x13   // Pause transmission

class FlashModule {
public:
    FlashModule();
    
    /**
     * Initialize flash file system
     * @return true if successful, false otherwise
     */
    bool begin();
    
    /**
     * Check if flash is initialized
     */
    bool isInitialized() const { return initialized; }
    
    /**
     * Get reference to LittleFS object
     */
    FS& getFS() { return LittleFS; }
    
    /**
     * List directory contents (identical API to SD card)
     */
    void listDirectory(const String& path, bool jsonOutput = false);
    
    /**
     * Show directory tree from root
     */
    void showTree(bool jsonOutput = false);
    
    /**
     * Show flash information
     */
    void showInfo(bool jsonOutput = false);
    
    /**
     * Display file contents
     */
    void showFile(const String& path);
    
    /**
     * Delete file from flash
     */
    bool deleteFile(const String& path);
    
    /**
     * Upload file via serial with flow control
     * Unified upload method for both storage and config CLIs
     * 
     * @param path File path to write to
     * @param totalSize Expected file size in bytes
     * @param serial Serial stream to read from (typically Serial)
     * @return true if successful, false otherwise
     */
    bool uploadFile(const String& path, uint32_t totalSize, Stream& serial);
    
    /**
     * Write data to file with flow control
     * Uses RAM buffer and chunked writes to prevent USB blocking
     * 
     * @param path File path
     * @param data Data to write
     * @param length Data length
     * @param append True to append, false to overwrite
     * @return true if successful, false if buffer full (wait for flush)
     */
    bool writeFile(const String& path, const uint8_t* data, size_t length, bool append = true);
    
    /**
     * Flush buffered data to flash
     * Call periodically during upload to prevent RAM overflow
     * Returns true when all data written, false if more data pending
     */
    bool flushBuffer();
    
    /**
     * Close and finalize file write
     * Flushes all remaining data
     */
    bool closeFile();
    
    /**
     * Get buffer status for flow control
     * Returns percentage full (0-100)
     */
    int getBufferFillPercent() const;
    
    /**
     * Get size of file
     */
    int getFileSize(const String& path);
    
private:
    bool initialized;
    
    // Buffered write state
    LFSFile writeFileHandle;
    bool writeFileOpen;
    
    // RAM buffer for upload (16KB - balance between RAM usage and chunk size)
    static const size_t BUFFER_SIZE = 16384;
    uint8_t* ramBuffer;
    size_t ramBufferUsed;
    size_t ramBufferFlushed;  // How much of buffer has been written to flash
    
    // Flow control thresholds
    static const int XOFF_THRESHOLD = 75;  // Pause at 75% full
    static const int XON_THRESHOLD = 25;   // Resume at 25% full
    bool flowControlPaused;
    
    // Helper functions
    void listDirRecursive(const char* path, int level, bool jsonOutput = false);
    void sendFlowControl(bool pause);
};

#endif // FLASH_H
