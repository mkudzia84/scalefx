/**
 * @file engine_cli.cpp
 * @brief Engine effects command handler implementation
 */

#include "engine_cli.h"
#include "command_parser.h"

bool EngineCli::handleCommand(const String& cmd) {
    if (!engine) return false;
    
    CommandParser p(cmd);
    
    // Engine start command
    if (p.is("engine") || p.matches("engine", "start")) {
        Serial.println("Starting engine effects");
        engine->forceStart();
        return true;
    }
    
    // Engine stop command
    if (p.matches("engine", "stop")) {
        Serial.println("Stopping engine effects");
        engine->forceStop();
        return true;
    }
    
    // Engine status [--json|-j]
    if (p.matches("engine", "status")) {
        if (p.jsonRequested()) {
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
