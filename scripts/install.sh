#!/bin/bash
#
# Installation script for Helicopter FX system
# Run with sudo: sudo ./install.sh [user] [config_path]
#
# Arguments:
#   user         - Username to install for (default: pi)
#   config_path  - Path to config.yaml (default: /home/user/helifx/config.yaml)
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
CONFIG_PATH="${2:-$INSTALL_DIR/config.yaml}"
SERVICE_NAME="helifx.service"

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Helicopter FX Installation Script${NC}"
echo -e "${GREEN}======================================${NC}"
echo -e "${YELLOW}Installing for user:${NC} $USER"
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo -e "${YELLOW}Configuration file:${NC} $CONFIG_PATH"
echo ""

# Verify config path exists
if [ ! -f "$CONFIG_PATH" ]; then
    echo -e "${RED}Error: Configuration file not found at: $CONFIG_PATH${NC}"
    exit 1
fi

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Check if user exists
if ! id "$USER" &>/dev/null; then
    echo -e "${RED}Error: User '$USER' does not exist${NC}"
    exit 1
fi

# Install dependencies
echo -e "${YELLOW}Installing dependencies...${NC}"
apt-get update
apt-get install -y \
    build-essential \
    libyaml-dev \
    libcyaml-dev \
    libasound2-dev \
    libgpiod-dev \
    gpiod

echo -e "${GREEN}All dependencies installed${NC}"
echo -e "${GREEN}Using libgpiod for GPIO control (no daemon required)${NC}"
echo ""

# Build the application
echo -e "${YELLOW}Building helifx...${NC}"
make clean
make
echo -e "${GREEN}Build complete${NC}"
echo ""

# Create installation directory
echo -e "${YELLOW}Creating installation directory...${NC}"
mkdir -p "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR/assets"

# Copy files
echo -e "${YELLOW}Installing application files...${NC}"
cp ./build/helifx "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/helifx"

# Copy config (don't overwrite existing)
if [ ! -f "$INSTALL_DIR/config.yaml" ]; then
    cp ./config.yaml "$INSTALL_DIR/"
    echo -e "${GREEN}  ✓ Configuration copied${NC}"
else
    echo -e "${YELLOW}  ⚠ Config exists, skipping (backup at config.yaml.new)${NC}"
    cp ./config.yaml "$INSTALL_DIR/config.yaml.new"
fi

# Set ownership
chown -R $USER:$USER "$INSTALL_DIR"
echo -e "${GREEN}Files installed${NC}"
echo ""

# Install audio setup script
echo -e "${YELLOW}Installing audio setup script...${NC}"
cp ./scripts/setup-audio-levels.sh /usr/local/bin/helifx-audio-setup
chmod +x /usr/local/bin/helifx-audio-setup
echo -e "${GREEN}Audio setup script installed${NC}"

# Install audio systemd service
cp ./scripts/helifx-audio.service /etc/systemd/system/
echo -e "${GREEN}Audio service installed${NC}"

# Install systemd service
echo -e "${YELLOW}Installing systemd service...${NC}"

# Get user UID for runtime directories
USER_UID=$(id -u $USER)
USER_HOME=$(eval echo ~$USER)

# Update service file with user-specific paths and config path
sed -e "s|{{USER}}|$USER|g" \
    -e "s|{{USER_HOME}}|$USER_HOME|g" \
    -e "s|{{USER_UID}}|$USER_UID|g" \
    -e "s|{{INSTALL_DIR}}|$INSTALL_DIR|g" \
    -e "s|{{CONFIG_PATH}}|$CONFIG_PATH|g" \
    "./scripts/helifx.service" > /etc/systemd/system/$SERVICE_NAME

systemctl daemon-reload
echo -e "${GREEN}Service installed${NC}"
echo ""

# Service configuration
echo -e "${YELLOW}Service configuration:${NC}"
read -p "Enable audio setup on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable helifx-audio.service
    echo -e "${GREEN}  ✓ Audio setup auto-start enabled${NC}"
fi

read -p "Enable helifx auto-start on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable $SERVICE_NAME
    echo -e "${GREEN}  ✓ HeliFX auto-start enabled${NC}"
fi

# Run audio setup now
echo -e "${YELLOW}Running audio setup...${NC}"
/usr/local/bin/helifx-audio-setup --verbose || echo -e "${YELLOW}  ⚠ Audio setup returned errors (may be normal)${NC}"
echo ""

read -p "Start service now? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl start $SERVICE_NAME
    sleep 1
    systemctl status $SERVICE_NAME --no-pager
fi

echo ""
echo -e "${YELLOW}Audio setup commands:${NC}"
echo "  sudo helifx-audio-setup              # Run audio setup manually"
echo "  sudo helifx-audio-setup --verbose    # Show detailed output"
echo "  sudo systemctl status helifx-audio   # Check audio service status"
echo ""

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo -e "${YELLOW}Configuration file:${NC} $CONFIG_PATH"
echo -e "${YELLOW}Service commands:${NC}"
echo "  sudo systemctl start $SERVICE_NAME"
echo "  sudo systemctl stop $SERVICE_NAME"
echo "  sudo systemctl restart $SERVICE_NAME"
echo "  sudo systemctl status $SERVICE_NAME"
echo ""
echo -e "${YELLOW}Configuration:${NC}"
echo "  Edit config: $CONFIG_PATH"
echo "  Verify audio: sudo helifx-audio-setup --verbose"
echo "  Start service: sudo systemctl start $SERVICE_NAME"
echo ""
echo -e "${YELLOW}Note:${NC} Audio HAT pins are protected by software"
echo "      (GPIO 2,3,18-22 reserved for WM8960/DigiAMP+)"
echo ""
