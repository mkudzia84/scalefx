"""
GunFX Pico Test Configuration

Shared fixtures and configuration for pytest-based serial tests.
Uses text protocol for direct communication with GunFX board.

Protocol: Text-based (INIT protocol=text)
Commands: INIT, TRIGGER_ON, TRIGGER_OFF, SERVO_SET, SMOKE_HEAT, etc.
"""

import pytest
import serial
import time
import re
import os
from typing import Optional, Generator, Dict, Tuple

# Configuration
DEFAULT_PORT = os.environ.get('GUNFX_PORT', 'COM10')
BAUD_RATE = 115200
TIMEOUT = 2
STARTUP_DELAY = 0.5
INIT_TIMEOUT = 3.0


class GunFxConnection:
    """Wrapper for serial communication with GunFX Pico using text protocol."""
    
    def __init__(self, port: str = DEFAULT_PORT):
        self.port = port
        self.ser: Optional[serial.Serial] = None
        self._initialized = False
        self._device_name = ""
        self._firmware_version = ""
        self._build_number = 0
        self._platform = ""
        
    def connect(self) -> bool:
        """Connect to the serial port and initialize with text protocol."""
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(STARTUP_DELAY)
            self._clear_buffer()
            
            # Initialize with text protocol
            if not self._send_init():
                pytest.skip(f"GunFX on {self.port} did not respond to INIT")
                return False
            
            return True
        except serial.SerialException as e:
            pytest.skip(f"Serial port {self.port} not available: {e}")
            return False
    
    def disconnect(self):
        """Close the serial connection."""
        if self.ser:
            try:
                if self.ser.is_open:
                    # Send shutdown command before disconnect
                    try:
                        self.send_command("SHUTDOWN")
                    except:
                        pass
                    time.sleep(0.1)
                    self.ser.close()
            except:
                pass
            finally:
                self.ser = None
        self._initialized = False
    
    def close(self):
        """Alias for disconnect()."""
        self.disconnect()
    
    def _clear_buffer(self):
        """Clear any pending data in the receive buffer."""
        if self.ser and self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
    
    def _send_init(self) -> bool:
        """Send INIT command and wait for INIT_READY response."""
        if not self.ser:
            return False
        
        self._clear_buffer()
        self.ser.write(b"INIT protocol=text\n")
        
        # Wait for INIT_READY response
        start = time.time()
        buffer = ""
        
        while time.time() - start < INIT_TIMEOUT:
            if self.ser.in_waiting:
                buffer += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                
                # Look for INIT_READY line
                for line in buffer.split('\n'):
                    if line.startswith('INIT_READY'):
                        self._parse_init_ready(line)
                        self._initialized = True
                        return True
            time.sleep(0.05)
        
        return False
    
    def _parse_init_ready(self, line: str):
        """Parse INIT_READY response to extract device info."""
        # Format: INIT_READY name=GunFX-A4B2 version=0.2.0 build=1 platform=RP2040 cpuMHz=120 ramBytes=221624
        parts = line.split()
        for part in parts[1:]:  # Skip "INIT_READY"
            if '=' in part:
                key, value = part.split('=', 1)
                if key == 'name':
                    self._device_name = value
                elif key == 'version':
                    self._firmware_version = value
                elif key == 'build':
                    self._build_number = int(value)
                elif key == 'platform':
                    self._platform = value
    
    def send_command(self, cmd: str, delay: float = 0.3) -> str:
        """
        Send a command and return the response.
        
        Args:
            cmd: Command to send (without newline)
            delay: Wait time after sending (seconds)
            
        Returns:
            Response string
        """
        if not self.ser:
            raise RuntimeError("Not connected")
        
        self._clear_buffer()
        self.ser.write(f"{cmd}\n".encode('utf-8'))
        time.sleep(delay)
        
        response = ""
        while self.ser.in_waiting > 0:
            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
            time.sleep(0.05)
        
        return response.strip()
    
    def send_and_expect(self, cmd: str, expected: str, delay: float = 0.3) -> Tuple[bool, str]:
        """
        Send a command and check if response contains expected string.
        
        Returns:
            Tuple of (success, response)
        """
        response = self.send_command(cmd, delay)
        return (expected in response, response)
    
    def send_and_expect_ack(self, cmd: str, delay: float = 0.3) -> Tuple[bool, str]:
        """Send a command and expect ACK response."""
        return self.send_and_expect(cmd, "ACK", delay)
    
    def send_and_expect_nack(self, cmd: str, delay: float = 0.3) -> Tuple[bool, str]:
        """Send a command and expect NACK response."""
        return self.send_and_expect(cmd, "NACK", delay)
    
    def wait_for_status(self, timeout: float = 2.0) -> Optional[Dict]:
        """
        Wait for a STATUS message from the device.
        
        Note: With STATUS_REQ protocol, this method sends a STATUS_REQ command
        and waits for the STATUS response.
        
        Returns:
            Parsed status dict or None if timeout
        """
        # Send STATUS_REQ to request status
        self._clear_buffer()
        self.ser.write(b"STATUS_REQ\n")
        
        start = time.time()
        buffer = ""
        
        while time.time() - start < timeout:
            if self.ser and self.ser.in_waiting:
                buffer += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                
                for line in buffer.split('\n'):
                    if line.startswith('STATUS'):
                        return self._parse_status(line)
            time.sleep(0.05)
        
        return None
    
    def request_status(self, timeout: float = 2.0) -> Optional[Dict]:
        """
        Request status via STATUS_REQ command.
        
        Returns:
            Parsed status dict or None if timeout
        """
        return self.wait_for_status(timeout)
    
    def _parse_status(self, line: str) -> Dict:
        """
        Parse STATUS response into dict.
        
        Format: STATUS firing=1 flashActive=0 flashFading=0 heaterOn=1 fanOn=1 
                fanSpindown=0 fanOffRemainingMs=0 servo0=1500 servo1=1500 servo2=1500 rpm=600
        """
        result = {}
        parts = line.split()
        
        for part in parts[1:]:  # Skip "STATUS"
            if '=' in part:
                key, value = part.split('=', 1)
                # Convert to appropriate type
                if value in ('0', '1'):
                    result[key] = bool(int(value))
                elif value.isdigit():
                    result[key] = int(value)
                else:
                    result[key] = value
        
        return result
    
    def parse_nack(self, response: str) -> Tuple[int, str]:
        """
        Parse NACK response to extract error code and reason.
        
        Format: NACK code=32 reason=Invalid servo ID (use 1-3)
        
        Returns:
            Tuple of (error_code, reason)
        """
        code = 0
        reason = ""
        
        match = re.search(r'code=(\d+)', response)
        if match:
            code = int(match.group(1))
        
        match = re.search(r'reason=(.+)', response)
        if match:
            reason = match.group(1)
        
        return (code, reason)
    
    @property
    def device_name(self) -> str:
        return self._device_name
    
    @property
    def firmware_version(self) -> str:
        return self._firmware_version
    
    @property
    def build_number(self) -> int:
        return self._build_number
    
    @property
    def platform(self) -> str:
        return self._platform
    
    @property
    def is_initialized(self) -> bool:
        return self._initialized


@pytest.fixture(scope="session")
def serial_port() -> str:
    """Return the configured serial port."""
    return DEFAULT_PORT


@pytest.fixture(scope="module")
def gunfx() -> Generator[GunFxConnection, None, None]:
    """
    Fixture providing a serial connection to GunFX Pico.
    
    Connection is established once per test module and reused.
    Initializes with text protocol via INIT command.
    """
    conn = GunFxConnection()
    try:
        if conn.connect():
            yield conn
        else:
            yield conn  # Will skip tests
    finally:
        # Always ensure port is released
        conn.disconnect()


@pytest.fixture
def fresh_gunfx(gunfx: GunFxConnection) -> GunFxConnection:
    """
    Fixture providing a clean connection state.
    
    Clears buffers and stops any ongoing firing before each test.
    """
    if gunfx.ser:
        # Stop any ongoing firing
        gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.1)
        gunfx._clear_buffer()
    return gunfx


# Markers for test categorization
def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "hardware: Tests requiring physical hardware")
    config.addinivalue_line("markers", "text: Tests using text protocol")
    config.addinivalue_line("markers", "firing: Tests that involve firing (muzzle flash, smoke)")
    config.addinivalue_line("markers", "servo: Tests for servo control")
    config.addinivalue_line("markers", "smoke: Tests for smoke generator")
    config.addinivalue_line("markers", "slow: Tests that take longer to run")
    config.addinivalue_line("markers", "destructive: Tests that may affect hardware state")
