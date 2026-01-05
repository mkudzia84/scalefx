/**
 * @file storage_cli.cpp
 * @brief Storage command handler implementation (refactored to use CommandParser)
 */

#include "storage_cli.h"
#include "command_parser.h"

bool StorageCli::handleCommand(const String& cmd) {
    CommandParser p(cmd);
    
    // ==================== SD CARD COMMANDS ====================
    
    // sd init [speed] - Initialize SD card with optional speed
    if (p.matches("sd", "init")) {
        uint8_t speed = p.argInt(0, 20);  // Default to 20 MHz
        if (speed == 0 || speed > 50) {
            Serial.println("Error: Speed must be 1-50 MHz");
            return true;
        }
        
        Serial.printf("Attempting SD card initialization at %d MHz...\n", speed);
        Serial.println(sdCard().retryInit(speed) ? "Success! SD card initialized." : "Failed. Try different speed or check wiring.");
        return true;
    }
    
    // Check if SD is initialized for commands that need it
    if (!sdCard().isInitialized()) {
        if (p.hasPrefix("sd ")) {
            if (p.jsonRequested()) {
                Serial.println("{\"error\":\"SD card not initialized\"}");
            } else {
                Serial.println("Error: SD card not initialized");
            }
            return true;
        }
        return false;
    }
    
    // sd ls [path] [--json|-j]
    if (p.matches("sd", "ls")) {
        String path = p.arg(0).length() > 0 ? p.arg(0) : "/";
        sdCard().listDirectory(path, p.jsonRequested());
        return true;
    }
    
    // sd tree [--json|-j]
    if (p.matches("sd", "tree")) {
        sdCard().showTree(p.jsonRequested());
        return true;
    }
    
    // sd info [--json|-j]
    if (p.matches("sd", "info")) {
        sdCard().showInfo(p.jsonRequested());
        return true;
    }
    
    // sd cat <file>
    if (p.matches("sd", "cat")) {
        String path = p.arg(0);
        if (path.length() == 0) {
            Serial.println("Usage: sd cat <file>");
            return true;
        }
        sdCard().showFile(path);
        return true;
    }
    
    // sd upload <path> <size>
    if (p.matches("sd", "upload")) {
        String path = p.arg(0);
        uint32_t totalSize = p.argInt(1, 0);
        
        if (path.length() == 0 || totalSize == 0) {
            Serial.println("Usage: sd upload <path> <size_bytes>");
            return true;
        }
        
        if (totalSize > 104857600) {  // Max 100MB
            Serial.println("Error: Size must be 1 to 104857600 bytes (100MB)");
            return true;
        }
        
        sdCard().uploadFile(path, totalSize, Serial);
        return true;
    }
    
    // sd download <path>
    if (p.matches("sd", "download")) {
        String path = p.arg(0);
        if (path.length() == 0) {
            Serial.println("Usage: sd download <path>");
            return true;
        }
        sdCard().downloadFile(path, Serial);
        return true;
    }
    
    // sd rm <path>
    if (p.matches("sd", "rm")) {
        String path = p.arg(0);
        if (path.length() == 0) {
            Serial.println("Usage: sd rm <path>");
            return true;
        }
        
        if (sdCard().removeFile(path)) {
            Serial.printf("Removed: %s\n", path.c_str());
        } else {
            Serial.println("Failed to remove file");
        }
        return true;
    }
    
    // sd mkdir <path>
    if (p.matches("sd", "mkdir")) {
        String path = p.arg(0);
        if (path.length() == 0) {
            Serial.println("Usage: sd mkdir <path>");
            return true;
        }
        
        if (sdCard().makeDirectory(path)) {
            Serial.printf("Created: %s\n", path.c_str());
        } else {
            Serial.println("Failed to create directory");
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
    Serial.println("  sd mkdir <path>          - Create directory");
    Serial.println("  sd upload <path> <size>  - Upload file via serial (max 100MB)");
    Serial.println("  sd info [--json]         - Show SD card information");
}
