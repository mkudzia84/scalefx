/**
 * @file config_cli.h
 * @brief Configuration command handler
 */

#ifndef CONFIG_CLI_H
#define CONFIG_CLI_H

#include "../cli/command_handler.h"
#include "../storage/config_reader.h"

class ConfigCli : public CommandHandler {
private:
    ConfigReader* config;
    
public:
    ConfigCli(ConfigReader* config_ptr) : config(config_ptr) {}
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Config"; }
};

#endif // CONFIG_CLI_H
