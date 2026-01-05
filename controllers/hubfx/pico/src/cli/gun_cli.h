/**
 * @file gun_cli.h
 * @brief Gun effects (GunFX slave) command handler
 * 
 * Provides CLI commands for controlling GunFX slave boards
 * connected via USB serial.
 */

#ifndef GUN_CLI_H
#define GUN_CLI_H

#include "../cli/command_handler.h"
#include "../effects/gun_fx.h"

class GunCli : public CommandHandler {
private:
    GunFX* gunFx;
    
public:
    GunCli(GunFX* gun_ptr) : gunFx(gun_ptr) {}
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Gun"; }
};

#endif // GUN_CLI_H
