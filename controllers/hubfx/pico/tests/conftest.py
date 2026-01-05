"""
HubFX Pico Test Configuration

Shared fixtures and configuration for pytest-based serial tests.
"""

import pytest
import serial
import time
import json
import os
from typing import Optional, Generator

# Configuration
DEFAULT_PORT = os.environ.get('HUBFX_PORT', 'COM10')
BAUD_RATE = 115200
TIMEOUT = 2
STARTUP_DELAY = 0.5


class SerialConnection:
    """Wrapper for serial communication with the Pico."""
    
    def __init__(self, port: str = DEFAULT_PORT):
        self.port = port
        self.ser: Optional[serial.Serial] = None
        
    def connect(self) -> bool:
        """Connect to the serial port."""
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(STARTUP_DELAY)
            self._clear_buffer()
            return True
        except serial.SerialException as e:
            pytest.skip(f"Serial port {self.port} not available: {e}")
            return False
    
    def disconnect(self):
        """Close the serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()
            self.ser = None
    
    def _clear_buffer(self):
        """Clear any pending data in the receive buffer."""
        if self.ser and self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
    
    def send(self, cmd: str, delay: float = 0.3) -> str:
        """
        Send a command and return the response.
        
        Args:
            cmd: Command to send
            delay: Wait time after sending (seconds)
            
        Returns:
            Response string (excluding echo)
        """
        if not self.ser:
            raise RuntimeError("Not connected")
        
        self._clear_buffer()
        self.ser.write(f"{cmd}\r\n".encode('utf-8'))
        time.sleep(delay)
        
        response = ""
        while self.ser.in_waiting > 0:
            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
            time.sleep(0.05)
        
        # Remove echo line (first line usually mirrors the command)
        lines = response.strip().split('\n')
        if lines and lines[0].strip().startswith('>'):
            lines = lines[1:]
        
        return '\n'.join(lines).strip()
    
    def send_json(self, cmd: str, delay: float = 0.3) -> dict:
        """
        Send a command with --json flag and parse the JSON response.
        
        Args:
            cmd: Command to send (--json will be added if not present)
            delay: Wait time after sending
            
        Returns:
            Parsed JSON dict, or empty dict on parse error
        """
        if not cmd.endswith('--json') and not cmd.endswith('-j'):
            cmd = f"{cmd} --json"
        
        response = self.send(cmd, delay)
        
        # Find JSON in response
        for line in response.split('\n'):
            line = line.strip()
            if line.startswith('{') or line.startswith('['):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue
        
        return {}
    
    def wait_for(self, pattern: str, timeout: float = 5.0) -> bool:
        """Wait for a specific pattern in the response."""
        start = time.time()
        buffer = ""
        
        while time.time() - start < timeout:
            if self.ser and self.ser.in_waiting:
                buffer += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                if pattern in buffer:
                    return True
            time.sleep(0.05)
        
        return False


@pytest.fixture(scope="session")
def serial_port() -> str:
    """Return the configured serial port."""
    return DEFAULT_PORT


@pytest.fixture(scope="module")
def pico() -> Generator[SerialConnection, None, None]:
    """
    Fixture providing a serial connection to the Pico.
    
    Connection is established once per test module and reused.
    """
    conn = SerialConnection()
    if conn.connect():
        yield conn
        conn.disconnect()
    else:
        yield conn  # Will skip tests


@pytest.fixture
def fresh_pico(pico: SerialConnection) -> SerialConnection:
    """
    Fixture providing a clean connection state.
    
    Clears buffers before each test.
    """
    if pico.ser:
        pico._clear_buffer()
    return pico


# Markers for test categorization
def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "hardware: Tests requiring physical hardware")
    config.addinivalue_line("markers", "json: Tests for JSON output format")
    config.addinivalue_line("markers", "slow: Tests that take longer to run")
