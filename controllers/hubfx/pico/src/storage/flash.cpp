/*
 * Flash Module - Implementation
 * 
 * LittleFS flash file system with flow-controlled serial uploads
 */

#include "flash.h"

FlashModule::FlashModule() 
    : initialized(false)
    , writeFileOpen(false)
    , ramBuffer(nullptr)
    , ramBufferUsed(0)
    , ramBufferFlushed(0)
    , flowControlPaused(false) {
}

bool FlashModule::begin() {
    Serial.println("[FLASH] Initializing LittleFS...");
    
    if (!LittleFS.begin()) {
        Serial.println("[FLASH] ERROR: Failed to initialize LittleFS");
        return false;
    }
    
    // Get flash info
    FSInfo fsinfo;
    LittleFS.info(fsinfo);
    
    Serial.printf("[FLASH] ✓ LittleFS initialized\n");
    Serial.printf("[FLASH] Total size: %lu bytes (%.2f KB)\n", 
                 fsinfo.totalBytes, fsinfo.totalBytes / 1024.0);
    Serial.printf("[FLASH] Used size: %lu bytes (%.2f KB)\n", 
                 fsinfo.usedBytes, fsinfo.usedBytes / 1024.0);
    Serial.printf("[FLASH] Free size: %lu bytes (%.2f KB)\n", 
                 fsinfo.totalBytes - fsinfo.usedBytes, 
                 (fsinfo.totalBytes - fsinfo.usedBytes) / 1024.0);
    
    initialized = true;
    return true;
}

void FlashModule::listDirectory(const String& path, bool jsonOutput) {
    Dir dir = LittleFS.openDir(path);
    
    if (jsonOutput) {
        Serial.printf("{\"path\":\"%s\",\"items\":[", path.c_str());
    } else {
        Serial.printf("=== %s ===\n", path.c_str());
    }
    
    int count = 0;
    while (dir.next()) {
        if (jsonOutput) {
            if (count > 0) Serial.print(",");
            Serial.printf("{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu}",
                         dir.fileName().c_str(),
                         dir.isDirectory() ? "dir" : "file",
                         (unsigned long)dir.fileSize());
        } else {
            if (dir.isDirectory()) {
                Serial.printf("  [DIR]  %s/\n", dir.fileName().c_str());
            } else {
                Serial.printf("  [FILE] %s (%lu bytes)\n", 
                            dir.fileName().c_str(), 
                            (unsigned long)dir.fileSize());
            }
        }
        count++;
    }
    
    if (jsonOutput) {
        Serial.printf("],\"total\":%d}\n", count);
    } else {
        Serial.printf("Total: %d items\n", count);
        Serial.println("========================");
    }
}

void FlashModule::showTree(bool jsonOutput) {
    if (jsonOutput) {
        Serial.print("{\"tree\":");
        listDirRecursive("/", 0, jsonOutput);
        Serial.println("}\n");
    } else {
        Serial.println("=== Flash File Tree ===");
        Serial.println("/");
        listDirRecursive("/", 1, jsonOutput);
        Serial.println("====================");
    }
}

void FlashModule::showInfo(bool jsonOutput) {
    FSInfo fsinfo;
    LittleFS.info(fsinfo);
    
    if (jsonOutput) {
        Serial.printf("{\"totalBytes\":%lu,\"usedBytes\":%lu,\"freeBytes\":%lu}\n",
                     fsinfo.totalBytes, fsinfo.usedBytes, 
                     fsinfo.totalBytes - fsinfo.usedBytes);
    } else {
        Serial.println("=== Flash Info ===");
        Serial.printf("Total: %lu bytes (%.2f KB)\n", 
                     fsinfo.totalBytes, fsinfo.totalBytes / 1024.0);
        Serial.printf("Used:  %lu bytes (%.2f KB)\n", 
                     fsinfo.usedBytes, fsinfo.usedBytes / 1024.0);
        Serial.printf("Free:  %lu bytes (%.2f KB)\n", 
                     fsinfo.totalBytes - fsinfo.usedBytes,
                     (fsinfo.totalBytes - fsinfo.usedBytes) / 1024.0);
        Serial.println("===================");
    }
}

void FlashModule::showFile(const String& path) {
    LFSFile file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        Serial.printf("Error: Cannot open file '%s'\n", path.c_str());
        return;
    }
    
    Serial.printf("=== %s ===\n", path.c_str());
    while (file.available()) {
        Serial.write(file.read());
    }
    Serial.println();
    Serial.println("====================");
    file.close();
}

bool FlashModule::deleteFile(const String& path) {
    return LittleFS.remove(path.c_str());
}

bool FlashModule::uploadFile(const String& path, uint32_t totalSize, Stream& serial) {
    if (!initialized) {
        serial.println("ERROR: Flash not initialized");
        return false;
    }
    
    // Size validation (max 1MB for flash)
    if (totalSize == 0 || totalSize > 1048576) {
        serial.println("ERROR: Size must be 1 to 1048576 bytes");
        return false;
    }
    
    serial.println("READY");
    
    uint32_t bytesReceived = 0;
    uint8_t buffer[512];
    uint32_t lastReport = 0;
    uint32_t lastCheck = millis();
    const uint32_t TIMEOUT_MS = 5000;
    bool firstWrite = true;
    
    while (bytesReceived < totalSize) {
        // NON-BLOCKING: Only read if data is available
        if (serial.available()) {
            size_t toRead = min((size_t)(totalSize - bytesReceived), sizeof(buffer));
            size_t available = serial.available();
            toRead = min(toRead, available);
            
            if (toRead > 0) {
                // Read byte by byte to avoid readBytes() blocking
                size_t bytesRead = 0;
                for (size_t i = 0; i < toRead; i++) {
                    buffer[bytesRead++] = serial.read();
                }
                
                // Write to RAM buffer (fast, non-blocking)
                uint8_t retries = 0;
                while (retries < 10) {
                    if (writeFile(path, buffer, bytesRead, !firstWrite)) {
                        firstWrite = false;
                        break;  // Success
                    }
                    // Buffer full - flush once and retry
                    flushBuffer();
                    yield();
                    retries++;
                }
                
                if (retries >= 10) {
                    serial.println("ERROR: Buffer overflow");
                    closeFile();
                    return false;
                }
                
                bytesReceived += bytesRead;
                lastCheck = millis();
                
                // Progress report every 5KB or at completion
                if (bytesReceived - lastReport >= 5120 || bytesReceived >= totalSize) {
                    serial.print("PROGRESS: ");
                    serial.print(bytesReceived);
                    serial.print("/");
                    serial.println(totalSize);
                    lastReport = bytesReceived;
                }
            }
        }
        
        // Flush buffer periodically (allows USB to service)
        flushBuffer();
        
        // Check timeout
        if (millis() - lastCheck > TIMEOUT_MS) {
            serial.println("ERROR: Timeout waiting for data");
            closeFile();
            return false;
        }
        
        // CRITICAL: Yield to allow USB stack to run
        yield();
    }
    
    // Final flush and close
    bool closeResult = closeFile();
    if (closeResult) {
        serial.println("SUCCESS: File uploaded");
    } else {
        serial.println("SUCCESS with warnings");
    }
    
    return true;
}

bool FlashModule::writeFile(const String& path, const uint8_t* data, size_t length, bool append) {
    if (!initialized) {
        Serial.println("Error: Flash not initialized");
        return false;
    }
    
    // First call - open file and allocate buffer
    if (!writeFileOpen) {
        const char* mode = append ? "a" : "w";
        writeFileHandle = LittleFS.open(path.c_str(), mode);
        if (!writeFileHandle) {
            Serial.printf("Error: Cannot open '%s' for writing\n", path.c_str());
            return false;
        }
        
        // Allocate RAM buffer if needed
        if (!ramBuffer) {
            ramBuffer = (uint8_t*)malloc(BUFFER_SIZE);
            if (!ramBuffer) {
                Serial.println("Error: Cannot allocate RAM buffer");
                writeFileHandle.close();
                return false;
            }
            Serial.printf("[FLASH] RAM buffer allocated: %d bytes\n", BUFFER_SIZE);
        }
        
        ramBufferUsed = 0;
        ramBufferFlushed = 0;
        flowControlPaused = false;
        writeFileOpen = true;
    }
    
    // Check if buffer has space
    if (ramBufferUsed + length > BUFFER_SIZE) {
        // Buffer full - caller should wait and call flushBuffer()
        return false;
    }
    
    // Copy data to RAM buffer (fast, non-blocking)
    memcpy(ramBuffer + ramBufferUsed, data, length);
    ramBufferUsed += length;
    
    // Check flow control thresholds
    int fillPercent = getBufferFillPercent();
    
    if (!flowControlPaused && fillPercent >= XOFF_THRESHOLD) {
        sendFlowControl(true);  // Pause transmission
        flowControlPaused = true;
    }
    
    return true;
}

bool FlashModule::flushBuffer() {
    if (!writeFileOpen || ramBufferFlushed >= ramBufferUsed) {
        return true;  // Nothing to flush
    }
    
    // Write in 256-byte chunks to match flash page size
    // This prevents long blocking periods
    const size_t CHUNK_SIZE = 256;
    size_t toWrite = min(CHUNK_SIZE, ramBufferUsed - ramBufferFlushed);
    
    // This write will block for ~10ms (one flash page)
    size_t written = writeFileHandle.write(ramBuffer + ramBufferFlushed, toWrite);
    
    if (written != toWrite) {
        Serial.printf("[FLASH] Error: Write failed (wrote %d of %d bytes)\n", written, toWrite);
        return false;
    }
    
    ramBufferFlushed += written;
    
    // Check if we can resume flow control
    int fillPercent = getBufferFillPercent();
    
    if (flowControlPaused && fillPercent <= XON_THRESHOLD) {
        sendFlowControl(false);  // Resume transmission
        flowControlPaused = false;
    }
    
    // If buffer completely flushed, reset for next batch
    if (ramBufferFlushed >= ramBufferUsed) {
        ramBufferUsed = 0;
        ramBufferFlushed = 0;
        return true;
    }
    
    return false;  // More data to flush
}

bool FlashModule::closeFile() {
    if (!writeFileOpen) {
        return true;
    }
    
    Serial.println("[FLASH] Closing file...");
    
    // Flush any remaining data
    uint8_t flushCount = 0;
    while (ramBufferUsed > 0 && ramBufferFlushed < ramBufferUsed) {
        flushBuffer();
        yield();  // Allow other tasks to run
        flushCount++;
        if (flushCount > 100) {
            Serial.println("[FLASH] Warning: Too many flush iterations");
            break;
        }
    }
    
    Serial.println("[FLASH] Closing handle...");
    writeFileHandle.close();
    writeFileOpen = false;
    
    // Free RAM buffer
    if (ramBuffer) {
        free(ramBuffer);
        ramBuffer = nullptr;
    }
    
    Serial.println("[FLASH] File closed");
    return true;
}

int FlashModule::getBufferFillPercent() const {
    if (!ramBuffer || BUFFER_SIZE == 0) {
        return 0;
    }
    
    // Calculate based on unflushed data
    size_t unflushed = ramBufferUsed - ramBufferFlushed;
    return (unflushed * 100) / BUFFER_SIZE;
}

int FlashModule::getFileSize(const String& path) {
    LFSFile file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        return -1;
    }
    int size = file.size();
    file.close();
    return size;
}

void FlashModule::sendFlowControl(bool pause) {
    if (pause) {
        Serial.write(XOFF);  // Pause
        Serial.println("[FLOW] XOFF - Buffer filling up");
    } else {
        Serial.write(XON);   // Resume
        Serial.println("[FLOW] XON - Buffer available");
    }
    Serial.flush();
}

void FlashModule::listDirRecursive(const char* path, int level, bool jsonOutput) {
    Dir dir = LittleFS.openDir(path);
    
    if (jsonOutput && level == 0) {
        Serial.print("[");
    }
    
    bool first = true;
    while (dir.next()) {
        if (jsonOutput) {
            if (!first) Serial.print(",\n");
            first = false;
            
            for (int i = 0; i < level + 1; i++) Serial.print("  ");
            Serial.printf("{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu",
                         dir.fileName().c_str(),
                         dir.isDirectory() ? "dir" : "file",
                         (unsigned long)dir.fileSize());
            
            if (dir.isDirectory()) {
                Serial.print(",\"children\":");
                String fullPath = String(path) + "/" + dir.fileName();
                listDirRecursive(fullPath.c_str(), level + 1, jsonOutput);
                for (int i = 0; i < level + 1; i++) Serial.print("  ");
            }
            Serial.print("}");
        } else {
            // Print indentation
            for (int i = 0; i < level; i++) {
                Serial.print("  ");
            }
            
            if (dir.isDirectory()) {
                Serial.printf("+-- %s/\n", dir.fileName().c_str());
                String fullPath = String(path) + "/" + dir.fileName();
                listDirRecursive(fullPath.c_str(), level + 1, jsonOutput);
            } else {
                Serial.printf("+-- %s (%lu bytes)\n", 
                            dir.fileName().c_str(), 
                            (unsigned long)dir.fileSize());
            }
        }
    }
    
    if (jsonOutput && level == 0) {
        Serial.print("\n]");
    }
}
