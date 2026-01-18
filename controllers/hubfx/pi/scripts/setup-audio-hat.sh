#!/bin/bash
#
# ScaleFX Audio HAT Setup Script
# Interactive menu to install and configure audio HAT drivers
#
# Usage: sudo ./setup-audio-hat.sh
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Function to display menu
show_menu() {
    clear
    echo -e "${GREEN}=====================================${NC}"
    echo -e "${GREEN}  ScaleFX Audio HAT Setup${NC}"
    echo -e "${GREEN}=====================================${NC}"
    echo ""
    echo -e "${BLUE}Select your audio HAT:${NC}"
    echo ""
    echo "  1) WM8960 Audio HAT (Waveshare/Generic)"
    echo "  2) Raspberry Pi DigiAMP+ (IQaudio)"
    echo "  3) Detect installed audio HAT"
    echo "  4) Test audio output"
    echo "  5) Configure audio levels"
    echo "  6) Show audio device information"
    echo "  7) Exit"
    echo ""
}

# Function to detect current audio HAT
detect_audio_hat() {
    echo -e "${YELLOW}Detecting audio HAT...${NC}"
    echo ""
    
    # Check device tree overlays
    if grep -q "^dtoverlay=wm8960-soundcard" /boot/config.txt 2>/dev/null; then
        echo -e "${GREEN}✓ WM8960 Audio HAT configured in device tree${NC}"
    fi
    
    if grep -q "^dtoverlay=iqaudio-dacplus" /boot/config.txt 2>/dev/null; then
        echo -e "${GREEN}✓ DigiAMP+ configured in device tree${NC}"
    fi
    
    echo ""
    echo -e "${YELLOW}Available audio devices:${NC}"
    aplay -l 2>/dev/null || echo "No audio devices found"
    
    echo ""
    echo -e "${YELLOW}I2C devices (codecs):${NC}"
    if command -v i2cdetect &> /dev/null; then
        i2cdetect -y 1 2>/dev/null || echo "I2C not available"
        echo ""
        echo "Expected codec addresses:"
        echo "  - WM8960: 0x1a"
        echo "  - DigiAMP+ (TAS5756): 0x4c"
    else
        echo "i2cdetect not installed"
    fi
    
    echo ""
    read -p "Press Enter to continue..."
}

# Function to test audio
test_audio() {
    echo -e "${YELLOW}Testing audio output...${NC}"
    echo ""
    echo -e "${RED}⚠️  Starting audio test at low volume${NC}"
    echo -e "${YELLOW}Press Ctrl+C to stop${NC}"
    echo ""
    sleep 2
    
    speaker-test -c 2 -t wav -l 3 || echo -e "${RED}Audio test failed${NC}"
    
    echo ""
    read -p "Press Enter to continue..."
}

# Function to show device info
show_device_info() {
    echo -e "${YELLOW}Audio Device Information${NC}"
    echo ""
    
    echo -e "${BLUE}=== Audio Cards ===${NC}"
    aplay -l 2>/dev/null || echo "No audio devices"
    
    echo ""
    echo -e "${BLUE}=== PCM Devices ===${NC}"
    aplay -L 2>/dev/null | head -20 || echo "No PCM devices"
    
    echo ""
    echo -e "${BLUE}=== Current Volume Levels ===${NC}"
    CARD=$(aplay -l 2>/dev/null | grep -m1 "card" | sed 's/card \([0-9]\).*/\1/' || echo "0")
    amixer -c "$CARD" 2>/dev/null || echo "Could not read mixer controls"
    
    echo ""
    echo -e "${BLUE}=== Kernel Modules ===${NC}"
    lsmod | grep -E "snd|i2c" | head -10
    
    echo ""
    read -p "Press Enter to continue..."
}

# Main menu loop
while true; do
    show_menu
    read -p "Enter your choice [1-7]: " choice
    
    case $choice in
        1)
            echo ""
            echo -e "${GREEN}Installing WM8960 Audio HAT driver...${NC}"
            echo ""
            if [ -f "./install-wm8960-driver.sh" ]; then
                ./install-wm8960-driver.sh
            else
                echo -e "${RED}Error: install-wm8960-driver.sh not found${NC}"
            fi
            echo ""
            read -p "Press Enter to continue..."
            ;;
        2)
            echo ""
            echo -e "${GREEN}Configuring DigiAMP+...${NC}"
            echo ""
            if [ -f "./install-digiamp-driver.sh" ]; then
                ./install-digiamp-driver.sh
            else
                echo -e "${RED}Error: install-digiamp-driver.sh not found${NC}"
            fi
            echo ""
            read -p "Press Enter to continue..."
            ;;
        3)
            detect_audio_hat
            ;;
        4)
            test_audio
            ;;
        5)
            echo ""
            if [ -f "/usr/local/bin/sfxhub-audio-setup" ]; then
                /usr/local/bin/sfxhub-audio-setup --verbose
            elif [ -f "./setup-audio-levels.sh" ]; then
                ./setup-audio-levels.sh --verbose
            else
                echo -e "${RED}Error: Audio setup script not found${NC}"
            fi
            echo ""
            read -p "Press Enter to continue..."
            ;;
        6)
            show_device_info
            ;;
        7)
            echo ""
            echo -e "${GREEN}Exiting...${NC}"
            exit 0
            ;;
        *)
            echo ""
            echo -e "${RED}Invalid option. Please try again.${NC}"
            sleep 2
            ;;
    esac
done
