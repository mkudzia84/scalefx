/*
 * SD Card Module - Implementation
 * 
 * Handles SD card initialization, file operations, and debug commands
 */

#include "sd_card.h"

SdCardModule::SdCardModule() : initialized(false) {
}

bool SdCardModule::begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin, uint8_t miso_pin, uint8_t speed_mhz) {
    Serial.println("[SD] Initializing SD card...");
    
    // Configure SPI pins
    SPI.setRX(miso_pin);
    SPI.setTX(mosi_pin);
    SPI.setSCK(sck_pin);
    
    // Initialize SD card
    if (!sd.begin(cs_pin, SD_SCK_MHZ(speed_mhz))) {
        Serial.println("[SD] Initialization failed!");
        Serial.println("[SD] Check wiring and card format (FAT32)");
        initialized = false;
        return false;
    }
    
    // Print card info
    uint32_t size = sd.card()->sectorCount();
    if (size > 0) {
        Serial.printf("[SD] Card size: %lu MB\n", (unsigned long)(size / 2048));
    }
    
    Serial.println("[SD] Initialization complete");
    initialized = true;
    return true;
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
