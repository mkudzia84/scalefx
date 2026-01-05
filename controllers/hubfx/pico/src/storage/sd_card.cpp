/*
 * SD Card Module - Implementation
 * 
 * Handles SD card initialization, file operations, and debug commands
 */

#include "sd_card.h"
#include "storage_config.h"

SdCardModule::SdCardModule() : initialized(false) {
    mutex_init(&_sdMutex);
}

bool SdCardModule::begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin, uint8_t miso_pin, uint8_t speed_mhz) {
    // Store pin configuration
    _cs_pin = cs_pin;
    _sck_pin = sck_pin;
    _mosi_pin = mosi_pin;
    _miso_pin = miso_pin;
    
    SD_LOG("Initializing SD card...");
    SD_LOG("Pin configuration:");
    SD_LOG("  CS (Chip Select): GP%d", cs_pin);
    SD_LOG("  SCK (Clock):      GP%d", sck_pin);
    SD_LOG("  MOSI (Data Out):  GP%d", mosi_pin);
    SD_LOG("  MISO (Data In):   GP%d", miso_pin);
    SD_LOG("  Speed:            %d MHz", speed_mhz);
    
    // Configure SPI pins
    SPI.setRX(miso_pin);
    SPI.setTX(mosi_pin);
    SPI.setSCK(sck_pin);
    
    // Initialize SD card
    SD_LOG("Attempting card detection...");
    if (!sd.begin(cs_pin, SD_SCK_MHZ(speed_mhz))) {
        SD_LOG("╔═══════════════════════════════════════╗");
        SD_LOG("║  SD CARD INITIALIZATION FAILED!       ║");
        SD_LOG("╚═══════════════════════════════════════╝");
        SD_LOG(" ");
        SD_LOG("Troubleshooting checklist:");
        SD_LOG("1. Check wiring:");
        SD_LOG("   - CS   → GP%d (Pin %d)", cs_pin, cs_pin < 16 ? cs_pin + 1 : (cs_pin == 16 ? 21 : (cs_pin == 17 ? 22 : (cs_pin == 18 ? 24 : (cs_pin == 19 ? 25 : 0)))));
        SD_LOG("   - SCK  → GP%d (Pin %d)", sck_pin, sck_pin < 16 ? sck_pin + 1 : (sck_pin == 16 ? 21 : (sck_pin == 17 ? 22 : (sck_pin == 18 ? 24 : (sck_pin == 19 ? 25 : 0)))));
        SD_LOG("   - MOSI → GP%d (Pin %d)", mosi_pin, mosi_pin < 16 ? mosi_pin + 1 : (mosi_pin == 16 ? 21 : (mosi_pin == 17 ? 22 : (mosi_pin == 18 ? 24 : (mosi_pin == 19 ? 25 : 0)))));
        SD_LOG("   - MISO → GP%d (Pin %d)", miso_pin, miso_pin < 16 ? miso_pin + 1 : (miso_pin == 16 ? 21 : (miso_pin == 17 ? 22 : (miso_pin == 18 ? 24 : (miso_pin == 19 ? 25 : 0)))));
        SD_LOG("   - VCC  → 3.3V");
        SD_LOG("   - GND  → GND");
        SD_LOG("2. Check SD card:");
        SD_LOG("   - Card fully inserted");
        SD_LOG("   - Card formatted as FAT32");
        SD_LOG("   - Card capacity ≤32GB (SDHC)");
        SD_LOG("   - Try a different card");
        SD_LOG("3. Check module:");
        SD_LOG("   - Module powered (3.3V)");
        SD_LOG("   - Module compatible with 3.3V");
        SD_LOG("   - Good solder joints");
        SD_LOG("4. Try slower speed:");
        SD_LOG("   - Change speed from 25 MHz to 5 MHz");
        SD_LOG("   - Edit hubfx_pico.ino line 420");
        SD_LOG(" ");
        initialized = false;
        return false;
    }
    
    // Print card info
    uint32_t size = sd.card()->sectorCount();
    if (size > 0) {
        SD_LOG("Card size: %lu MB", (unsigned long)(size / 2048));
    }
    
    SD_LOG("✓ Initialization complete");
    initialized = true;
    return true;
}

bool SdCardModule::retryInit(uint8_t speed_mhz) {
    SD_LOG("Retrying initialization at %d MHz...", speed_mhz);
    return begin(_cs_pin, _sck_pin, _mosi_pin, _miso_pin, speed_mhz);
}

void SdCardModule::listDirectory(const String& path, bool jsonOutput) {
    lock();  // Acquire SD mutex
    
    File32 dir = sd.open(path.c_str(), O_RDONLY);
    if (!dir) {
        unlock();
        if (jsonOutput) {
            Serial.printf("{\"error\":\"Failed to open '%s'\"}\n", path.c_str());
        } else {
            Serial.printf("Error: Failed to open '%s'\n", path.c_str());
        }
        return;
    }
    
    if (!dir.isDirectory()) {
        dir.close();
        unlock();
        if (jsonOutput) {
            Serial.printf("{\"error\":\"'%s' is not a directory\"}\n", path.c_str());
        } else {
            Serial.printf("Error: '%s' is not a directory\n", path.c_str());
        }
        return;
    }
    
    if (jsonOutput) {
        Serial.printf("{\"path\":\"%s\",\"items\":[", path.c_str());
    } else {
        Serial.printf("=== %s ===\n", path.c_str());
    }
    
    int count = 0;
    while (true) {
        File32 entry = dir.openNextFile();
        if (!entry) break;
        
        char name[64];
        entry.getName(name, sizeof(name));
        
        if (jsonOutput) {
            if (count > 0) Serial.print(",");
            Serial.printf("{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu}",
                         name,
                         entry.isDirectory() ? "dir" : "file",
                         (unsigned long)entry.size());
        } else {
            if (entry.isDirectory()) {
                Serial.printf("  [DIR]  %s/\n", name);
            } else {
                Serial.printf("  [FILE] %s (%lu bytes)\n", name, (unsigned long)entry.size());
            }
        }
        count++;
        entry.close();
    }
    dir.close();
    unlock();  // Release SD mutex
    
    if (jsonOutput) {
        Serial.printf("],\"total\":%d}\n", count);
    } else {
        Serial.printf("Total: %d items\n", count);
        Serial.println("========================");
    }
}

void SdCardModule::showTree(bool jsonOutput) {
    lock();  // Acquire SD mutex
    if (jsonOutput) {
        Serial.print("{\"tree\":");
        listDirRecursive("/", 0, jsonOutput);
        Serial.println("}\n");
    } else {
        Serial.println("=== SD Card Tree ===");
        Serial.println("/");
        listDirRecursive("/", 1, jsonOutput);
        Serial.println("====================");
    }
    unlock();  // Release SD mutex
}

void SdCardModule::showInfo(bool jsonOutput) {
    lock();  // Acquire SD mutex
    uint64_t cardSize = sd.card()->sectorCount() * 512ULL;
    uint32_t clusterSize = (unsigned long)sd.clusterCount() * sd.bytesPerCluster() / sd.clusterCount();
    uint32_t totalSpace = (unsigned long)(sd.clusterCount() * sd.bytesPerCluster() / 1048576);
    uint32_t freeSpace = (unsigned long)(sd.freeClusterCount() * sd.bytesPerCluster() / 1048576);
    unlock();  // Release SD mutex before printing
    
    if (jsonOutput) {
        Serial.printf("{\"cardSizeMB\":%llu,\"volumeType\":\"FAT%d\",\"clusterSizeBytes\":%lu,\"totalSpaceMB\":%lu,\"freeSpaceMB\":%lu}\n",
                     cardSize / 1048576, sd.fatType(), clusterSize, totalSpace, freeSpace);
    } else {
        Serial.println("=== SD Card Info ===");
        Serial.printf("Card size: %llu MB\n", cardSize / 1048576);
        Serial.printf("Volume type: FAT%d\n", sd.fatType());
        Serial.printf("Cluster size: %lu bytes\n", clusterSize);
        Serial.printf("Total space: %lu MB\n", totalSpace);
        Serial.printf("Free space: %lu MB\n", freeSpace);
        Serial.println("====================");
    }
}

void SdCardModule::showFile(const String& path) {
    lock();  // Acquire SD mutex
    File32 file = sd.open(path.c_str(), O_RDONLY);
    if (!file) {
        unlock();
        Serial.printf("Error: Failed to open '%s'\n", path.c_str());
        return;
    }
    
    if (file.isDirectory()) {
        file.close();
        unlock();
        Serial.printf("Error: '%s' is a directory\n", path.c_str());
        return;
    }
    
    Serial.printf("=== %s (%lu bytes) ===\n", path.c_str(), (unsigned long)file.size());
    while (file.available()) {
        Serial.write(file.read());
    }
    file.close();
    unlock();  // Release SD mutex
    Serial.println();
    Serial.println("====================");
}

bool SdCardModule::uploadFile(const String& path, uint32_t totalSize, Stream& serial) {
    if (!initialized) {
        serial.println("ERROR: SD card not initialized");
        return false;
    }
    
    // Size validation (max 100MB for SD card)
    if (totalSize == 0 || totalSize > 104857600) {
        serial.println("ERROR: Size must be 1 to 104857600 bytes (100MB)");
        return false;
    }
    
    serial.println("READY");
    
    // Open file for writing (with lock)
    lock();
    File32 file = sd.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) {
        unlock();
        serial.print("ERROR: Cannot open file for writing: ");
        serial.println(path);
        return false;
    }
    unlock();
    
    uint32_t bytesReceived = 0;
    uint8_t buffer[512];
    uint32_t lastReport = 0;
    
    while (bytesReceived < totalSize) {
        // Wait for data with timeout (no lock needed)
        uint32_t startWait = millis();
        while (!serial.available() && (millis() - startWait) < 5000) {
            delay(10);
        }
        
        if (!serial.available()) {
            serial.println("ERROR: Timeout waiting for data");
            lock();
            file.close();
            unlock();
            return false;
        }
        
        // Read available data
        size_t toRead = min((size_t)(totalSize - bytesReceived), sizeof(buffer));
        size_t available = serial.available();
        toRead = min(toRead, available);
        
        size_t bytesRead = serial.readBytes(buffer, toRead);
        
        // Write to SD card (with lock)
        lock();
        size_t written = file.write(buffer, bytesRead);
        unlock();
        
        if (written != bytesRead) {
            serial.println("ERROR: Write failed");
            lock();
            file.close();
            unlock();
            return false;
        }
        
        bytesReceived += bytesRead;
        
        // Progress report every 10KB or at completion
        if (bytesReceived - lastReport >= 10240 || bytesReceived >= totalSize) {
            serial.print("PROGRESS: ");
            serial.print(bytesReceived);
            serial.print("/");
            serial.println(totalSize);
            lastReport = bytesReceived;
        }
    }
    
    // Close file (with lock)
    lock();
    file.sync();
    file.close();
    unlock();
    
    serial.println("SUCCESS: File uploaded");
    return true;
}

bool SdCardModule::downloadFile(const String& path, Stream& serial) {
    if (!initialized) {
        serial.println("ERROR: SD card not initialized");
        return false;
    }
    
    // Open file for reading (with lock)
    lock();
    File32 file = sd.open(path.c_str(), O_RDONLY);
    if (!file) {
        unlock();
        serial.print("ERROR: Cannot open file: ");
        serial.println(path);
        return false;
    }
    
    uint32_t fileSize = file.size();
    unlock();
    
    // Send file size
    serial.print("SIZE: ");
    serial.println(fileSize);
    serial.println("READY");
    
    // Wait for START confirmation (no lock needed)
    uint32_t startWait = millis();
    bool gotStart = false;
    while (millis() - startWait < 5000) {
        if (serial.available()) {
            String response = serial.readStringUntil('\n');
            response.trim();
            if (response == "START") {
                gotStart = true;
                break;
            }
        }
        delay(10);
    }
    
    if (!gotStart) {
        serial.println("ERROR: Timeout waiting for START");
        lock();
        file.close();
        unlock();
        return false;
    }
    
    // Send file data in chunks
    uint8_t buffer[512];
    uint32_t bytesSent = 0;
    uint32_t lastReport = 0;
    
    lock();
    while (file.available()) {
        int bytesRead = file.read(buffer, sizeof(buffer));
        unlock();  // Release lock during serial write
        
        if (bytesRead > 0) {
            serial.write(buffer, bytesRead);
            serial.flush();
            bytesSent += bytesRead;
            
            // Progress report every 10KB
            if (bytesSent - lastReport >= 10240 || bytesSent >= fileSize) {
                serial.print("\nPROGRESS: ");
                serial.print(bytesSent);
                serial.print("/");
                serial.print(fileSize);
                serial.println();
                lastReport = bytesSent;
            }
        }
        
        lock();  // Re-acquire for next read
    }
    file.close();
    unlock();
    
    serial.println("\nSUCCESS: Download complete");
    return true;
}

bool SdCardModule::removeFile(const String& path) {
    if (!initialized) {
        Serial.println("Error: SD card not initialized");
        return false;
    }
    
    lock();
    if (!sd.exists(path.c_str())) {
        unlock();
        Serial.print("Error: File not found: ");
        Serial.println(path);
        return false;
    }
    
    if (!sd.remove(path.c_str())) {
        unlock();
        Serial.print("Error: Failed to remove file: ");
        Serial.println(path);
        return false;
    }
    unlock();
    
    return true;
}

bool SdCardModule::makeDirectory(const String& path) {
    if (!initialized) {
        Serial.println("Error: SD card not initialized");
        return false;
    }
    
    lock();
    // Check if already exists
    if (sd.exists(path.c_str())) {
        File32 dir = sd.open(path.c_str(), O_RDONLY);
        if (dir && dir.isDirectory()) {
            dir.close();
            unlock();
            return true;  // Directory already exists
        }
        if (dir) dir.close();
        unlock();
        Serial.print("Error: Path exists but is not a directory: ");
        Serial.println(path);
        return false;
    }
    
    // Create directory (mkdir creates recursively)
    if (!sd.mkdir(path.c_str())) {
        unlock();
        Serial.print("Error: Failed to create directory: ");
        Serial.println(path);
        return false;
    }
    unlock();
    
    return true;
}

void SdCardModule::listDirRecursive(const char* path, int level, bool jsonOutput) {
    File32 dir = sd.open(path, O_RDONLY);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        if (jsonOutput && level == 0) Serial.print("[]");
        return;
    }
    
    if (jsonOutput && level == 0) Serial.print("[\n");
    
    bool first = true;
    while (true) {
        File32 entry = dir.openNextFile();
        if (!entry) break;
        
        char name[64];
        entry.getName(name, sizeof(name));
        
        if (jsonOutput) {
            if (!first) Serial.print(",\n");
            first = false;
            
            for (int i = 0; i < level + 1; i++) Serial.print("  ");
            Serial.printf("{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu",
                         name,
                         entry.isDirectory() ? "dir" : "file",
                         (unsigned long)entry.size());
            
            if (entry.isDirectory()) {
                Serial.print(",\"children\":");
                char fullPath[128];
                snprintf(fullPath, sizeof(fullPath), "%s%s%s", 
                        path, 
                        (path[strlen(path)-1] == '/') ? "" : "/",
                        name);
                entry.close();
                listDirRecursive(fullPath, level + 1, jsonOutput);
                for (int i = 0; i < level + 1; i++) Serial.print("  ");
            } else {
                entry.close();
            }
            Serial.print("}");
        } else {
            // Print indentation
            for (int i = 0; i < level; i++) {
                Serial.print("  ");
            }
            
            if (entry.isDirectory()) {
                Serial.printf("+-- %s/\n", name);
                
                // Build full path for recursion
                char fullPath[128];
                snprintf(fullPath, sizeof(fullPath), "%s%s%s", 
                        path, 
                        (path[strlen(path)-1] == '/') ? "" : "/",
                        name);
                
                entry.close();
                listDirRecursive(fullPath, level + 1, jsonOutput);
            } else {
                Serial.printf("+-- %s (%lu bytes)\n", name, (unsigned long)entry.size());
                entry.close();
            }
        }
    }
    dir.close();
    
    if (jsonOutput && level == 0) Serial.print("\n]");
}
