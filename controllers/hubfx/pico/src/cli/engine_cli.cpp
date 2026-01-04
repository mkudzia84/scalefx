/**
 * @file engine_cli.cpp
 * @brief Engine effects command handler implementation
 */

#include "engine_cli.h"

bool EngineCli::handleCommand(const String& cmd) {
    if (!engine) return false;
    
    // Engine start command
    if (cmd == "engine start" || cmd == "engine") {
        Serial.println("Starting engine effects");
        engine->forceStart();
        return true;
    }
    
    // Engine stop command
    if (cmd == "engine stop") {
        Serial.println("Stopping engine effects");
        engine->forceStop();
        return true;
    }
    
    // Engine status [--json|-j]
    if (cmd == "engine status" || cmd.startsWith("engine status ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        
        if (jsonOutput) {
            Serial.printf("{\"state\":\"%s\",\"toggleEngaged\":%s,\"active\":%s}\n",
                         EngineFX::stateString(engine->state()),
                         engine->isToggleEngaged() ? "true" : "false",
                         engine->isActive() ? "true" : "false");
        } else {
            Serial.println("\n=== Engine Status ===");
            Serial.print("State: ");
            Serial.println(EngineFX::stateString(engine->state()));
            Serial.print("Toggle Engaged: ");
            Serial.println(engine->isToggleEngaged() ? "YES" : "NO");
            Serial.println();
        }
        return true;
    }
    
    return false;
}

void EngineCli::printHelp() const {
    Serial.println("=== Engine Commands ===");
    Serial.println("  engine                   - Start engine effects");
    Serial.println("  engine start             - Start engine effects");
    Serial.println("  engine stop              - Stop engine effects");
    Serial.println("  engine status [--json]   - Show engine status");
}
