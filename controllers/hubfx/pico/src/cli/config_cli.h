/**
 * @file config_cli.h
 * @brief Configuration command handler
 */

#ifndef CONFIG_CLI_H
#define CONFIG_CLI_H

#include "../cli/command_handler.h"
#include "../storage/config_reader.h"

// Forward declaration
class SdCardModule;

class ConfigCli : public CommandHandler {
private:
    ConfigReader* config;
    SdCardModule* sdCard;
    
public:
    ConfigCli(ConfigReader* config_ptr, SdCardModule* sd_ptr = nullptr) 
        : config(config_ptr), sdCard(sd_ptr) {}
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Config"; }
};

#endif // CONFIG_CLI_H
