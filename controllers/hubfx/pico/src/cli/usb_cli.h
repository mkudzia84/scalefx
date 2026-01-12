/**
 * @file usb_cli.h
 * @brief USB Host CLI commands
 * 
 * Uses the UsbHost class from serial library
 */

#ifndef USB_CLI_H
#define USB_CLI_H

#include "command_handler.h"
#include <serial.h>

class UsbCli : public CommandHandler {
public:
    UsbCli(UsbHost* usb) : _usb(usb) {}
    
    const char* getName() const override { return "usb"; }
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    
private:
    UsbHost* _usb;
};

#endif // USB_CLI_H
