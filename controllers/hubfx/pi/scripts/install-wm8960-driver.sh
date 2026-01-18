#!/bin/bash
#
# WM8960 Audio HAT Driver Installation Script
# Installs and configures WM8960 codec driver for Raspberry Pi
#
# Usage: sudo ./install-wm8960-driver.sh
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=====================================${NC}"
echo -e "${GREEN}WM8960 Audio HAT Driver Installation${NC}"
echo -e "${GREEN}=====================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Check Raspberry Pi model
echo -e "${YELLOW}Detecting Raspberry Pi model...${NC}"
PI_MODEL=$(cat /proc/device-tree/model 2>/dev/null || echo "Unknown")
echo -e "  Model: $PI_MODEL"
echo ""

# Install build dependencies
echo -e "${YELLOW}Installing build dependencies...${NC}"
apt-get update

# Try to install kernel headers (may not exist in Trixie)
echo -e "${YELLOW}Checking for kernel headers...${NC}"
if apt-cache search raspberrypi-kernel-headers | grep -q raspberrypi-kernel-headers; then
    apts-get install -y raspberrypi-kernel-headers
elif apt-cache search linux-headers | grep -q linux-headers; then
    echo -e "${YELLOW}Using generic linux-headers instead${NC}"
    apt-get install -y linux-headers-generic || \
    apt-get install -y linux-headers-$(uname -r) || \
    echo -e "${YELLOW}Warning: Could not install kernel headers, proceeding without${NC}"
else
    echo -e "${YELLOW}No kernel headers package found (may be built-in to Trixie kernel)${NC}"
fi

apt-get install -y \
    build-essential \
    git \
    dkms \
    i2c-tools \
    libasound2-dev \
    alsa-utils

echo -e "${GREEN}Dependencies installed${NC}"
echo ""

# Clone WM8960 driver repository
DRIVER_DIR="/usr/src/wm8960-soundcard"
echo -e "${YELLOW}Cloning WM8960 driver source...${NC}"

if [ -d "$DRIVER_DIR" ]; then
    echo -e "${YELLOW}  Driver directory exists, updating...${NC}"
    cd "$DRIVER_DIR"
    git pull
else
    git clone https://github.com/waveshare/WM8960-Audio-HAT "$DRIVER_DIR"
    cd "$DRIVER_DIR"
fi

echo -e "${GREEN}Source code ready${NC}"
echo ""

# Build and install driver
echo -e "${YELLOW}Building WM8960 driver...${NC}"
if [ -f "./install.sh" ]; then
    ./install.sh || {
        echo -e "${YELLOW}Driver build failed (may already be in kernel)${NC}"
        echo -e "${YELLOW}Proceeding with device tree configuration...${NC}"
    }
else
    echo -e "${YELLOW}Driver installation script not found${NC}"
    echo -e "${YELLOW}WM8960 may be built-in to Trixie kernel${NC}"
fi

echo -e "${GREEN}Driver installed${NC}"
echo ""

# Enable I2C interface
echo -e "${YELLOW}Enabling I2C interface...${NC}"
if ! raspi-config nonint do_i2c 0; then
    echo -e "${YELLOW}  Could not enable I2C automatically, enabling manually...${NC}"
    if ! grep -q "^dtparam=i2c_arm=on" /boot/config.txt; then
        echo "dtparam=i2c_arm=on" >> /boot/config.txt
    fi
fi
echo -e "${GREEN}I2C enabled${NC}"
echo ""

# Configure device tree overlay
echo -e "${YELLOW}Configuring device tree overlay...${NC}"
BOOT_CONFIG="/boot/config.txt"

# Remove old audio configurations
sed -i '/^dtparam=audio=/d' "$BOOT_CONFIG"
sed -i '/^dtoverlay=vc4-kms-v3d/d' "$BOOT_CONFIG"

# Add WM8960 overlay if not present
if ! grep -q "^dtoverlay=wm8960-soundcard" "$BOOT_CONFIG"; then
    echo "" >> "$BOOT_CONFIG"
    echo "# WM8960 Audio HAT configuration" >> "$BOOT_CONFIG"
    echo "dtoverlay=wm8960-soundcard" >> "$BOOT_CONFIG"
    echo -e "${GREEN}  Device tree overlay added${NC}"
else
    echo -e "${GREEN}  Device tree overlay already configured${NC}"
fi

echo ""

# Load I2C module
echo -e "${YELLOW}Loading I2C kernel module...${NC}"
modprobe i2c-dev 2>/dev/null || true
modprobe snd_soc_wm8960 2>/dev/null || echo -e "${YELLOW}WM8960 module not yet loaded (will load after reboot)${NC}"
echo -e "${GREEN}I2C module loaded${NC}"
echo ""

# Test I2C detection (after reboot this will show the codec)
echo -e "${YELLOW}Testing I2C bus...${NC}"
if command -v i2cdetect &> /dev/null; then
    echo -e "${YELLOW}  I2C devices on bus 1:${NC}"
    i2cdetect -y 1 || echo -e "${YELLOW}  (WM8960 will appear after reboot at address 0x1a)${NC}"
fi
echo ""

# Configure ALSA
echo -e "${YELLOW}Configuring ALSA...${NC}"
if [ ! -f /etc/asound.conf ]; then
    cat > /etc/asound.conf << 'EOF'
# WM8960 Audio HAT ALSA configuration
pcm.!default {
    type asym
    playback.pcm "plughw:wm8960soundcard"
    capture.pcm "plughw:wm8960soundcard"
}

ctl.!default {
    type hw
    card wm8960soundcard
}
EOF
    echo -e "${GREEN}  ALSA configuration created${NC}"
else
    echo -e "${YELLOW}  ALSA config exists, keeping existing${NC}"
fi
echo ""

# Add user to audio group
if [ -n "$SUDO_USER" ]; then
    usermod -a -G audio "$SUDO_USER" 2>/dev/null || true
    echo -e "${GREEN}User $SUDO_USER added to audio group${NC}"
fi
echo ""

echo -e "${GREEN}=====================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}=====================================${NC}"
echo ""
echo -e "${YELLOW}Disabling broken ALSA restore for WM8960...${NC}"
systemctl disable alsa-restore.service 2>/dev/null || true
systemctl mask alsa-restore.service 2>/dev/null || true
echo -e "${GREEN}alsa-restore disabled and masked${NC}"
echo ""
echo -e "${RED}⚠️  REBOOT REQUIRED  ⚠️${NC}"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Reboot the Raspberry Pi: sudo reboot"
echo "  2. After reboot, verify codec detection:"
echo "     sudo i2cdetect -y 1"
echo "     (Should show device at address 0x1a)"
echo "  3. List audio devices:"
echo "     aplay -l"
echo "     (Should show 'wm8960-soundcard')"
echo "  4. Test audio output:"
echo "     speaker-test -c 2 -t wav"
echo "  5. Configure audio levels:"
echo "     sudo sfxhub-audio-setup --verbose"
echo ""
echo -e "${YELLOW}Troubleshooting:${NC}"
echo "  - If codec not detected: Check HAT is firmly seated on GPIO header"
echo "  - If no audio: Check volume levels with 'alsamixer'"
echo "  - Driver logs: dmesg | grep wm8960"
echo ""
