/*
 * SD Card Module - Implementation
 * 
 * Handles SD card initialization, file operations, and debug commands
 */

#include "sd_card.h"

SdCardModule::SdCardModule() : initialized(false) {
}

bool SdCardModule::begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin, uint8_t miso_pin, uint8_t speed_mhz) {
    // Store pin configuration
    _cs_pin = cs_pin;
    _sck_pin = sck_pin;
    _mosi_pin = mosi_pin;
    _miso_pin = miso_pin;
    
    Serial.println("[SD] Initializing SD card...");
    Serial.printf("[SD] Pin configuration:\n");
    Serial.printf("[SD]   CS (Chip Select): GP%d\n", cs_pin);
    Serial.printf("[SD]   SCK (Clock):      GP%d\n", sck_pin);
    Serial.printf("[SD]   MOSI (Data Out):  GP%d\n", mosi_pin);
    Serial.printf("[SD]   MISO (Data In):   GP%d\n", miso_pin);
    Serial.printf("[SD]   Speed:            %d MHz\n", speed_mhz);
    
    // Configure SPI pins
    SPI.setRX(miso_pin);
    SPI.setTX(mosi_pin);
    SPI.setSCK(sck_pin);
    
    // Initialize SD card
    Serial.println("[SD] Attempting card detection...");
    if (!sd.begin(cs_pin, SD_SCK_MHZ(speed_mhz))) {
        Serial.println("[SD] ╔═══════════════════════════════════════╗");
        Serial.println("[SD] ║  SD CARD INITIALIZATION FAILED!       ║");
        Serial.println("[SD] ╚═══════════════════════════════════════╝");
        Serial.println("[SD] ");
        Serial.println("[SD] Troubleshooting checklist:");
        Serial.println("[SD] 1. Check wiring:");
        Serial.printf("[SD]    - CS   → GP%d (Pin %d)\n", cs_pin, cs_pin < 16 ? cs_pin + 1 : (cs_pin == 16 ? 21 : (cs_pin == 17 ? 22 : (cs_pin == 18 ? 24 : (cs_pin == 19 ? 25 : 0)))));
        Serial.printf("[SD]    - SCK  → GP%d (Pin %d)\n", sck_pin, sck_pin < 16 ? sck_pin + 1 : (sck_pin == 16 ? 21 : (sck_pin == 17 ? 22 : (sck_pin == 18 ? 24 : (sck_pin == 19 ? 25 : 0)))));
        Serial.printf("[SD]    - MOSI → GP%d (Pin %d)\n", mosi_pin, mosi_pin < 16 ? mosi_pin + 1 : (mosi_pin == 16 ? 21 : (mosi_pin == 17 ? 22 : (mosi_pin == 18 ? 24 : (mosi_pin == 19 ? 25 : 0)))));
        Serial.printf("[SD]    - MISO → GP%d (Pin %d)\n", miso_pin, miso_pin < 16 ? miso_pin + 1 : (miso_pin == 16 ? 21 : (miso_pin == 17 ? 22 : (miso_pin == 18 ? 24 : (miso_pin == 19 ? 25 : 0)))));
        Serial.println("[SD]    - VCC  → 3.3V");
        Serial.println("[SD]    - GND  → GND");
        Serial.println("[SD] 2. Check SD card:");
        Serial.println("[SD]    - Card fully inserted");
        Serial.println("[SD]    - Card formatted as FAT32");
        Serial.println("[SD]    - Card capacity ≤32GB (SDHC)");
        Serial.println("[SD]    - Try a different card");
        Serial.println("[SD] 3. Check module:");
        Serial.println("[SD]    - Module powered (3.3V)");
        Serial.println("[SD]    - Module compatible with 3.3V");
        Serial.println("[SD]    - Good solder joints");
        Serial.println("[SD] 4. Try slower speed:");
        Serial.println("[SD]    - Change speed from 25 MHz to 5 MHz");
        Serial.println("[SD]    - Edit hubfx_pico.ino line 420");
        Serial.println("[SD] ");
        initialized = false;
        return false;
    }
    
    // Print card info
    uint32_t size = sd.card()->sectorCount();
    if (size > 0) {
        Serial.printf("[SD] Card size: %lu MB\n", (unsigned long)(size / 2048));
    }
    
    Serial.println("[SD] ✓ Initialization complete");
    initialized = true;
    return true;
}

bool SdCardModule::retryInit(uint8_t speed_mhz) {
    Serial.printf("[SD] Retrying initialization at %d MHz...\n", speed_mhz);
    return begin(_cs_pin, _sck_pin, _mosi_pin, _miso_pin, speed_mhz);
}

void SdCardModule::listDirectory(const String& path, bool jsonOutput) {
    File32 dir = sd.open(path.c_str(), O_RDONLY);
    if (!dir) {
        if (jsonOutput) {
            Serial.printf("{\"error\":\"Failed to open '%s'\"}\n", path.c_str());
        } else {
            Serial.printf("Error: Failed to open '%s'\n", path.c_str());
        }
        return;
    }
    
    if (!dir.isDirectory()) {
        if (jsonOutput) {
            Serial.printf("{\"error\":\"'%s' is not a directory\"}\n", path.c_str());
        } else {
            Serial.printf("Error: '%s' is not a directory\n", path.c_str());
        }
        dir.close();
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
    
    if (jsonOutput) {
        Serial.printf("],\"total\":%d}\n", count);
    } else {
        Serial.printf("Total: %d items\n", count);
        Serial.println("========================");
    }
}

void SdCardModule::showTree(bool jsonOutput) {
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
}

void SdCardModule::showInfo(bool jsonOutput) {
    uint64_t cardSize = sd.card()->sectorCount() * 512ULL;
    uint32_t clusterSize = (unsigned long)sd.clusterCount() * sd.bytesPerCluster() / sd.clusterCount();
    uint32_t totalSpace = (unsigned long)(sd.clusterCount() * sd.bytesPerCluster() / 1048576);
    uint32_t freeSpace = (unsigned long)(sd.freeClusterCount() * sd.bytesPerCluster() / 1048576);
    
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
    File32 file = sd.open(path.c_str(), O_RDONLY);
    if (!file) {
        Serial.printf("Error: Failed to open '%s'\n", path.c_str());
        return;
    }
    
    if (file.isDirectory()) {
        Serial.printf("Error: '%s' is a directory\n", path.c_str());
        file.close();
        return;
    }
    
    Serial.printf("=== %s (%lu bytes) ===\n", path.c_str(), (unsigned long)file.size());
    while (file.available()) {
        Serial.write(file.read());
    }
    file.close();
    Serial.println();
    Serial.println("====================");
}

bool SdCardModule::writeFile(const String& path, const uint8_t* data, size_t length, bool append) {
    if (!initialized) {
        Serial.println("Error: SD card not initialized");
        return false;
    }
    
    // Open file for writing (create if doesn't exist)
    if (!writeFileHandle.isOpen()) {
        int flags = O_WRONLY | O_CREAT;
        if (append) {
            flags |= O_AT_END;
        } else {
            flags |= O_TRUNC;  // Truncate if not appending
        }
        
        if (!writeFileHandle.open(path.c_str(), flags)) {
            Serial.print("Error: Cannot open file for writing: ");
            Serial.println(path);
            return false;
        }
    }
    
    // Write data
    size_t written = writeFileHandle.write(data, length);
    if (written != length) {
        Serial.println("Error: Write failed");
        writeFileHandle.close();
        return false;
    }
    
    return true;
}

bool SdCardModule::closeFile() {
    if (writeFileHandle.isOpen()) {
        writeFileHandle.sync();  // Ensure data is flushed
        writeFileHandle.close();
        return true;
    }
    return false;
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
