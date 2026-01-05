#!/usr/bin/env python3
"""
HubFX Pico - Build and Flash Script

Performs verified build, automatic BOOTSEL entry via serial, and firmware upload
with version verification.

Usage:
    python build_and_flash.py [options]

Options:
    --no-build      Skip build step (use existing firmware)
    --no-clean      Skip clean step (incremental build)
    --skip-verify   Skip post-flash verification
    --port PORT     Serial port (default: auto-detect)
    --timeout SEC   BOOTSEL wait timeout (default: 15)
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
from typing import Optional, Tuple

# Conditional import for serial
try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


@dataclass
class BuildInfo:
    """Information extracted from build"""
    firmware_path: Path
    firmware_size: int
    firmware_hash: str
    version: str
    build_number: int
    flash_used: str = ""
    ram_used: str = ""


class Colors:
    """ANSI color codes for terminal output"""
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    GRAY = '\033[90m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def print_header(text: str):
    """Print a header banner"""
    print()
    print(f"{Colors.CYAN}{'=' * 44}{Colors.RESET}")
    print(f"{Colors.CYAN}  {text}{Colors.RESET}")
    print(f"{Colors.CYAN}{'=' * 44}{Colors.RESET}")


def print_step(step: int, total: int, message: str):
    """Print a step indicator"""
    print()
    print(f"{Colors.YELLOW}[{step}/{total}] {message}{Colors.RESET}")


def print_ok(message: str):
    """Print success message"""
    print(f"    {Colors.GREEN}[OK]{Colors.RESET} {message}")


def print_err(message: str):
    """Print error message"""
    print(f"    {Colors.RED}[FAIL]{Colors.RESET} {message}")


def print_info(message: str):
    """Print info message"""
    print(f"    {Colors.GRAY}{message}{Colors.RESET}")


def get_project_dir() -> Path:
    """Get the project directory (parent of scripts/)"""
    return Path(__file__).parent.parent.resolve()


def extract_version_from_source(project_dir: Path) -> Tuple[str, int]:
    """Extract firmware version and build number from source code"""
    source_file = project_dir / "src" / "hubfx_pico.ino"
    
    version = "unknown"
    build_number = 0
    
    if source_file.exists():
        content = source_file.read_text()
        
        # Extract version
        version_match = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', content)
        if version_match:
            version = version_match.group(1)
        
        # Extract build number
        build_match = re.search(r'#define\s+BUILD_NUMBER\s+(\d+)', content)
        if build_match:
            build_number = int(build_match.group(1))
    
    return version, build_number


def increment_build_number(project_dir: Path) -> int:
    """Increment the build number in source file and return new value"""
    source_file = project_dir / "src" / "hubfx_pico.ino"
    
    if not source_file.exists():
        return 0
    
    content = source_file.read_text()
    
    # Find and increment build number
    def increment_match(match):
        old_num = int(match.group(1))
        new_num = old_num + 1
        return f'#define BUILD_NUMBER {new_num}'
    
    new_content, count = re.subn(
        r'#define\s+BUILD_NUMBER\s+(\d+)',
        increment_match,
        content
    )
    
    if count > 0:
        source_file.write_text(new_content)
        # Re-extract to get the new number
        _, build_number = extract_version_from_source(project_dir)
        return build_number
    
    return 0


def run_clean(project_dir: Path) -> bool:
    """Run PlatformIO clean"""
    try:
        result = subprocess.run(
            [sys.executable, "-m", "platformio", "run", "--target", "clean", "-e", "pico"],
            cwd=project_dir,
            capture_output=True,
            text=True
        )
        return result.returncode == 0
    except Exception as e:
        print_info(f"Clean warning: {e}")
        return True  # Non-fatal


def run_build(project_dir: Path) -> Tuple[bool, str]:
    """Run PlatformIO build and return (success, output)"""
    try:
        result = subprocess.run(
            [sys.executable, "-m", "platformio", "run", "-e", "pico"],
            cwd=project_dir,
            capture_output=True,
            text=True
        )
        output = result.stdout + result.stderr
        return result.returncode == 0, output
    except Exception as e:
        return False, str(e)


def parse_build_output(output: str) -> Tuple[str, str]:
    """Parse build output for flash and RAM usage"""
    flash_used = ""
    ram_used = ""
    
    for line in output.split('\n'):
        if 'Flash:' in line:
            flash_used = line.strip()
        elif 'RAM:' in line:
            ram_used = line.strip()
    
    return flash_used, ram_used


def verify_firmware(project_dir: Path) -> Optional[BuildInfo]:
    """Verify firmware file exists and gather build info"""
    firmware_path = project_dir / ".pio" / "build" / "pico" / "firmware.uf2"
    
    if not firmware_path.exists():
        return None
    
    # Get file info
    firmware_size = firmware_path.stat().st_size
    
    # Calculate MD5 hash
    hasher = hashlib.md5()
    with open(firmware_path, 'rb') as f:
        hasher.update(f.read())
    firmware_hash = hasher.hexdigest().upper()
    
    # Get version from source
    version, build_number = extract_version_from_source(project_dir)
    
    return BuildInfo(
        firmware_path=firmware_path,
        firmware_size=firmware_size,
        firmware_hash=firmware_hash,
        version=version,
        build_number=build_number
    )


def find_pico_serial_port() -> Optional[str]:
    """Auto-detect Pico serial port"""
    if not SERIAL_AVAILABLE:
        return None
    
    for port in serial.tools.list_ports.comports():
        # Look for Pico identifiers
        desc_lower = (port.description or "").lower()
        if any(x in desc_lower for x in ['pico', 'rp2040', 'usb serial']):
            return port.device
        # Also check VID/PID for Raspberry Pi Pico
        if port.vid == 0x2E8A:  # Raspberry Pi VID
            return port.device
    
    return None


def find_bootsel_drive() -> Optional[str]:
    """Find the RPI-RP2 drive letter (BOOTSEL mode)"""
    if sys.platform == 'win32':
        import ctypes
        import string
        
        # Get all drive letters
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        
        for letter in string.ascii_uppercase:
            if bitmask & 1:
                drive = f"{letter}:"
                try:
                    # Check if it's a removable drive with RPI-RP2 label
                    drive_type = ctypes.windll.kernel32.GetDriveTypeW(f"{drive}\\")
                    if drive_type == 2:  # DRIVE_REMOVABLE
                        # Try to check volume label
                        volume_name = ctypes.create_unicode_buffer(261)
                        if ctypes.windll.kernel32.GetVolumeInformationW(
                            f"{drive}\\", volume_name, 261, None, None, None, None, 0
                        ):
                            if volume_name.value == "RPI-RP2":
                                return drive
                except Exception:
                    pass
            bitmask >>= 1
    else:
        # Linux/Mac - look for mount point
        for mount_point in ['/media', '/mnt', '/Volumes']:
            rpi_path = Path(mount_point)
            if rpi_path.exists():
                for item in rpi_path.iterdir():
                    if 'RPI-RP2' in item.name.upper():
                        return str(item)
    
    return None


def send_bootsel_command(port: str) -> bool:
    """Send bootsel command to trigger BOOTSEL mode"""
    if not SERIAL_AVAILABLE:
        print_info("pyserial not installed - cannot send BOOTSEL command")
        return False
    
    try:
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(0.3)  # Wait for connection
        
        # Clear any pending data
        ser.reset_input_buffer()
        
        # Send bootsel command (CLI command is just "bootsel")
        ser.write(b"bootsel\r\n")
        ser.flush()
        
        time.sleep(0.5)
        ser.close()
        
        print_info(f"BOOTSEL command sent to {port}")
        return True
    except Exception as e:
        print_info(f"Could not send command: {e}")
        return False


def wait_for_bootsel_drive(timeout: int = 15) -> Optional[str]:
    """Wait for RPI-RP2 drive to appear"""
    print_info("Waiting for RPI-RP2 drive...")
    
    start_time = time.time()
    dots_printed = 0
    
    while time.time() - start_time < timeout:
        drive = find_bootsel_drive()
        if drive:
            if dots_printed > 0:
                print()  # Newline after dots
            return drive
        
        time.sleep(0.5)
        print(".", end="", flush=True)
        dots_printed += 1
    
    if dots_printed > 0:
        print()  # Newline after dots
    
    return None


def flash_firmware(firmware_path: Path, drive: str) -> bool:
    """Copy firmware to BOOTSEL drive"""
    try:
        dest = Path(drive) / "firmware.uf2"
        shutil.copy2(firmware_path, dest)
        return True
    except Exception as e:
        print_err(f"Copy failed: {e}")
        return False


def wait_for_serial_port(port: Optional[str], timeout: int = 15) -> Optional[str]:
    """Wait for serial port to reappear after flash"""
    if not SERIAL_AVAILABLE:
        return None
    
    print_info("Waiting for device to reboot...")
    
    start_time = time.time()
    
    while time.time() - start_time < timeout:
        if port:
            # Check if specific port is available
            for p in serial.tools.list_ports.comports():
                if p.device == port:
                    return port
        else:
            # Auto-detect
            found = find_pico_serial_port()
            if found:
                return found
        
        time.sleep(0.5)
    
    return None


def verify_device_version(port: str, expected_version: str, expected_build: int) -> Tuple[bool, str, int]:
    """
    Connect to device and verify firmware version matches.
    Returns (success, actual_version, actual_build)
    """
    if not SERIAL_AVAILABLE:
        return False, "", 0
    
    try:
        ser = serial.Serial(port, 115200, timeout=3)
        time.sleep(1.0)  # Wait for device to be ready
        
        # Clear buffer
        ser.reset_input_buffer()
        
        # Send version command with JSON flag for easy parsing
        ser.write(b"version --json\r\n")
        ser.flush()
        
        time.sleep(0.5)
        
        # Read response
        response = ser.read(ser.in_waiting or 1024).decode('utf-8', errors='ignore')
        ser.close()
        
        # Parse JSON response
        # Looking for: {"firmware":"1.1.0","build":122,...}
        version_match = re.search(r'"firmware"\s*:\s*"([^"]+)"', response)
        build_match = re.search(r'"build"\s*:\s*(\d+)', response)
        
        if version_match and build_match:
            actual_version = version_match.group(1)
            actual_build = int(build_match.group(1))
            
            # Normalize version comparison (strip 'v' prefix if present)
            expected_normalized = expected_version.lstrip('v')
            actual_normalized = actual_version.lstrip('v')
            
            success = (actual_normalized == expected_normalized and actual_build == expected_build)
            return success, actual_version, actual_build
        
        # Fallback: try text parsing
        # Looking for: Firmware: 1.1.0  Build: 122
        version_match = re.search(r'(?:Firmware|Version)[:\s]+v?(\d+\.\d+\.\d+)', response, re.IGNORECASE)
        build_match = re.search(r'Build[:\s]+(\d+)', response, re.IGNORECASE)
        
        if version_match and build_match:
            actual_version = version_match.group(1)
            actual_build = int(build_match.group(1))
            
            # Normalize version comparison (strip 'v' prefix if present)
            expected_normalized = expected_version.lstrip('v')
            actual_normalized = actual_version.lstrip('v')
            
            success = (actual_normalized == expected_normalized and actual_build == expected_build)
            return success, actual_version, actual_build
        
        return False, "", 0
        
    except Exception as e:
        print_info(f"Version check error: {e}")
        return False, "", 0


def main():
    parser = argparse.ArgumentParser(description="HubFX Pico Build and Flash Utility")
    parser.add_argument("--no-build", action="store_true", help="Skip build step")
    parser.add_argument("--no-clean", action="store_true", help="Skip clean step")
    parser.add_argument("--skip-verify", action="store_true", help="Skip post-flash verification")
    parser.add_argument("--port", type=str, help="Serial port (default: auto-detect)")
    parser.add_argument("--timeout", type=int, default=15, help="BOOTSEL wait timeout in seconds")
    args = parser.parse_args()

    project_dir = get_project_dir()
    os.chdir(project_dir)

    print_header("HubFX Pico - Build and Flash Utility")

    # Calculate total steps
    total_steps = 5
    if args.no_build:
        total_steps -= 1
    if args.skip_verify:
        total_steps -= 1
    
    current_step = 0
    build_info: Optional[BuildInfo] = None

    # =========================================================================
    # STEP 1: Clean and Build
    # =========================================================================
    if not args.no_build:
        current_step += 1
        print_step(current_step, total_steps, "Building firmware...")
        
        # Increment build number
        new_build = increment_build_number(project_dir)
        if new_build:
            print_info(f"Build number incremented to {new_build}")
        
        # Clean (unless --no-clean)
        if not args.no_clean:
            print_info("Cleaning previous build...")
            run_clean(project_dir)
        
        # Build
        success, output = run_build(project_dir)
        
        if not success:
            print_err("Build failed!")
            print(output)
            return 1
        
        # Parse build output for stats
        flash_used, ram_used = parse_build_output(output)
        
        print_ok("Build complete")
        if flash_used:
            print_info(flash_used)
        if ram_used:
            print_info(ram_used)

    # =========================================================================
    # STEP 2: Verify firmware file
    # =========================================================================
    current_step += 1
    print_step(current_step, total_steps, "Verifying firmware...")
    
    build_info = verify_firmware(project_dir)
    
    if not build_info:
        print_err("Firmware file not found!")
        print_info("Expected: .pio/build/pico/firmware.uf2")
        return 1
    
    print_ok("Firmware verified")
    print_info(f"Size: {build_info.firmware_size:,} bytes")
    print_info(f"MD5:  {build_info.firmware_hash}")
    print_info(f"Version: {build_info.version} (Build {build_info.build_number})")

    # =========================================================================
    # STEP 3: Enter BOOTSEL mode
    # =========================================================================
    current_step += 1
    print_step(current_step, total_steps, "Entering BOOTSEL mode...")
    
    # Check if already in BOOTSEL mode
    drive = find_bootsel_drive()
    serial_port = args.port or find_pico_serial_port()
    
    if drive:
        print_ok("Device already in BOOTSEL mode")
    else:
        # Try to send bootsel command via serial
        if serial_port:
            print_info(f"Found device at {serial_port}")
            send_bootsel_command(serial_port)
        else:
            print_info("No serial port found")
        
        # Wait for BOOTSEL drive
        drive = wait_for_bootsel_drive(args.timeout)
        
        if not drive:
            print_err(f"RPI-RP2 drive not found within {args.timeout}s")
            print_info("Try: Hold BOOTSEL button and press RESET manually")
            return 1
        
        print_ok("BOOTSEL mode active")
    
    print_info(f"Drive: {drive}")

    # =========================================================================
    # STEP 4: Flash firmware
    # =========================================================================
    current_step += 1
    print_step(current_step, total_steps, "Flashing firmware...")
    
    if not flash_firmware(build_info.firmware_path, drive):
        return 1
    
    print_ok(f"Firmware copied to {drive}")
    print_info("Device will reboot automatically...")
    
    # Wait for device to reboot
    time.sleep(3)

    # =========================================================================
    # STEP 5: Verify device version
    # =========================================================================
    if not args.skip_verify:
        current_step += 1
        print_step(current_step, total_steps, "Verifying device...")
        
        # Wait for serial port to reappear
        port = wait_for_serial_port(serial_port, timeout=15)
        
        if not port:
            print_err("Device not found after flash")
            print_info("Try: python -m serial.tools.list_ports")
            return 1
        
        # Give device time to fully initialize
        time.sleep(2)
        
        # Verify version
        success, actual_version, actual_build = verify_device_version(
            port, 
            build_info.version, 
            build_info.build_number
        )
        
        if success:
            print_ok(f"Version verified: {actual_version} (Build {actual_build})")
        elif actual_version and actual_build:
            print_err(f"Version mismatch!")
            print_info(f"Expected: {build_info.version} (Build {build_info.build_number})")
            print_info(f"Actual:   {actual_version} (Build {actual_build})")
            return 1
        else:
            print_info(f"Device online at {port}")
            print_info("Could not verify version (non-fatal)")

    # =========================================================================
    # Done!
    # =========================================================================
    print()
    print(f"{Colors.GREEN}{'=' * 44}{Colors.RESET}")
    print(f"{Colors.GREEN}  Build and Flash Complete!{Colors.RESET}")
    print(f"{Colors.GREEN}{'=' * 44}{Colors.RESET}")
    print()
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
