/**
 * @file storage_cli.h
 * @brief Storage (SD card) command handler
 */

#ifndef STORAGE_CLI_H
#define STORAGE_CLI_H

#include "../cli/command_handler.h"
#include "../storage/sd_card.h"

class StorageCli : public CommandHandler {
private:
    // Uses SdCardModule singleton directly
    SdCardModule& sdCard() { return SdCardModule::instance(); }
    
public:
    StorageCli() = default;
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Storage"; }
};

#endif // STORAGE_CLI_H
