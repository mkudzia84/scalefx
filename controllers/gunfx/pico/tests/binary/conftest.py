"""
GunFX Pico Binary Protocol Test Configuration

Shared fixtures and configuration for pytest-based serial tests.
Uses binary protocol with COBS encoding for communication with GunFX board.

Protocol: Binary (INIT protocol=binary)
Encoding: COBS with 0x00 delimiter
CRC: CRC-8 polynomial 0x07
"""

import pytest
import serial
import time
import struct
import os
from typing import Optional, Generator, Dict, Tuple, List

# Configuration
DEFAULT_PORT = os.environ.get('GUNFX_PORT', 'COM10')
BAUD_RATE = 115200
TIMEOUT = 2
STARTUP_DELAY = 0.5
INIT_TIMEOUT = 3.0

# ============================================================================
# Packet Type Constants
# ============================================================================

# Universal packet types (0xF0-0xFF)
SFX_PKT_INIT = 0xF0
SFX_PKT_SHUTDOWN = 0xF1
SFX_PKT_KEEPALIVE = 0xF2
SFX_PKT_INIT_READY = 0xF3
SFX_PKT_STATUS = 0xF4
SFX_PKT_ERROR = 0xF5
SFX_PKT_ACK = 0xF6
SFX_PKT_NACK = 0xF7
SFX_PKT_REBOOT = 0xF8
SFX_PKT_BOOTSEL = 0xF9
SFX_PKT_STATUS_REQ = 0xFA

# GunFX-specific packet types (0x01-0x2F)
GUNFX_PKT_TRIGGER_ON = 0x01
GUNFX_PKT_TRIGGER_OFF = 0x02
GUNFX_PKT_SRV_SET = 0x10
GUNFX_PKT_SRV_SETTINGS = 0x11
GUNFX_PKT_SRV_RECOIL_JERK = 0x12
GUNFX_PKT_SMOKE_HEAT = 0x20
GUNFX_PKT_SMOKE_SETTINGS = 0x21

# Error codes
class GunFxError:
    OK = 0x00
    UNKNOWN_COMMAND = 0x01
    INVALID_PARAMETER = 0x02
    MISSING_PARAMETER = 0x03
    SERVO_INVALID_ID = 0x20
    SERVO_PULSE_RANGE = 0x21
    SERVO_MIN_MAX = 0x22
    SERVO_NOT_CONFIGURED = 0x23
    HEATER_SAFETY = 0x30
    FAN_NOT_RUNNING = 0x31
    INVALID_FAN_SPEED = 0x32
    INVALID_RPM = 0x40
    ALREADY_FIRING = 0x41
    NOT_FIRING = 0x42


# ============================================================================
# COBS Encoding/Decoding
# ============================================================================

def cobs_encode(data: bytes) -> bytes:
    """
    COBS (Consistent Overhead Byte Stuffing) encode data.
    
    Encodes data so it contains no zero bytes, then appends 0x00 delimiter.
    """
    if len(data) == 0:
        return b'\x01\x00'
    
    output = bytearray()
    code_idx = 0
    output.append(0)  # Placeholder for first code
    code = 1
    
    for byte in data:
        if byte == 0:
            output[code_idx] = code
            code_idx = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_idx] = code
                code_idx = len(output)
                output.append(0)
                code = 1
    
    output[code_idx] = code
    output.append(0)  # Delimiter
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    """
    COBS decode data.
    
    Assumes 0x00 delimiter is NOT included in input.
    """
    if len(data) == 0:
        return b''
    
    output = bytearray()
    idx = 0
    
    while idx < len(data):
        code = data[idx]
        idx += 1
        
        if code == 0:
            break
            
        for _ in range(code - 1):
            if idx >= len(data):
                break
            output.append(data[idx])
            idx += 1
        
        if code < 0xFF and idx < len(data):
            output.append(0)
    
    # Remove trailing zero if present
    if output and output[-1] == 0:
        output = output[:-1]
    
    return bytes(output)


# ============================================================================
# CRC-8 Calculation
# ============================================================================

def crc8(data: bytes, poly: int = 0x07) -> int:
    """Calculate CRC-8 using polynomial 0x07."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ poly) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


# ============================================================================
# Packet Building/Parsing
# ============================================================================

def build_packet(pkt_type: int, payload: bytes = b'') -> bytes:
    """
    Build a binary packet with COBS encoding.
    
    Format before encoding: [type:u8][len:u8][payload:len bytes][crc:u8]
    """
    length = len(payload)
    packet = bytes([pkt_type, length]) + payload
    packet_crc = crc8(packet)
    raw_packet = packet + bytes([packet_crc])
    return cobs_encode(raw_packet)


def parse_packet(data: bytes) -> Tuple[Optional[int], bytes]:
    """
    Parse a COBS-encoded packet.
    
    Returns:
        Tuple of (packet_type, payload) or (None, b'') if invalid
    """
    # Remove delimiter if present
    if data.endswith(b'\x00'):
        data = data[:-1]
    
    decoded = cobs_decode(data)
    if len(decoded) < 3:  # type + len + crc minimum
        return None, b''
    
    pkt_type = decoded[0]
    length = decoded[1]
    
    if len(decoded) < 2 + length + 1:
        return None, b''
    
    payload = decoded[2:2+length]
    received_crc = decoded[2+length]
    
    # Verify CRC
    expected_crc = crc8(decoded[:2+length])
    if received_crc != expected_crc:
        return None, b''
    
    return pkt_type, payload


# ============================================================================
# GunFX Binary Connection Class
# ============================================================================

class GunFxBinaryConnection:
    """Wrapper for serial communication with GunFX Pico using binary protocol."""
    
    def __init__(self, port: str = DEFAULT_PORT):
        self.port = port
        self.ser: Optional[serial.Serial] = None
        self._initialized = False
        self._device_name = ""
        self._firmware_version = ""
        self._build_number = 0
        self._platform = ""
        
    def connect(self) -> bool:
        """Connect to the serial port and initialize with binary protocol."""
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(STARTUP_DELAY)
            self._clear_buffer()
            
            # Initialize with binary protocol (INIT is always text)
            if not self._send_init():
                pytest.skip(f"GunFX on {self.port} did not respond to INIT")
                return False
            
            return True
        except serial.SerialException as e:
            pytest.skip(f"Serial port {self.port} not available: {e}")
            return False
    
    def close(self):
        """Close the serial connection."""
        if self.ser:
            try:
                if self.ser.is_open:
                    # Send shutdown command before disconnect
                    try:
                        self.send_packet(SFX_PKT_SHUTDOWN)
                    except:
                        pass
                    time.sleep(0.1)
                    self.ser.close()
            except:
                pass
            finally:
                self.ser = None
        self._initialized = False
    
    def disconnect(self):
        """Alias for close()."""
        self.close()
    
    def _clear_buffer(self):
        """Clear any pending data in the receive buffer."""
        if self.ser and self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
    
    def _send_init(self) -> bool:
        """Send INIT command (text) and wait for INIT_READY response (text)."""
        if not self.ser:
            return False
        
        self._clear_buffer()
        # INIT is always text, regardless of protocol mode
        self.ser.write(b"INIT protocol=binary\n")
        
        # Wait for INIT_READY response (text)
        start = time.time()
        buffer = ""
        
        while time.time() - start < INIT_TIMEOUT:
            if self.ser.in_waiting:
                buffer += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                
                for line in buffer.split('\n'):
                    if line.startswith('INIT_READY'):
                        self._parse_init_ready(line)
                        self._initialized = True
                        return True
            time.sleep(0.05)
        
        return False
    
    def _parse_init_ready(self, line: str):
        """Parse INIT_READY response to extract device info."""
        parts = line.split()
        for part in parts[1:]:
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
    
    def send_packet(self, pkt_type: int, payload: bytes = b'') -> None:
        """Send a binary packet."""
        if not self.ser:
            raise RuntimeError("Not connected")
        
        packet = build_packet(pkt_type, payload)
        self.ser.write(packet)
    
    def receive_packet(self, timeout: float = 1.0) -> Tuple[Optional[int], bytes]:
        """
        Receive a binary packet.
        
        Returns:
            Tuple of (packet_type, payload) or (None, b'') if timeout/error
        """
        if not self.ser:
            return None, b''
        
        start = time.time()
        buffer = bytearray()
        
        while time.time() - start < timeout:
            if self.ser.in_waiting:
                byte = self.ser.read(1)
                if byte == b'\x00':
                    # End of packet
                    if buffer:
                        return parse_packet(bytes(buffer))
                    buffer = bytearray()
                else:
                    buffer.extend(byte)
            else:
                time.sleep(0.01)
        
        return None, b''
    
    def send_and_receive(self, pkt_type: int, payload: bytes = b'', 
                         timeout: float = 1.0) -> Tuple[Optional[int], bytes]:
        """Send a packet and wait for response."""
        self._clear_buffer()
        self.send_packet(pkt_type, payload)
        time.sleep(0.1)
        return self.receive_packet(timeout)
    
    def send_and_expect_ack(self, pkt_type: int, payload: bytes = b'',
                            timeout: float = 1.0) -> Tuple[bool, bytes]:
        """Send a packet and expect ACK response."""
        resp_type, resp_payload = self.send_and_receive(pkt_type, payload, timeout)
        return (resp_type == SFX_PKT_ACK, resp_payload)
    
    def send_and_expect_nack(self, pkt_type: int, payload: bytes = b'',
                             timeout: float = 1.0) -> Tuple[bool, int, bytes]:
        """
        Send a packet and expect NACK response.
        
        Returns:
            Tuple of (is_nack, error_code, reason_payload)
        """
        resp_type, resp_payload = self.send_and_receive(pkt_type, payload, timeout)
        if resp_type == SFX_PKT_NACK and len(resp_payload) >= 1:
            error_code = resp_payload[0]
            reason = resp_payload[1:] if len(resp_payload) > 1 else b''
            return True, error_code, reason
        return False, 0, b''
    
    def wait_for_status(self, timeout: float = 2.0) -> Optional[Dict]:
        """
        Request and wait for a STATUS packet from the device.
        
        Sends STATUS_REQ and waits for STATUS response.
        
        Returns:
            Parsed status dict or None if timeout
        """
        # Send STATUS_REQ packet
        self._clear_buffer()
        self.send_packet(SFX_PKT_STATUS_REQ)
        
        start = time.time()
        
        while time.time() - start < timeout:
            pkt_type, payload = self.receive_packet(timeout=0.5)
            if pkt_type == SFX_PKT_STATUS:
                return self._parse_status(payload)
        
        return None
    
    def request_status(self, timeout: float = 2.0) -> Optional[Dict]:
        """
        Request status via STATUS_REQ packet.
        
        Returns:
            Parsed status dict or None if timeout
        """
        return self.wait_for_status(timeout)
    
    def _parse_status(self, payload: bytes) -> Dict:
        """
        Parse STATUS packet payload.
        
        Binary format (28 bytes):
        [flags:u8][fanSpeed:u8][fanOffRemainingMs:u16][servo0Us:u16][servo1Us:u16]
        [servo2Us:u16][rpm:u16][shotsFired:u32][heaterOnTimeMs:u32][uptimeMs:u32][freeRam:u32]
        
        Flags:
        - Bit 0: firing
        - Bit 1: flashActive
        - Bit 2: flashFading
        - Bit 3: heaterOn
        - Bit 4: fanOn
        - Bit 5: fanSpindown
        """
        if len(payload) < 28:
            return {}
        
        flags = payload[0]
        fan_speed = payload[1]
        fan_off_remaining, servo0, servo1, servo2, rpm = struct.unpack('<HHHHH', payload[2:12])
        shots_fired, heater_on_time, uptime, free_ram = struct.unpack('<IIII', payload[12:28])
        
        return {
            'firing': bool(flags & 0x01),
            'flashActive': bool(flags & 0x02),
            'flashFading': bool(flags & 0x04),
            'heaterOn': bool(flags & 0x08),
            'fanOn': bool(flags & 0x10),
            'fanSpindown': bool(flags & 0x20),
            'fanSpeed': fan_speed,
            'fanOffRemainingMs': fan_off_remaining,
            'servo0': servo0,
            'servo1': servo1,
            'servo2': servo2,
            'rpm': rpm,
            'shotsFired': shots_fired,
            'heaterOnTimeMs': heater_on_time,
            'uptimeMs': uptime,
            'freeRam': free_ram
        }
    
    # ========================================================================
    # Command Helper Methods
    # ========================================================================
    
    def trigger_on(self, rpm: int) -> Tuple[bool, bytes]:
        """Send TRIGGER_ON command."""
        payload = struct.pack('<H', rpm)
        return self.send_and_expect_ack(GUNFX_PKT_TRIGGER_ON, payload)
    
    def trigger_off(self, fan_delay_ms: int = 3000) -> Tuple[bool, bytes]:
        """Send TRIGGER_OFF command."""
        payload = struct.pack('<H', fan_delay_ms)
        return self.send_and_expect_ack(GUNFX_PKT_TRIGGER_OFF, payload)
    
    def servo_set(self, servo_id: int, pulse_us: int) -> Tuple[bool, bytes]:
        """Send SERVO_SET command."""
        payload = struct.pack('<BH', servo_id, pulse_us)
        return self.send_and_expect_ack(GUNFX_PKT_SRV_SET, payload)
    
    def servo_config(self, servo_id: int, min_us: int, max_us: int,
                     max_speed: int = 0, max_accel: int = 0, 
                     max_decel: int = 0) -> Tuple[bool, bytes]:
        """Send SERVO_CONFIG command."""
        payload = struct.pack('<BHHHHH', servo_id, min_us, max_us, 
                             max_speed, max_accel, max_decel)
        return self.send_and_expect_ack(GUNFX_PKT_SRV_SETTINGS, payload)
    
    def servo_recoil_jerk(self, servo_id: int, jerk_us: int, 
                          variance_us: int) -> Tuple[bool, bytes]:
        """Send SERVO_RECOIL_JERK command."""
        payload = struct.pack('<BHH', servo_id, jerk_us, variance_us)
        return self.send_and_expect_ack(GUNFX_PKT_SRV_RECOIL_JERK, payload)
    
    def smoke_heat(self, enable: bool) -> Tuple[bool, bytes]:
        """Send SMOKE_HEAT command."""
        payload = struct.pack('<B', 1 if enable else 0)
        return self.send_and_expect_ack(GUNFX_PKT_SMOKE_HEAT, payload)
    
    def keepalive(self) -> None:
        """Send KEEPALIVE packet (no response expected)."""
        self.send_packet(SFX_PKT_KEEPALIVE)
    
    def shutdown(self) -> None:
        """Send SHUTDOWN packet (fire-and-forget)."""
        self.send_packet(SFX_PKT_SHUTDOWN)
    
    def reboot(self) -> None:
        """Send REBOOT packet (fire-and-forget)."""
        self.send_packet(SFX_PKT_REBOOT)
    
    def bootsel(self) -> None:
        """Send BOOTSEL packet (fire-and-forget)."""
        self.send_packet(SFX_PKT_BOOTSEL)


# ============================================================================
# Pytest Fixtures
# ============================================================================

@pytest.fixture(scope="module")
def gunfx_binary() -> Generator[GunFxBinaryConnection, None, None]:
    """
    Module-scoped fixture providing a connected GunFX binary connection.
    
    Connection is reused across all tests in the same module.
    """
    conn = GunFxBinaryConnection()
    try:
        if conn.connect():
            yield conn
        else:
            yield conn
    finally:
        # Always ensure port is released
        conn.close()


@pytest.fixture(scope="function")
def fresh_gunfx_binary() -> Generator[GunFxBinaryConnection, None, None]:
    """
    Function-scoped fixture providing a fresh GunFX binary connection.
    
    Each test gets a newly initialized connection.
    """
    conn = GunFxBinaryConnection()
    try:
        if conn.connect():
            yield conn
        else:
            yield conn
    finally:
        # Always ensure port is released
        conn.close()


# ============================================================================
# Pytest Configuration
# ============================================================================

def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "hardware: requires connected GunFX hardware")
    config.addinivalue_line("markers", "firing: tests that trigger gun effects")
    config.addinivalue_line("markers", "servo: tests that move servo motors")
    config.addinivalue_line("markers", "smoke: tests that control smoke generator")
    config.addinivalue_line("markers", "slow: tests that take a long time")
    config.addinivalue_line("markers", "destructive: tests that reboot or reset device")
    config.addinivalue_line("markers", "binary: tests using binary protocol")
