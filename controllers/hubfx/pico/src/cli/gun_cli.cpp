/**
 * @file gun_cli.cpp
 * @brief Gun effects (GunFX slave) command handler implementation
 */

#include "gun_cli.h"
#include "command_parser.h"

bool GunCli::handleCommand(const String& cmd) {
    if (!gunFx) return false;
    
    CommandParser p(cmd);
    
    // ---- GUN STATUS [--json] ----
    if (p.matches("gun", "status") || p.is("gun")) {
        if (p.is("gun") && !p.jsonRequested()) {
            // Just "gun" - show status
        }
        
        const GunFxStatus& slave = gunFx->slaveStatus();
        
        if (p.jsonRequested()) {
            Serial.printf("{\"connected\":%s,\"slaveReady\":%s,"
                         "\"firing\":%s,\"rpm\":%d,\"rateIndex\":%d,"
                         "\"heaterOn\":%s,\"triggerPwm\":%d,\"heaterPwm\":%d,"
                         "\"pitchPwm\":%d,\"yawPwm\":%d,"
                         "\"slave\":{\"flashActive\":%s,\"fanOn\":%s,\"heaterOn\":%s}}\n",
                         gunFx->isConnected() ? "true" : "false",
                         gunFx->isSlaveReady() ? "true" : "false",
                         gunFx->isFiring() ? "true" : "false",
                         gunFx->rpm(),
                         gunFx->rateIndex(),
                         gunFx->isHeaterOn() ? "true" : "false",
                         gunFx->triggerPwm(),
                         gunFx->heaterTogglePwm(),
                         gunFx->pitchPwm(),
                         gunFx->yawPwm(),
                         slave.flashActive ? "true" : "false",
                         slave.fanOn ? "true" : "false",
                         slave.heaterOn ? "true" : "false");
        } else {
            Serial.println("\n=== Gun FX Status ===");
            Serial.printf("Connected: %s\n", gunFx->isConnected() ? "YES" : "NO");
            Serial.printf("Slave Ready: %s\n", gunFx->isSlaveReady() ? "YES" : "NO");
            Serial.println();
            
            Serial.println("--- Firing ---");
            Serial.printf("Firing: %s\n", gunFx->isFiring() ? "YES" : "NO");
            Serial.printf("RPM: %d\n", gunFx->rpm());
            Serial.printf("Rate Index: %d\n", gunFx->rateIndex());
            Serial.println();
            
            Serial.println("--- Smoke ---");
            Serial.printf("Heater: %s\n", gunFx->isHeaterOn() ? "ON" : "OFF");
            Serial.println();
            
            Serial.println("--- PWM Inputs ---");
            Serial.printf("Trigger: %d us\n", gunFx->triggerPwm());
            Serial.printf("Heater Toggle: %d us\n", gunFx->heaterTogglePwm());
            Serial.printf("Pitch Servo: %d us\n", gunFx->pitchPwm());
            Serial.printf("Yaw Servo: %d us\n", gunFx->yawPwm());
            Serial.println();
            
            Serial.println("--- Slave Board ---");
            Serial.printf("Flash Active: %s\n", slave.flashActive ? "YES" : "NO");
            Serial.printf("Fan On: %s\n", slave.fanOn ? "YES" : "NO");
            Serial.printf("Heater On: %s\n", slave.heaterOn ? "YES" : "NO");
            Serial.println();
        }
        return true;
    }
    
    // ---- GUN FIRE <rpm> ----
    if (p.matches("gun", "fire")) {
        int rpm = p.argInt(0, 600);  // Default 600 RPM
        if (p.jsonRequested()) {
            Serial.printf("{\"command\":\"fire\",\"rpm\":%d}\n", rpm);
        } else {
            Serial.printf("Firing at %d RPM\n", rpm);
        }
        gunFx->trigger(rpm);
        return true;
    }
    
    // ---- GUN CEASEFIRE | GUN STOP ----
    if (p.matches("gun", "ceasefire") || p.matches("gun", "stop")) {
        if (p.jsonRequested()) {
            Serial.println("{\"command\":\"ceasefire\"}");
        } else {
            Serial.println("Cease fire");
        }
        gunFx->ceaseFire();
        return true;
    }
    
    // ---- GUN HEATER <on|off> ----
    if (p.matches("gun", "heater")) {
        String arg = p.arg(0);
        bool on = (arg == "on" || arg == "1" || arg == "true");
        
        if (p.jsonRequested()) {
            Serial.printf("{\"command\":\"heater\",\"state\":\"%s\"}\n", on ? "on" : "off");
        } else {
            Serial.printf("Smoke heater %s\n", on ? "ON" : "OFF");
        }
        gunFx->setSmokeHeater(on);
        return true;
    }
    
    // ---- GUN SERVO <id> <pulse_us> ----
    if (p.matches("gun", "servo")) {
        int servoId = p.argInt(0, -1);
        int pulseUs = p.argInt(1, -1);
        
        if (servoId < 0 || servoId > 2 || pulseUs < 500 || pulseUs > 2500) {
            if (p.jsonRequested()) {
                Serial.println("{\"error\":\"Invalid servo ID (0-2) or pulse (500-2500us)\"}");
            } else {
                Serial.println("Error: servo ID must be 0-2, pulse must be 500-2500us");
            }
            return true;
        }
        
        if (p.jsonRequested()) {
            Serial.printf("{\"command\":\"servo\",\"id\":%d,\"pulseUs\":%d}\n", servoId, pulseUs);
        } else {
            Serial.printf("Setting servo %d to %d us\n", servoId, pulseUs);
        }
        gunFx->setServo(servoId, pulseUs);
        return true;
    }
    
    return false;
}

void GunCli::printHelp() const {
    Serial.println("=== Gun FX Commands ===");
    Serial.println("  gun                      - Show gun status");
    Serial.println("  gun status [--json]      - Show gun and slave status");
    Serial.println("  gun fire [rpm]           - Trigger firing (default 600 RPM)");
    Serial.println("  gun ceasefire            - Stop firing");
    Serial.println("  gun heater <on|off>      - Control smoke heater");
    Serial.println("  gun servo <id> <pulse>   - Set servo position (0-2, 500-2500us)");
}
