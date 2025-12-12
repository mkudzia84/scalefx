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

# Install dependencies
echo -e "${YELLOW}Installing dependencies...${NC}"
apt-get update
apt-get install -y \
    build-essential \
    libyaml-dev \
    libcyaml-dev \
    libasound2-dev \
    pigpio

echo -e "${GREEN}All dependencies installed${NC}"
echo ""

# Configure pigpiod daemon with WM8960 Audio HAT pin exclusions
echo -e "${YELLOW}Configuring pigpiod daemon...${NC}"

# Create systemd service override with pin exclusion mask
# Mask 0x3C000C = bits 2,3,18,19,20,21 (GPIO 2,3 for I2C, GPIO 18-21 for I2S)
mkdir -p /etc/systemd/system/pigpiod.service.d
cat > /etc/systemd/system/pigpiod.service.d/override.conf << EOF
[Service]
ExecStart=
ExecStart=/usr/bin/pigpiod -l -x 0x3C000C
EOF

# Enable and start pigpiod daemon
systemctl daemon-reload
systemctl enable pigpiod
systemctl restart pigpiod

if systemctl is-active --quiet pigpiod; then
    echo -e "${GREEN}pigpiod daemon configured and running${NC}"
    echo -e "${GREEN}WM8960 Audio HAT pins excluded (GPIO 2,3 for I2C, 18-21 for I2S)${NC}"
else
    echo -e "${RED}Error: pigpiod daemon failed to start${NC}"
    echo -e "${YELLOW}Check logs with: sudo journalctl -u pigpiod -n 20${NC}"
    exit 1
fi
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

# Install systemd service
echo -e "${YELLOW}Installing systemd service...${NC}"

# Update service file with user-specific paths
sed -e "s|User=helisfx|User=$USER|g" \
    -e "s|Group=helisfx|Group=$USER|g" \
    -e "s|WorkingDirectory=/home/helisfx/helifx|WorkingDirectory=$INSTALL_DIR|g" \
    -e "s|ExecStart=/home/helisfx/helifx/helifx /home/helisfx/helifx/config.yaml|ExecStart=$INSTALL_DIR/helifx $INSTALL_DIR/config.yaml|g" \
    -e "s|Environment=\"HOME=/home/helisfx\"|Environment=\"HOME=/home/$USER\"|g" \
    "./scripts/helifx.service" > /etc/systemd/system/$SERVICE_NAME

systemctl daemon-reload
echo -e "${GREEN}Service installed${NC}"
echo ""

# Service configuration
echo -e "${YELLOW}Service configuration:${NC}"
read -p "Enable auto-start on boot? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable $SERVICE_NAME
    echo -e "${GREEN}  ✓ Auto-start enabled${NC}"
fi

read -p "Start service now? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl start $SERVICE_NAME
    sleep 1
    systemctl status $SERVICE_NAME --no-pager
fi

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "${YELLOW}Installation directory:${NC} $INSTALL_DIR"
echo -e "${YELLOW}Service commands:${NC}"
echo "  sudo systemctl start $SERVICE_NAME"
echo "  sudo systemctl stop $SERVICE_NAME"
echo "  sudo systemctl restart $SERVICE_NAME"
echo "  sudo systemctl status $SERVICE_NAME"
echo "  sudo journalctl -u $SERVICE_NAME -f"
echo ""
echo -e "${GREEN}Next steps:${NC}"
echo "  1. Copy sound files to $INSTALL_DIR/assets/"
echo "  2. Edit $INSTALL_DIR/config.yaml if needed"
echo "  3. Start the service: sudo systemctl start $SERVICE_NAME"
echo ""
echo -e "${YELLOW}Note:${NC} pigpio is configured to avoid WM8960 Audio HAT pins"
echo "      (GPIO 2,3,18,19,20,21 are excluded from GPIO control)"
echo ""
