#!/bin/bash
#
# Uninstallation script for Helicopter FX system
# Run with sudo: sudo ./uninstall.sh [user]
#
# Arguments:
#   user - Username to uninstall for (default: pi)
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse command-line arguments
USER="${1:-pi}"
INSTALL_DIR="/home/$USER/helifx"
SERVICE_NAME="helifx.service"

echo -e "${RED}======================================${NC}"
echo -e "${RED}Helicopter FX Uninstallation Script${NC}"
echo -e "${RED}======================================${NC}"
echo -e "${YELLOW}Uninstalling for user:${NC} $USER"
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

echo -e "${YELLOW}Warning: This will remove the Helicopter FX system${NC}"
read -p "Are you sure you want to continue? (y/n): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${GREEN}Uninstallation cancelled${NC}"
    exit 0
fi

# Stop and disable service
echo -e "${YELLOW}Stopping and disabling service...${NC}"
if systemctl is-active --quiet $SERVICE_NAME; then
    systemctl stop $SERVICE_NAME
    echo -e "${GREEN}  ✓ Service stopped${NC}"
fi

if systemctl is-enabled --quiet $SERVICE_NAME 2>/dev/null; then
    systemctl disable $SERVICE_NAME
    echo -e "${GREEN}  ✓ Service disabled${NC}"
fi

# Remove service file
if [ -f "/etc/systemd/system/$SERVICE_NAME" ]; then
    rm /etc/systemd/system/$SERVICE_NAME
    systemctl daemon-reload
    echo -e "${GREEN}  ✓ Service file removed${NC}"
fi

echo ""

# Ask about removing installation directory
echo -e "${YELLOW}Remove installation directory?${NC}"
echo -e "${YELLOW}Location: $INSTALL_DIR${NC}"
echo -e "${YELLOW}This will delete configuration and log files${NC}"
read -p "Remove directory? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    if [ -d "$INSTALL_DIR" ]; then
        rm -rf "$INSTALL_DIR"
        echo -e "${GREEN}  ✓ Installation directory removed${NC}"
    else
        echo -e "${YELLOW}  ⚠ Directory not found${NC}"
    fi
else
    echo -e "${YELLOW}  ⚠ Installation directory preserved${NC}"
fi

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Uninstallation Complete!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
