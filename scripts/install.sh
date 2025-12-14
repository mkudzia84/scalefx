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

# ============================================================================
# AUDIO SETUP INSTALLATION
# ============================================================================
echo ""
echo -e "${GREEN}[Step 1/2] Audio Setup Installation${NC}"
echo -e "${YELLOW}Installing audio level setup script...${NC}"

if [ ! -f "./scripts/setup-audio-levels.sh" ]; then
    echo -e "${RED}Error: setup-audio-levels.sh not found${NC}"
    exit 1
fi

cp ./scripts/setup-audio-levels.sh /usr/local/bin/helifx-audio-setup
chmod +x /usr/local/bin/helifx-audio-setup
echo -e "${GREEN}  ✓ Audio setup script installed to /usr/local/bin/helifx-audio-setup${NC}"

echo ""
echo -e "${YELLOW}Installing audio systemd service...${NC}"
if [ ! -f "./scripts/helifx-audio.service" ]; then
    echo -e "${RED}Error: helifx-audio.service not found${NC}"
    exit 1
fi

cp ./scripts/helifx-audio.service /etc/systemd/system/
echo -e "${GREEN}  ✓ Audio service installed to /etc/systemd/system/helifx-audio.service${NC}"

echo ""
echo -e "${YELLOW}Configure audio service auto-start?${NC}"
echo "  This service sets audio levels at boot time"
read -p "Enable helifx-audio.service on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable helifx-audio.service
    echo -e "${GREEN}  ✓ helifx-audio.service enabled (will run at boot)${NC}"
else
    systemctl disable helifx-audio.service 2>/dev/null || true
    echo -e "${YELLOW}  ⚠ helifx-audio.service disabled (manual setup required)${NC}"
fi

echo ""
echo -e "${YELLOW}Run audio setup now?${NC}"
read -p "Configure audio levels now? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    /usr/local/bin/helifx-audio-setup --verbose || echo -e "${YELLOW}  ⚠ Audio setup completed with warnings${NC}"
else
    echo -e "${YELLOW}  ⚠ Skipped. Run manually: sudo helifx-audio-setup --verbose${NC}"
fi

# ============================================================================
# HELIFX SERVICE INSTALLATION
# ============================================================================
echo ""
echo -e "${GREEN}[Step 2/2] HeliFX Service Installation${NC}"
echo -e "${YELLOW}Installing helifx systemd service...${NC}"

if [ ! -f "./scripts/helifx.service" ]; then
    echo -e "${RED}Error: helifx.service template not found${NC}"
    exit 1
fi

# Get user UID and home directory for service configuration
USER_UID=$(id -u $USER)
USER_HOME=$(eval echo ~$USER)

echo -e "${YELLOW}  Configuring service with:${NC}"
echo "    User: $USER (UID: $USER_UID)"
echo "    Home: $USER_HOME"
echo "    Working Directory: $INSTALL_DIR"
echo "    Config Path: $CONFIG_PATH"

# Generate service file from template
sed -e "s|{{USER}}|$USER|g" \
    -e "s|{{USER_HOME}}|$USER_HOME|g" \
    -e "s|{{USER_UID}}|$USER_UID|g" \
    -e "s|{{INSTALL_DIR}}|$INSTALL_DIR|g" \
    -e "s|{{CONFIG_PATH}}|$CONFIG_PATH|g" \
    "./scripts/helifx.service" > /etc/systemd/system/$SERVICE_NAME

systemctl daemon-reload
echo -e "${GREEN}  ✓ helifx.service installed to /etc/systemd/system/$SERVICE_NAME${NC}"

echo ""
echo -e "${YELLOW}Configure helifx service auto-start?${NC}"
echo "  This starts the helicopter FX system at boot"
read -p "Enable $SERVICE_NAME on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable $SERVICE_NAME
    echo -e "${GREEN}  ✓ $SERVICE_NAME enabled (will start at boot)${NC}"
else
    systemctl disable $SERVICE_NAME 2>/dev/null || true
    echo -e "${YELLOW}  ⚠ $SERVICE_NAME disabled (manual start required)${NC}"
fi

echo ""
echo -e "${YELLOW}Start helifx service now?${NC}"
read -p "Start $SERVICE_NAME? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}Starting service...${NC}"
    systemctl start $SERVICE_NAME
    sleep 2
    echo ""
    systemctl status $SERVICE_NAME --no-pager --lines=10
else
    echo -e "${YELLOW}  ⚠ Service not started. Start manually: sudo systemctl start $SERVICE_NAME${NC}"
fi


# ============================================================================
# INSTALLATION SUMMARY
# ============================================================================
echo ""
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}   Installation Complete!${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo -e "${YELLOW}Installation Summary:${NC}"
echo "  User:             $USER"
echo "  Install Dir:      $INSTALL_DIR"
echo "  Config File:      $CONFIG_PATH"
echo "  Executable:       $INSTALL_DIR/helifx"
echo ""

# Show service status
AUDIO_ENABLED=$(systemctl is-enabled helifx-audio.service 2>/dev/null || echo "disabled")
HELIFX_ENABLED=$(systemctl is-enabled $SERVICE_NAME 2>/dev/null || echo "disabled")
HELIFX_ACTIVE=$(systemctl is-active $SERVICE_NAME 2>/dev/null || echo "inactive")

echo -e "${YELLOW}Service Status:${NC}"
echo "  helifx-audio.service:  $AUDIO_ENABLED"
echo "  $SERVICE_NAME:     $HELIFX_ENABLED ($HELIFX_ACTIVE)"
echo ""

echo -e "${YELLOW}Useful Commands:${NC}"
echo ""
echo -e "${GREEN}Audio Setup:${NC}"
echo "  sudo helifx-audio-setup --verbose    # Configure audio levels"
echo "  sudo systemctl start helifx-audio    # Run audio setup service"
echo "  sudo systemctl enable helifx-audio   # Enable audio setup at boot"
echo "  sudo systemctl disable helifx-audio  # Disable audio setup at boot"
echo ""
echo -e "${GREEN}HeliFX Service:${NC}"
echo "  sudo systemctl start $SERVICE_NAME      # Start the service"
echo "  sudo systemctl stop $SERVICE_NAME       # Stop the service"
echo "  sudo systemctl restart $SERVICE_NAME    # Restart the service"
echo "  sudo systemctl status $SERVICE_NAME     # Check service status"
echo "  sudo systemctl enable $SERVICE_NAME     # Enable auto-start at boot"
echo "  sudo systemctl disable $SERVICE_NAME    # Disable auto-start at boot"
echo "  sudo journalctl -u $SERVICE_NAME -f    # View live logs"
echo ""
echo -e "${GREEN}Configuration:${NC}"
echo "  nano $CONFIG_PATH                       # Edit configuration"
echo "  sudo systemctl restart $SERVICE_NAME    # Apply config changes"
echo ""
echo -e "${YELLOW}Important Notes:${NC}"
echo "  • Audio HAT pins (GPIO 2,3,18-22) are protected by software"
echo "  • Run as sudo for GPIO access or add user to 'gpio' group"
echo "  • Configuration changes require service restart"
echo "  • Check logs if service fails to start: journalctl -u $SERVICE_NAME"
echo ""
