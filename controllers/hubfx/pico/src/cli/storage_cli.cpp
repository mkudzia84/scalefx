/**
 * @file storage_cli.cpp
 * @brief Storage command handler implementation
 */

#include "storage_cli.h"

bool StorageCli::handleCommand(const String& cmd) {
    // ==================== SD CARD COMMANDS ====================
    
    if (!sdCard) return false;
    
    // NEW API: sd init [speed] - Initialize SD card with optional speed
    if (cmd == "sd init" || cmd.startsWith("sd init ")) {
        String remaining = (cmd == "sd init") ? "" : cmd.substring(8);
        remaining.trim();
        
        uint8_t speed = 20;  // Default to 20 MHz
        if (remaining.length() > 0) {
            speed = remaining.toInt();
            if (speed == 0 || speed > 50) {
                Serial.println("Error: Speed must be 1-50 MHz");
                return true;
            }
        }
        
        Serial.printf("Attempting SD card initialization at %d MHz...\n", speed);
        if (sdCard->retryInit(speed)) {
            Serial.println("Success! SD card initialized.");
        } else {
            Serial.println("Failed. Try different speed or check wiring.");
        }
        return true;
    }
    
    // Check if SD is initialized
    if (!sdCard->isInitialized()) {
        // Only show error for SD commands
        if (cmd == "ls" || cmd.startsWith("ls ") || cmd.startsWith("tree") || 
            cmd.startsWith("sdinfo") || cmd.startsWith("cat ") ||
            cmd == "sd ls" || cmd.startsWith("sd ls ") || cmd == "sd tree" || cmd.startsWith("sd tree ") ||
            cmd == "sd info" || cmd.startsWith("sd info ") || cmd == "sd cat" || cmd.startsWith("sd cat ")) {
            // Check if JSON output was requested
            bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
            if (jsonOutput) {
                Serial.println("{\"error\":\"SD card not initialized\"}");
            } else {
                Serial.println("Error: SD card not initialized");
            }
            return true;
        }
        return false;
    }
    
    // NEW API: sd ls [path] [--json|-j]
    if (cmd == "sd ls" || cmd.startsWith("sd ls ")) {
        String remaining = (cmd == "sd ls") ? "" : cmd.substring(6);
        remaining.trim();
        
        bool jsonOutput = (remaining.indexOf("--json") >= 0 || remaining.indexOf("-j") >= 0);
        
        // Extract path (remove flags)
        String path = remaining;
        path.replace("--json", "");
        path.replace("-j", "");
        path.trim();
        if (path.length() == 0) path = "/";
        
        sdCard->listDirectory(path, jsonOutput);
        return true;
    }
    
    // NEW API: sd tree [--json|-j]
    if (cmd == "sd tree" || cmd.startsWith("sd tree ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        sdCard->showTree(jsonOutput);
        return true;
    }
    
    // NEW API: sd info [--json|-j]
    if (cmd == "sd info" || cmd.startsWith("sd info ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        sdCard->showInfo(jsonOutput);
        return true;
    }
    
    // NEW API: sd cat <file>
    if (cmd.startsWith("sd cat ")) {
        String path = cmd.substring(7);
        path.trim();
        sdCard->showFile(path);
        return true;
    }
    
    // NEW API: sd upload <path> <size>
    if (cmd.startsWith("sd upload ")) {
        String remaining = cmd.substring(10);
        remaining.trim();
        
        int spaceIdx = remaining.indexOf(' ');
        if (spaceIdx < 0) {
            Serial.println("Usage: sd upload <path> <size_bytes>");
            return true;
        }
        
        String path = remaining.substring(0, spaceIdx);
        String sizeStr = remaining.substring(spaceIdx + 1);
        sizeStr.trim();
        
        uint32_t totalSize = sizeStr.toInt();
        if (totalSize == 0 || totalSize > 104857600) {  // Max 100MB
            Serial.println("Error: Size must be 1 to 104857600 bytes (100MB)");
            return true;
        }
        
        // Use unified upload method
        sdCard->uploadFile(path, totalSize, Serial);
        return true;
    }
    
    // NEW API: sd download <path>
    if (cmd.startsWith("sd download ")) {
        String path = cmd.substring(12);
        path.trim();
        
        if (path.length() == 0) {
            Serial.println("Usage: sd download <path>");
            return true;
        }
        
        sdCard->downloadFile(path, Serial);
        return true;
    }
    
    // NEW API: sd rm <path>
    if (cmd.startsWith("sd rm ")) {
        String path = cmd.substring(6);
        path.trim();
        
        if (path.length() == 0) {
            Serial.println("Usage: sd rm <path>");
            return true;
        }
        
        if (sdCard->removeFile(path)) {
            Serial.print("✓ Removed: ");
            Serial.println(path);
        } else {
            Serial.println("✗ Failed to remove file");
        }
        return true;
    }
    
    return false;
}

void StorageCli::printHelp() const {
    Serial.println("=== Storage Commands ===");
    Serial.println();
    Serial.println("SD CARD STORAGE:");
    Serial.println("  sd init [speed]          - Initialize SD card (default 20 MHz)");
    Serial.println("  sd ls [path] [--json]    - List directory contents");
    Serial.println("  sd tree [--json]         - Show directory tree");
    Serial.println("  sd cat <file>            - Display file contents");
    Serial.println("  sd download <file>       - Download file via serial");
    Serial.println("  sd rm <file>             - Remove file");
    Serial.println("  sd upload <path> <size>  - Upload file via serial (max 100MB)");
    Serial.println("  sd info [--json]         - Show SD card information");
}
