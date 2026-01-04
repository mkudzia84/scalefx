/**
 * @file storage_cli.cpp
 * @brief Storage command handler implementation
 */

#include "storage_cli.h"

bool StorageCli::handleCommand(const String& cmd) {
    if (!sdCard) return false;
    
    // Check if SD is initialized
    if (!sdCard->isInitialized()) {
        // Only show error for SD commands
        if (cmd == "ls" || cmd.startsWith("ls ") || cmd.startsWith("tree") || 
            cmd.startsWith("sdinfo") || cmd.startsWith("cat ")) {
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
    
    // ls [path] [--json|-j]
    if (cmd == "ls" || cmd.startsWith("ls ")) {
        String remaining = (cmd == "ls") ? "" : cmd.substring(3);
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
    
    // tree [--json|-j]
    if (cmd == "tree" || cmd.startsWith("tree ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        sdCard->showTree(jsonOutput);
        return true;
    }
    
    // sdinfo [--json|-j]
    if (cmd == "sdinfo" || cmd.startsWith("sdinfo ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        sdCard->showInfo(jsonOutput);
        return true;
    }
    
    // cat <file>
    if (cmd.startsWith("cat ")) {
        String path = cmd.substring(4);
        path.trim();
        sdCard->showFile(path);
        return true;
    }
    
    return false;
}

void StorageCli::printHelp() const {
    Serial.println("=== Storage Commands ===");
    Serial.println("  ls [path] [--json]       - List directory contents");
    Serial.println("  tree [--json]            - Show directory tree");
    Serial.println("  cat <file>               - Display file contents");
    Serial.println("  sdinfo [--json]          - Show SD card information");
}
