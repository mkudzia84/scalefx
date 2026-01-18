"""
Serial Connection Handler

Manages USB/serial communication with ScaleFX controllers.
"""

import os
import sys
import time
import threading
from typing import Optional, Tuple, Callable, List
from dataclasses import dataclass

import serial

from .protocol import build_packet, parse_packet, cobs_decode
from .packets import CorePacket, CoreError


@dataclass
class Response:
    """Response from controller."""
    packet_type: int
    payload: bytes
    raw: bytes
    
    @property
    def is_ack(self) -> bool:
        return self.packet_type == CorePacket.ACK
    
    @property
    def is_nack(self) -> bool:
        return self.packet_type == CorePacket.NACK
    
    @property
    def is_init_ready(self) -> bool:
        return self.packet_type == CorePacket.INIT_READY
    
    @property
    def error_code(self) -> int:
        """Get NACK error code."""
        if self.is_nack and len(self.payload) > 0:
            return self.payload[0]
        return 0
    
    @property
    def error_message(self) -> str:
        """Get NACK error message."""
        if self.is_nack and len(self.payload) > 1:
            return self.payload[1:].decode('utf-8', errors='replace')
        return ""


class ScaleFXConnection:
    """
    Serial connection to a ScaleFX controller.
    
    Handles COBS packet framing, send/receive, and response parsing.
    
    Example:
        conn = ScaleFXConnection(port="COM3")
        if conn.connect():
            response = conn.send_and_wait(CommandBuilder.init())
            if response and response.is_init_ready:
                print("Connected!")
            conn.close()
    """
    
    DEFAULT_BAUD = 115200
    DEFAULT_TIMEOUT = 2.0
    
    def __init__(self, port: Optional[str] = None, baud: int = DEFAULT_BAUD,
                 timeout: float = DEFAULT_TIMEOUT):
        """
        Initialize connection.
        
        Args:
            port: Serial port (e.g., "COM3", "/dev/ttyACM0")
            baud: Baud rate (default 115200)
            timeout: Response timeout in seconds
        """
        self.port = port or self._default_port()
        self.baud = baud
        self.timeout = timeout
        self._serial: Optional[serial.Serial] = None
        self._rx_buffer = bytearray()
        self._lock = threading.Lock()
        self._initialized = False
        self._callbacks: List[Callable[[Response], None]] = []
    
    @staticmethod
    def _default_port() -> str:
        """Get default port from environment or platform default."""
        if os.environ.get('SCALEFX_PORT'):
            return os.environ['SCALEFX_PORT']
        if sys.platform == 'win32':
            return 'COM3'
        return '/dev/ttyACM0'
    
    @property
    def is_connected(self) -> bool:
        """Check if serial port is open."""
        return self._serial is not None and self._serial.is_open
    
    @property
    def is_initialized(self) -> bool:
        """Check if INIT handshake completed."""
        return self._initialized
    
    def connect(self, init: bool = True) -> bool:
        """
        Open serial connection and optionally initialize.
        
        Args:
            init: Send INIT command after connecting
        
        Returns:
            True if successful
        """
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.1,
                write_timeout=1.0
            )
            time.sleep(0.1)  # Wait for device to be ready
            self._rx_buffer.clear()
            
            if init:
                return self.initialize()
            return True
            
        except serial.SerialException as e:
            print(f"Connection failed: {e}")
            return False
    
    def close(self):
        """Close serial connection."""
        if self._serial:
            try:
                self._serial.close()
            except:
                pass
            self._serial = None
        self._initialized = False
    
    def initialize(self) -> bool:
        """
        Send INIT command and wait for INIT_READY.
        
        Returns:
            True if initialization successful
        """
        response = self.send_and_wait(build_packet(CorePacket.INIT))
        if response and response.is_init_ready:
            self._initialized = True
            return True
        return False
    
    def send(self, data: bytes) -> bool:
        """
        Send raw bytes (already COBS encoded with delimiter).
        
        Args:
            data: COBS encoded packet bytes
        
        Returns:
            True if sent successfully
        """
        if not self.is_connected:
            return False
        
        with self._lock:
            try:
                self._serial.write(data)
                self._serial.flush()
                return True
            except serial.SerialException:
                return False
    
    def receive(self, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Wait for and parse a response packet.
        
        Args:
            timeout: Override default timeout
        
        Returns:
            Response object, or None if timeout/error
        """
        if not self.is_connected:
            return None
        
        timeout = timeout or self.timeout
        deadline = time.time() + timeout
        
        while time.time() < deadline:
            # Read available data
            try:
                if self._serial.in_waiting:
                    data = self._serial.read(self._serial.in_waiting)
                    self._rx_buffer.extend(data)
            except serial.SerialException:
                return None
            
            # Look for complete packet (0x00 delimiter)
            if 0x00 in self._rx_buffer:
                idx = self._rx_buffer.index(0x00)
                packet_data = bytes(self._rx_buffer[:idx])
                self._rx_buffer = self._rx_buffer[idx + 1:]
                
                parsed = parse_packet(packet_data + b'\x00')
                if parsed:
                    packet_type, payload = parsed
                    return Response(packet_type, payload, packet_data)
            
            time.sleep(0.01)
        
        return None
    
    def send_and_wait(self, data: bytes, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Send command and wait for response.
        
        Args:
            data: COBS encoded packet
            timeout: Response timeout
        
        Returns:
            Response object, or None if failed
        """
        if not self.send(data):
            return None
        return self.receive(timeout)
    
    def send_expect_ack(self, data: bytes, timeout: Optional[float] = None) -> Tuple[bool, Optional[Response]]:
        """
        Send command and expect ACK response.
        
        Args:
            data: COBS encoded packet
            timeout: Response timeout
        
        Returns:
            Tuple of (success, response)
        """
        response = self.send_and_wait(data, timeout)
        if response is None:
            return (False, None)
        return (response.is_ack, response)
    
    def drain(self):
        """Drain any pending data from receive buffer."""
        if self.is_connected:
            try:
                while self._serial.in_waiting:
                    self._serial.read(self._serial.in_waiting)
                    time.sleep(0.01)
            except:
                pass
        self._rx_buffer.clear()
    
    def add_callback(self, callback: Callable[[Response], None]):
        """Add callback for async responses (STATUS, ERROR)."""
        self._callbacks.append(callback)
    
    def remove_callback(self, callback: Callable[[Response], None]):
        """Remove callback."""
        if callback in self._callbacks:
            self._callbacks.remove(callback)
    
    def __enter__(self):
        """Context manager entry."""
        self.connect()
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.close()
        return False


def find_ports() -> List[str]:
    """
    Find available serial ports.
    
    Returns:
        List of port names
    """
    import serial.tools.list_ports
    ports = []
    for port in serial.tools.list_ports.comports():
        ports.append(port.device)
    return sorted(ports)


def find_scalefx_ports() -> List[Tuple[str, str]]:
    """
    Find serial ports that look like ScaleFX controllers.
    
    Returns:
        List of (port, description) tuples
    """
    import serial.tools.list_ports
    ports = []
    for port in serial.tools.list_ports.comports():
        # Look for Raspberry Pi Pico or similar
        if 'Pico' in port.description or 'RP2040' in port.description:
            ports.append((port.device, port.description))
        elif port.vid == 0x2E8A:  # Raspberry Pi VID
            ports.append((port.device, port.description))
    return ports
