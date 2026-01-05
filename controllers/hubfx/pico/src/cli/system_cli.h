/**
 * @file system_cli.h
 * @brief System-level commands (help, version, etc.)
 */

#ifndef SYSTEM_CLI_H
#define SYSTEM_CLI_H

#include "../cli/command_handler.h"
#include <vector>

// Forward declarations for slave status
class GunFX;
class SdCardModule;

class SystemCli : public CommandHandler {
private:
    std::vector<CommandHandler*> allHandlers;
    GunFX* _gunFx = nullptr;
    SdCardModule* _sdCard = nullptr;
    const char* _firmwareVersion = "0.1.0";
    int _buildNumber = 0;
    
public:
    SystemCli() {}
    
    void registerHandlers(const std::vector<CommandHandler*>& handlers) {
        allHandlers = handlers;
    }
    
    void setGunFX(GunFX* gunFx) { _gunFx = gunFx; }
    void setSdCard(SdCardModule* sdCard) { _sdCard = sdCard; }
    void setVersion(const char* version, int build) { _firmwareVersion = version; _buildNumber = build; }
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "System"; }
};

#endif // SYSTEM_CLI_H
