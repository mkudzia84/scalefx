#!/bin/bash
#
# Enable SSH over USB for Raspberry Pi
# Configures the Pi as a USB gadget for direct SSH access via USB-C
#
# Usage: sudo ./setup-usb-ssh.sh
#
# Compatible with: Pi Zero 2, Pi 4, Pi 5
# After running: Connect Pi to computer via USB-C, then:
#   ssh pi@raspberrypi.local  (or ssh pi@10.55.0.1 if .local doesn't resolve)

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Raspberry Pi USB SSH Setup${NC}"
echo -e "${GREEN}================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Detect Pi model
PI_MODEL=$(cat /proc/device-tree/model 2>/dev/null || echo "Unknown")
echo -e "${YELLOW}Pi Model: $PI_MODEL${NC}"
echo ""

# Check for USB gadget support
if ! grep -q "CONFIG_USB_GADGET" /boot/config.txt 2>/dev/null; then
    echo -e "${YELLOW}Configuring USB gadget mode...${NC}"
fi

# Step 1: Enable dwc2 overlay in /boot/config.txt
echo -e "${YELLOW}Step 1: Enabling dwc2 USB gadget overlay...${NC}"
BOOT_CONFIG="/boot/config.txt"

if ! grep -q "^dtoverlay=dwc2" "$BOOT_CONFIG"; then
    echo "dtoverlay=dwc2" >> "$BOOT_CONFIG"
    echo -e "${GREEN}  dwc2 overlay enabled${NC}"
else
    echo -e "${GREEN}  dwc2 overlay already enabled${NC}"
fi
echo ""

# Step 2: Enable g_ether module in /boot/cmdline.txt
echo -e "${YELLOW}Step 2: Enabling g_ether module...${NC}"
CMDLINE="/boot/cmdline.txt"

# Backup original cmdline
if [ ! -f "$CMDLINE.bak" ]; then
    cp "$CMDLINE" "$CMDLINE.bak"
    echo -e "${GREEN}  Backup created: $CMDLINE.bak${NC}"
fi

# Add g_ether to modules
if ! grep -q "modules-load=dwc2,g_ether" "$CMDLINE"; then
    # Read current cmdline and append module loading
    CURRENT=$(cat "$CMDLINE")
    echo "$CURRENT modules-load=dwc2,g_ether" > "$CMDLINE"
    echo -e "${GREEN}  g_ether module enabled${NC}"
else
    echo -e "${GREEN}  g_ether module already enabled${NC}"
fi
echo ""

# Step 3: Create systemd service to configure USB gadget on boot
echo -e "${YELLOW}Step 3: Creating USB gadget systemd service...${NC}"

cat > /etc/systemd/system/usb-gadget-ssh.service << 'EOF'
[Unit]
Description=USB Gadget SSH (g_ether)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/usb-gadget-setup.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

echo -e "${GREEN}  Service created${NC}"
echo ""

# Step 4: Create the gadget setup script
echo -e "${YELLOW}Step 4: Creating gadget configuration script...${NC}"

cat > /usr/local/bin/usb-gadget-setup.sh << 'EOF'
#!/bin/bash
# Configure USB gadget on boot

GADGET_PATH="/sys/kernel/config/usb_gadget"
GADGET_NAME="helifx"

# Skip if already configured
if [ -d "$GADGET_PATH/$GADGET_NAME" ]; then
    exit 0
fi

# Wait for configfs to be ready
sleep 1

# Create gadget directory
mkdir -p "$GADGET_PATH/$GADGET_NAME"
cd "$GADGET_PATH/$GADGET_NAME"

# Set USB device parameters
echo 0x1d6b > idVendor    # Linux Foundation
echo 0x0107 > idProduct   # Ethernet Gadget (g_ether)
echo 0x0100 > bcdDevice   # Device release

# Create device descriptor
mkdir -p strings/0x409
echo "0123456789ABCDEF" > strings/0x409/serialnumber
echo "Raspberry Pi" > strings/0x409/manufacturer
echo "HeliFX over USB" > strings/0x409/product

# Create configuration
mkdir -p configs/c.1/strings/0x409
echo "Ethernet" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower

# Create function
mkdir -p functions/ecm.usb0
echo "48:6f:73:74:50:43" > functions/ecm.usb0/host_addr
echo "42:61:64:55:53:42" > functions/ecm.usb0/dev_addr

# Link function to configuration (avoid duplicate links)
if [ ! -e configs/c.1/ecm.usb0 ]; then
    ln -sf functions/ecm.usb0 configs/c.1/
fi

# Bind to UDC (find the first available UDC), handle busy device
UDC=$(ls /sys/class/udc)
if [ -n "$UDC" ]; then
    # If already bound, skip
    CURRENT_UDC=$(cat UDC 2>/dev/null || echo "")
    if [ -n "$CURRENT_UDC" ]; then
        exit 0
    fi
    # Try to bind, retry if busy
    for i in 1 2 3; do
        if echo "$UDC" > UDC 2>/dev/null; then
            exit 0
        fi
        sleep 1
    done
    # As a last resort, attempt to unbind any existing gadget and retry
    for G in $(ls "$GADGET_PATH" | grep -v "$GADGET_NAME"); do
        if [ -f "$GADGET_PATH/$G/UDC" ]; then
            echo "" > "$GADGET_PATH/$G/UDC" 2>/dev/null || true
        fi
    done
    echo "$UDC" > UDC 2>/dev/null || true
fi

# Bring up usb0 interface with a static IP
/usr/bin/udevadm settle || true
sleep 1
if ip link show usb0 >/dev/null 2>&1; then
    ip link set usb0 up || true
    # Assign static IP if not present
    if ! ip -4 addr show usb0 | grep -q '10.55.0.1'; then
        ip addr add 10.55.0.1/24 dev usb0 || true
    fi
fi

exit 0
EOF

chmod +x /usr/local/bin/usb-gadget-setup.sh
echo -e "${GREEN}  Script created${NC}"
echo ""

# Step 5: Enable the service
echo -e "${YELLOW}Step 5: Enabling USB gadget service...${NC}"
systemctl daemon-reload
systemctl enable usb-gadget-ssh.service
echo -e "${GREEN}  Service enabled${NC}"
echo ""

# Step 6: Configure SSH to listen on USB interface
echo -e "${YELLOW}Step 6: Configuring SSH...${NC}"

# SSH already listens on all interfaces by default
if grep -q "^ListenAddress" /etc/ssh/sshd_config; then
    echo -e "${GREEN}  SSH already has custom listen address${NC}"
else
    echo -e "${GREEN}  SSH will listen on all interfaces (including USB)${NC}"
fi
echo ""

echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Setup Complete!${NC}"
echo -e "${GREEN}================================${NC}"
echo ""
echo -e "${RED}⚠️  REBOOT REQUIRED  ⚠️${NC}"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Reboot the Raspberry Pi:"
echo "     sudo reboot"
echo ""
echo "  2. After reboot, connect Pi to your computer via USB-C cable"
echo ""
echo "  3. On your computer, find the Pi's USB IP address:"
echo "     - Linux:   ip addr show"
echo "     - macOS:   ifconfig | grep 10.55"
echo "     - Windows: ipconfig"
echo ""
echo "  4. Connect via SSH:"
echo "     ssh pi@raspberrypi.local"
echo "     or"
echo "     ssh pi@10.55.0.1"
echo ""
echo -e "${YELLOW}Troubleshooting:${NC}"
echo "  - If SSH connection times out, check that g_ether module loaded:"
echo "    lsmod | grep g_ether"
echo ""
echo "  - View gadget status:"
echo "    systemctl status usb-gadget-ssh.service"
echo ""
echo "  - Check USB connection on computer:"
echo "    lsusb | grep Raspberry"
echo ""
echo "  - Manually reload gadget:"
echo "    sudo systemctl restart usb-gadget-ssh.service"
echo ""
echo -e "${YELLOW}Notes:${NC}"
echo "  - USB SSH uses IP 10.55.0.1 by default"
echo "  - Hostname remains raspberrypi.local"
echo "  - You can also use regular network SSH simultaneously"
echo "  - USB gadget provides Ethernet over USB (CDC Ethernet)"
echo ""
