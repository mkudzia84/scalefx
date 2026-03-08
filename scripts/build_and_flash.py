#!/usr/bin/env python3
"""
ScaleFX Build and Flash Script

Build and flash firmware for Pico controllers using binary COBS protocol.
Supports automatic BOOTSEL entry via binary protocol command.

Usage:
    python scripts/build_and_flash.py <controller> [options]
    
Controllers:
    noop, gunfx, lightfx, gearcontrol, hubfx

Options:
    --port PORT     Serial port (default: auto-detect)
    --no-build      Skip build step (flash existing firmware)
    --no-clean      Skip clean step (incremental build)
    --skip-verify   Skip post-flash verification
    --timeout SEC   BOOTSEL wait timeout (default: 15)

Examples:
    python scripts/build_and_flash.py gunfx
    python scripts/build_and_flash.py lightfx --port COM10
    python scripts/build_and_flash.py noop --no-build

Protocol:
    Uses binary COBS protocol (not text):
    - INIT (0xF0): Initialize connection
    - BOOTSEL (0xF9): Enter BOOTSEL mode for flashing
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple, List

# Add parent for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

# Import protocol from test framework
try:
    from tests.framework.protocol import build_packet, parse_packet, cobs_decode, read_u32_le
    from tests.framework.packets import CorePacket
    PROTOCOL_AVAILABLE = True
except ImportError:
    PROTOCOL_AVAILABLE = False


# =============================================================================
# Constants
# =============================================================================

CONTROLLERS = ['noop', 'gunfx', 'lightfx', 'gearcontrol', 'hubfx']
BAUD_RATE = 1000000
FRAME_DELIMITER = 0x00


# =============================================================================
# Terminal Colors
# =============================================================================

class Colors:
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    GRAY = '\033[90m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def print_header(text: str):
    print()
    print(f"{Colors.CYAN}{'=' * 50}{Colors.RESET}")
    print(f"{Colors.CYAN}  {text}{Colors.RESET}")
    print(f"{Colors.CYAN}{'=' * 50}{Colors.RESET}")


def print_step(step: int, total: int, message: str):
    print()
    print(f"{Colors.YELLOW}[{step}/{total}] {message}{Colors.RESET}")


def print_ok(message: str):
    print(f"    {Colors.GREEN}[OK]{Colors.RESET} {message}")


def print_err(message: str):
    print(f"    {Colors.RED}[FAIL]{Colors.RESET} {message}")


def print_info(message: str):
    print(f"    {Colors.GRAY}{message}{Colors.RESET}")


def print_warn(message: str):
    print(f"    {Colors.YELLOW}[WARN]{Colors.RESET} {message}")


# =============================================================================
# Build Info
# =============================================================================

@dataclass
class BuildInfo:
    firmware_path: Path
    firmware_size: int
    firmware_hash: str
    version: str
    build_number: int
    flash_used: str = ""
    ram_used: str = ""


# =============================================================================
# Path Helpers
# =============================================================================

def get_workspace_root() -> Path:
    return Path(__file__).parent.parent.resolve()


def get_controller_path(controller: str) -> Path:
    return get_workspace_root() / "controllers" / controller / "pico"


def get_firmware_path(controller: str) -> Path:
    return get_controller_path(controller) / ".pio" / "build" / "pico" / "firmware.uf2"


# =============================================================================
# Serial Port Detection
# =============================================================================

def find_pico_port() -> Optional[str]:
    """Find connected Pico serial port."""
    if not SERIAL_AVAILABLE:
        return None
    
    for port in serial.tools.list_ports.comports():
        # RP2040 CDC: VID=2E8A (Raspberry Pi), PID=000A or similar
        if port.vid == 0x2E8A:
            return port.device
        # Common descriptions
        desc_lower = (port.description or "").lower()
        if "pico" in desc_lower or "rp2040" in desc_lower:
            return port.device
    
    return None


def find_bootsel_drive() -> Optional[Path]:
    """Find the RPI-RP2 mass storage drive."""
    if sys.platform == "win32":
        import ctypes
        drives = []
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        for letter in 'DEFGHIJKLMNOPQRSTUVWXYZ':
            if bitmask & 1:
                drive_path = Path(f"{letter}:")
                info_file = drive_path / "INFO_UF2.TXT"
                if info_file.exists():
                    try:
                        content = info_file.read_text()
                        if "RPI-RP2" in content or "RP2040" in content:
                            return drive_path
                    except:
                        pass
            bitmask >>= 1
    else:
        # Linux/Mac: check /media, /mnt, /Volumes
        for base in ["/media", "/mnt", "/Volumes"]:
            base_path = Path(base)
            if base_path.exists():
                for mount in base_path.iterdir():
                    if mount.is_dir():
                        info = mount / "INFO_UF2.TXT"
                        if info.exists():
                            try:
                                content = info.read_text()
                                if "RPI-RP2" in content:
                                    return mount
                            except:
                                pass
    return None


# =============================================================================
# Build Functions
# =============================================================================

def increment_build_number(controller_path: Path) -> Optional[int]:
    """Increment build number in source file."""
    # Search version.h first, then .ino source files
    candidates = [
        controller_path / "src" / "version.h",
        controller_path / "include" / "version.h",
    ]
    src_dir = controller_path / "src"
    if src_dir.exists():
        candidates.extend(sorted(src_dir.glob("*.ino")))
    
    for source_file in candidates:
        if not source_file.exists():
            continue
        content = source_file.read_text(encoding='utf-8', errors='replace')
        match = re.search(r'#define\s+BUILD_NUMBER\s+(\d+)', content)
        if match:
            old_num = int(match.group(1))
            new_num = old_num + 1
            new_content = re.sub(
                r'#define\s+BUILD_NUMBER\s+\d+',
                f'#define BUILD_NUMBER {new_num}',
                content
            )
            source_file.write_text(new_content, encoding='utf-8')
            return new_num
    return None


def extract_version(controller_path: Path) -> Tuple[str, int]:
    """Extract version and build number from source."""
    version = "0.0.0"
    build = 0
    
    # Search version.h first, then .ino source files
    candidates = [
        controller_path / "src" / "version.h",
        controller_path / "include" / "version.h",
    ]
    # Add all .ino files in src/
    src_dir = controller_path / "src"
    if src_dir.exists():
        candidates.extend(sorted(src_dir.glob("*.ino")))
    
    for source_file in candidates:
        if not source_file.exists():
            continue
        content = source_file.read_text(encoding='utf-8', errors='replace')
        
        # Look for VERSION define
        match = re.search(r'#define\s+(?:FIRMWARE_)?VERSION\s+"([^"]+)"', content)
        if match:
            version = match.group(1)
        
        # Look for BUILD_NUMBER
        match = re.search(r'#define\s+BUILD_NUMBER\s+(\d+)', content)
        if match:
            build = int(match.group(1))
        
        # Stop once we find at least a version
        if version != "0.0.0":
            break
    
    return version, build


def build_firmware(controller: str, clean: bool = True) -> Optional[BuildInfo]:
    """Build firmware using PlatformIO."""
    controller_path = get_controller_path(controller)
    firmware_path = get_firmware_path(controller)
    
    if not controller_path.exists():
        print_err(f"Controller path not found: {controller_path}")
        return None
    
    # Increment build number
    build_num = increment_build_number(controller_path)
    if build_num:
        print_info(f"Build number incremented to {build_num}")
    
    # Clean if requested
    if clean:
        print_info("Cleaning previous build...")
        result = subprocess.run(
            [sys.executable, "-m", "platformio", "run", "-t", "clean"],
            cwd=controller_path,
            capture_output=True,
            text=True
        )
    
    # Build
    result = subprocess.run(
        [sys.executable, "-m", "platformio", "run"],
        cwd=controller_path,
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print_err("Build failed")
        print(result.stderr)
        return None
    
    # Extract size info from output
    flash_used = ""
    ram_used = ""
    for line in result.stdout.split('\n'):
        if "Flash:" in line:
            flash_used = line.strip()
        if "RAM:" in line:
            ram_used = line.strip()
    
    if not firmware_path.exists():
        print_err(f"Firmware not found: {firmware_path}")
        return None
    
    # Get firmware info
    firmware_size = firmware_path.stat().st_size
    firmware_hash = hashlib.md5(firmware_path.read_bytes()).hexdigest().upper()
    version, build = extract_version(controller_path)
    
    print_ok("Build complete")
    if flash_used:
        print_info(flash_used)
    if ram_used:
        print_info(ram_used)
    
    return BuildInfo(
        firmware_path=firmware_path,
        firmware_size=firmware_size,
        firmware_hash=firmware_hash,
        version=version,
        build_number=build,
        flash_used=flash_used,
        ram_used=ram_used
    )


# =============================================================================
# Binary Protocol - BOOTSEL Entry
# =============================================================================

def send_bootsel_command(port: str) -> bool:
    """
    Send BOOTSEL command using binary COBS protocol.
    
    Protocol:
    1. Send INIT packet (0xF0) 
    2. Wait for INIT_READY (0xF3)
    3. Send BOOTSEL packet (0xF9)
    4. Device reboots into BOOTSEL mode
    """
    if not SERIAL_AVAILABLE:
        print_err("pyserial not installed")
        return False
    
    if not PROTOCOL_AVAILABLE:
        print_err("Protocol module not available")
        return False
    
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=2)
        time.sleep(0.1)  # Allow device to settle
        
        # Clear any buffered data
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        
        # Build and send INIT packet
        print_info("Sending INIT packet (binary)...")
        init_packet = build_packet(CorePacket.INIT)
        ser.write(init_packet)
        ser.flush()
        
        # Wait for INIT_READY response
        print_info("Waiting for INIT_READY...")
        start = time.time()
        rx_buffer = bytearray()
        got_init_ready = False
        
        while time.time() - start < 3.0:
            if ser.in_waiting:
                byte = ser.read(1)
                if byte[0] == FRAME_DELIMITER:
                    if len(rx_buffer) > 0:
                        # Try to parse packet
                        result = parse_packet(bytes(rx_buffer) + b'\x00')
                        if result:
                            ptype, _tag, payload = result
                            if ptype == CorePacket.INIT_READY:
                                print_ok("Got INIT_READY")
                                got_init_ready = True
                                break
                            elif ptype == CorePacket.ACK:
                                print_info("Got ACK (device was already initialized)")
                                got_init_ready = True
                                break
                        rx_buffer.clear()
                else:
                    rx_buffer.append(byte[0])
            else:
                time.sleep(0.01)
        
        if not got_init_ready:
            print_warn("No INIT_READY response, sending BOOTSEL anyway...")
        
        # Send BOOTSEL command
        print_info("Sending BOOTSEL packet...")
        bootsel_packet = build_packet(CorePacket.BOOTSEL)
        ser.write(bootsel_packet)
        ser.flush()
        time.sleep(0.1)
        
        ser.close()
        print_ok("BOOTSEL command sent")
        return True
        
    except serial.SerialException as e:
        print_err(f"Serial error: {e}")
        return False
    except Exception as e:
        print_err(f"Error: {e}")
        return False


def wait_for_bootsel_drive(timeout: int = 15) -> Optional[Path]:
    """Wait for RPI-RP2 drive to appear."""
    print_info("Waiting for RPI-RP2 drive...")
    start = time.time()
    
    while time.time() - start < timeout:
        drive = find_bootsel_drive()
        if drive:
            print_ok(f"Found RPI-RP2 at {drive}")
            return drive
        sys.stdout.write('.')
        sys.stdout.flush()
        time.sleep(0.5)
    
    print()
    print_err(f"RPI-RP2 drive not found within {timeout}s")
    print_info("Try: Hold BOOTSEL button and plug in USB")
    return None


# =============================================================================
# Flash Functions
# =============================================================================

def flash_firmware(firmware_path: Path, drive: Path) -> bool:
    """Copy firmware to RPI-RP2 drive."""
    try:
        dest = drive / firmware_path.name
        print_info(f"Copying {firmware_path.name} to {drive}...")
        shutil.copy2(firmware_path, dest)
        
        # Wait for copy to complete and device to reboot
        time.sleep(2)
        
        # Drive should disappear after successful flash
        if not drive.exists():
            print_ok("Firmware flashed successfully")
            return True
        else:
            print_warn("Drive still present - flash may have failed")
            return True  # Might still be OK
            
    except Exception as e:
        print_err(f"Flash failed: {e}")
        return False


def _parse_init_ready_payload(payload: bytes) -> Optional[Tuple[str, str, str, int]]:
    """
    Parse INIT_READY binary payload.
    
    Format: [nameLen:u8][name][verLen:u8][version][platLen:u8][platform]
            [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
    
    Returns (name, version, platform, build_number) or None on parse error.
    """
    if not payload or len(payload) < 3:
        return None
    
    try:
        offset = 0
        
        # Device name (length-prefixed)
        name_len = payload[offset]
        offset += 1
        name = payload[offset:offset + name_len].decode('utf-8', errors='replace')
        offset += name_len
        
        # Firmware version (length-prefixed)
        ver_len = payload[offset]
        offset += 1
        version = payload[offset:offset + ver_len].decode('utf-8', errors='replace')
        offset += ver_len
        
        # Platform (length-prefixed)
        plat_len = payload[offset]
        offset += 1
        platform = payload[offset:offset + plat_len].decode('utf-8', errors='replace')
        offset += plat_len
        
        # CPU MHz (u32LE) - skip
        offset += 4
        
        # Free RAM (u32LE) - skip
        offset += 4
        
        # Build number (u32LE)
        build_num = read_u32_le(payload, offset) if offset + 4 <= len(payload) else 0
        
        return (name, version, platform, build_num)
    
    except (IndexError, KeyError):
        return None


def verify_flash(port: str, expected_version: str = None) -> bool:
    """Verify device is running after flash."""
    if not SERIAL_AVAILABLE or not PROTOCOL_AVAILABLE:
        print_warn("Cannot verify - serial or protocol not available")
        return True
    
    print_info("Waiting for device to reboot...")
    time.sleep(3)
    
    # Try to find the port again
    new_port = port
    for _ in range(5):
        new_port = find_pico_port()
        if new_port:
            break
        time.sleep(1)
    
    if not new_port:
        print_warn("Device not found after flash")
        return False
    
    try:
        ser = serial.Serial(new_port, BAUD_RATE, timeout=2)
        time.sleep(0.1)
        ser.reset_input_buffer()
        
        # Send INIT
        init_packet = build_packet(CorePacket.INIT)
        ser.write(init_packet)
        ser.flush()
        
        # Wait for response
        start = time.time()
        rx_buffer = bytearray()
        
        while time.time() - start < 3.0:
            if ser.in_waiting:
                byte = ser.read(1)
                if byte[0] == FRAME_DELIMITER:
                    if len(rx_buffer) > 0:
                        result = parse_packet(bytes(rx_buffer) + b'\x00')
                        if result:
                            ptype, _tag, payload = result
                            if ptype == CorePacket.INIT_READY:
                                # Parse binary INIT_READY payload
                                device_info = _parse_init_ready_payload(payload)
                                if device_info:
                                    name, ver, platform, build_num = device_info
                                    print_ok(f"Device responding: {name} v{ver} ({platform}, build {build_num})")
                                else:
                                    print_ok("Device responding")
                                ser.close()
                                return True
                        rx_buffer.clear()
                else:
                    rx_buffer.append(byte[0])
            else:
                time.sleep(0.01)
        
        ser.close()
        print_warn("No INIT_READY response during verification")
        return False
        
    except Exception as e:
        print_err(f"Verification failed: {e}")
        return False


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Build and flash ScaleFX Pico firmware"
    )
    parser.add_argument(
        "controller",
        choices=CONTROLLERS,
        help="Controller to build/flash"
    )
    parser.add_argument(
        "--port",
        help="Serial port (default: auto-detect)"
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Skip build step"
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Skip clean step (incremental build)"
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip post-flash verification"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=15,
        help="BOOTSEL wait timeout in seconds"
    )
    
    args = parser.parse_args()
    
    # Header
    print_header(f"ScaleFX {args.controller.upper()} - Build and Flash")
    
    # Validate dependencies
    if not SERIAL_AVAILABLE:
        print_err("pyserial not installed: pip install pyserial")
        return 1
    
    if not PROTOCOL_AVAILABLE:
        print_err("Protocol module not available")
        return 1
    
    # Determine steps
    steps = []
    if not args.no_build:
        steps.append("build")
    steps.append("verify_fw")
    steps.append("bootsel")
    steps.append("flash")
    if not args.skip_verify:
        steps.append("verify_device")
    
    total_steps = len(steps)
    current_step = 0
    
    # Step 1: Build firmware
    firmware_path = get_firmware_path(args.controller)
    build_info = None
    
    if "build" in steps:
        current_step += 1
        print_step(current_step, total_steps, "Building firmware...")
        build_info = build_firmware(args.controller, clean=not args.no_clean)
        if not build_info:
            return 1
    
    # Step 2: Verify firmware exists
    current_step += 1
    print_step(current_step, total_steps, "Verifying firmware...")
    
    if not firmware_path.exists():
        print_err(f"Firmware not found: {firmware_path}")
        print_info("Run without --no-build to build first")
        return 1
    
    firmware_size = firmware_path.stat().st_size
    firmware_hash = hashlib.md5(firmware_path.read_bytes()).hexdigest().upper()
    
    if not build_info:
        version, build_num = extract_version(get_controller_path(args.controller))
        build_info = BuildInfo(
            firmware_path=firmware_path,
            firmware_size=firmware_size,
            firmware_hash=firmware_hash,
            version=version,
            build_number=build_num
        )
    
    print_ok("Firmware verified")
    print_info(f"Size: {build_info.firmware_size:,} bytes")
    print_info(f"MD5:  {build_info.firmware_hash}")
    print_info(f"Version: {build_info.version} (Build {build_info.build_number})")
    
    # Step 3: Enter BOOTSEL mode
    current_step += 1
    print_step(current_step, total_steps, "Entering BOOTSEL mode...")
    
    # Check if already in BOOTSEL mode
    bootsel_drive = find_bootsel_drive()
    if bootsel_drive:
        print_ok(f"Already in BOOTSEL mode: {bootsel_drive}")
    else:
        # Find serial port
        port = args.port or find_pico_port()
        if port:
            print_info(f"Found device at {port}")
            send_bootsel_command(port)
        else:
            print_info("No serial port found")
            print_info("Waiting for manual BOOTSEL entry...")
        
        # Wait for BOOTSEL drive
        bootsel_drive = wait_for_bootsel_drive(args.timeout)
        if not bootsel_drive:
            return 1
    
    # Step 4: Flash firmware
    current_step += 1
    print_step(current_step, total_steps, "Flashing firmware...")
    
    if not flash_firmware(firmware_path, bootsel_drive):
        return 1
    
    # Step 5: Verify device
    if "verify_device" in steps:
        current_step += 1
        print_step(current_step, total_steps, "Verifying device...")
        
        port = args.port or find_pico_port()
        if verify_flash(port, build_info.version):
            print_ok("Device verified")
        else:
            print_warn("Verification incomplete")
    
    print()
    print(f"{Colors.GREEN}{'=' * 50}{Colors.RESET}")
    print(f"{Colors.GREEN}  Flash complete!{Colors.RESET}")
    print(f"{Colors.GREEN}{'=' * 50}{Colors.RESET}")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
