/**
 * @file system_cli.h
 * @brief System-level commands (help, version, etc.)
 */

#ifndef SYSTEM_CLI_H
#define SYSTEM_CLI_H

#include "../cli/command_handler.h"
#include "../storage/sd_card.h"
#include <vector>

// Forward declaration for slave status
class GunFX;

class SystemCli : public CommandHandler {
private:
    std::vector<CommandHandler*> allHandlers;
    GunFX* _gunFx = nullptr;
    const char* _firmwareVersion = "0.1.0";
    int _buildNumber = 0;
    
    // Uses SdCardModule singleton directly
    SdCardModule& sdCard() { return SdCardModule::instance(); }
    
public:
    SystemCli() {}
    
    void registerHandlers(const std::vector<CommandHandler*>& handlers) {
        allHandlers = handlers;
    }
    
    void setGunFX(GunFX* gunFx) { _gunFx = gunFx; }
    void setVersion(const char* version, int build) { _firmwareVersion = version; _buildNumber = build; }
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "System"; }
};

#endif // SYSTEM_CLI_H
