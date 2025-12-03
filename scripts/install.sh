#!/bin/bash
#
# Installation script for Helicopter FX system
# Run with sudo: sudo ./install.sh [user]
#
# Arguments:
#   user - Username to install for (default: pi)
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

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Helicopter FX Installation Script${NC}"
echo -e "${GREEN}======================================${NC}"
echo -e "${YELLOW}Installing for user:${NC} $USER"
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo ""

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

echo -e "${YELLOW}Installing dependencies...${NC}"

# Update package lists
apt-get update

# Install required packages
apt-get install -y \
    build-essential \
    cmake \
    git \
    libyaml-dev \
    libasound2-dev \
    libsndfile1-dev \
    pigpio \
    python3-pigpio

# Enable and start pigpio daemon
systemctl enable pigpiod
systemctl start pigpiod

echo -e "${GREEN}Dependencies installed${NC}"
echo ""

# Create installation directory
echo -e "${YELLOW}Creating installation directory...${NC}"
mkdir -p "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR/assets"
mkdir -p "$INSTALL_DIR/logs"

# Copy files
echo -e "${YELLOW}Installing application files...${NC}"

# Check if we're running from the source directory
if [ -f "./build/helifx" ]; then
    cp ./build/helifx "$INSTALL_DIR/"
    chmod +x "$INSTALL_DIR/helifx"
    echo -e "${GREEN}  ✓ Binary copied${NC}"
else
    echo -e "${RED}Error: helifx binary not found. Please compile first with 'make'.${NC}"
    exit 1
fi

if [ -f "./config.yaml" ]; then
    # Only copy if not already exists (don't overwrite user config)
    if [ ! -f "$INSTALL_DIR/config.yaml" ]; then
        cp ./config.yaml "$INSTALL_DIR/"
        echo -e "${GREEN}  ✓ Configuration file copied${NC}"
    else
        echo -e "${YELLOW}  ⚠ Configuration file exists, skipping (backup at config.yaml.new)${NC}"
        cp ./config.yaml "$INSTALL_DIR/config.yaml.new"
    fi
else
    echo -e "${YELLOW}  ⚠ Warning: config.yaml not found${NC}"
fi

# Set ownership
chown -R $USER:$USER "$INSTALL_DIR"

echo -e "${GREEN}Application files installed${NC}"
echo ""

# Install systemd service
echo -e "${YELLOW}Installing systemd service...${NC}"

SERVICE_FILE=""
if [ -f "./scripts/helifx.service" ]; then
    SERVICE_FILE="./scripts/helifx.service"
elif [ -f "./helifx.service" ]; then
    # Support running from scripts directory
    SERVICE_FILE="./helifx.service"
else
    echo -e "${RED}Error: helifx.service not found${NC}"
    exit 1
fi

# Create temporary service file with correct user and paths
TEMP_SERVICE="/tmp/helifx.service.tmp"
sed -e "s|User=pi|User=$USER|g" \
    -e "s|Group=pi|Group=$USER|g" \
    -e "s|WorkingDirectory=/home/pi/helifx|WorkingDirectory=$INSTALL_DIR|g" \
    -e "s|ExecStart=/home/pi/helifx/helifx /home/pi/helifx/config.yaml|ExecStart=$INSTALL_DIR/helifx $INSTALL_DIR/config.yaml|g" \
    -e "s|Environment=\"HOME=/home/pi\"|Environment=\"HOME=/home/$USER\"|g" \
    "$SERVICE_FILE" > "$TEMP_SERVICE"

cp "$TEMP_SERVICE" /etc/systemd/system/$SERVICE_NAME
rm "$TEMP_SERVICE"
systemctl daemon-reload
echo -e "${GREEN}  ✓ Service file installed${NC}"

echo -e "${GREEN}Service installed${NC}"
echo ""

# Ask if user wants to enable auto-start
echo -e "${YELLOW}Configuration:${NC}"
read -p "Enable auto-start on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable $SERVICE_NAME
    echo -e "${GREEN}  ✓ Auto-start enabled${NC}"
else
    echo -e "${YELLOW}  ⚠ Auto-start not enabled${NC}"
fi

read -p "Start service now? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl start $SERVICE_NAME
    echo -e "${GREEN}  ✓ Service started${NC}"
    echo ""
    echo -e "${YELLOW}Service status:${NC}"
    systemctl status $SERVICE_NAME --no-pager
else
    echo -e "${YELLOW}  ⚠ Service not started${NC}"
fi

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo -e "${YELLOW}Configuration file:${NC} $INSTALL_DIR/config.yaml"
echo -e "${YELLOW}Log files:${NC} $INSTALL_DIR/logs/"
echo ""
echo -e "${YELLOW}Useful commands:${NC}"
echo "  Start service:   sudo systemctl start $SERVICE_NAME"
echo "  Stop service:    sudo systemctl stop $SERVICE_NAME"
echo "  Restart service: sudo systemctl restart $SERVICE_NAME"
echo "  View status:     sudo systemctl status $SERVICE_NAME"
echo "  View logs:       sudo journalctl -u $SERVICE_NAME -f"
echo "  Enable auto:     sudo systemctl enable $SERVICE_NAME"
echo "  Disable auto:    sudo systemctl disable $SERVICE_NAME"
echo ""
echo -e "${YELLOW}Manual run:${NC} cd $INSTALL_DIR && ./helifx config.yaml"
echo ""
echo -e "${GREEN}Don't forget to:${NC}"
echo "  1. Copy your sound files to $INSTALL_DIR/assets/"
echo "  2. Edit $INSTALL_DIR/config.yaml if needed"
echo "  3. Test with: sudo systemctl start $SERVICE_NAME"
echo ""
