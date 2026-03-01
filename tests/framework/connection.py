"""
Serial Connection Handler

Manages USB/serial communication with ScaleFX controllers.

Verbose Mode:
    Set SCALEFX_VERBOSE=1 environment variable or pass verbose=True to enable
    detailed command/response logging during tests.
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


# ANSI colors for verbose output
class _Colors:
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    GRAY = '\033[90m'
    MAGENTA = '\033[95m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def _packet_type_name(ptype: int) -> str:
    """Get human-readable packet type name."""
    names = {
        CorePacket.INIT: "INIT",
        CorePacket.INIT_READY: "INIT_READY",
        CorePacket.ACK: "ACK",
        CorePacket.NACK: "NACK",
        CorePacket.STATUS: "STATUS",
        CorePacket.SHUTDOWN: "SHUTDOWN",
        CorePacket.REBOOT: "REBOOT",
        CorePacket.BOOTSEL: "BOOTSEL",
        CorePacket.KEEPALIVE: "KEEPALIVE",
        CorePacket.STATUS_REQ: "STATUS_REQ",
    }
    return names.get(ptype, f"0x{ptype:02X}")


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
    
    def __str__(self) -> str:
        """Human-readable response string."""
        name = _packet_type_name(self.packet_type)
        if self.is_nack:
            err_name = CoreError.name(self.error_code)
            return f"{name}({err_name})"
        elif self.payload:
            return f"{name}[{len(self.payload)}B]"
        return name


class ScaleFXConnection:
    """
    Serial connection to a ScaleFX controller.
    
    Handles COBS packet framing, send/receive, and response parsing.
    
    Verbose Mode:
        Set SCALEFX_VERBOSE=1 or pass verbose=True to see command/response details.
    
    Example:
        conn = ScaleFXConnection(port="COM3", verbose=True)
        if conn.connect():
            response = conn.send_and_wait(CommandBuilder.init())
            if response and response.is_init_ready:
                print("Connected!")
            conn.close()
    """
    
    DEFAULT_BAUD = 115200
    DEFAULT_TIMEOUT = 2.0
    
    def __init__(self, port: Optional[str] = None, baud: int = DEFAULT_BAUD,
                 timeout: float = DEFAULT_TIMEOUT, verbose: Optional[bool] = None):
        """
        Initialize connection.
        
        Args:
            port: Serial port (e.g., "COM3", "/dev/ttyACM0")
            baud: Baud rate (default 115200)
            timeout: Response timeout in seconds
            verbose: Enable verbose logging (default: from SCALEFX_VERBOSE env)
        """
        self.port = port or self._default_port()
        self.baud = baud
        self.timeout = timeout
        self._serial: Optional[serial.Serial] = None
        self._rx_buffer = bytearray()
        self._lock = threading.Lock()
        self._initialized = False
        self._callbacks: List[Callable[[Response], None]] = []
        
        # Verbose mode from env or parameter
        if verbose is None:
            self.verbose = os.environ.get('SCALEFX_VERBOSE', '').lower() in ('1', 'true', 'yes')
        else:
            self.verbose = verbose
    
    def _log_send(self, data: bytes, cmd_name: str = None):
        """Log outgoing packet if verbose mode enabled."""
        if not self.verbose:
            return
        
        # Parse packet type from raw data
        if len(data) >= 2:
            # COBS decode to get packet type
            try:
                parsed = parse_packet(data)
                if parsed:
                    ptype, payload = parsed
                    name = cmd_name or _packet_type_name(ptype)
                    payload_hex = payload.hex() if payload else ""
                    print(f"  {_Colors.CYAN}→ TX:{_Colors.RESET} {_Colors.BOLD}{name}{_Colors.RESET}", end="")
                    if payload_hex:
                        print(f" {_Colors.GRAY}[{payload_hex}]{_Colors.RESET}", end="")
                    print()
                    return
            except:
                pass
        
        # Fallback: raw hex
        print(f"  {_Colors.CYAN}→ TX:{_Colors.RESET} {_Colors.GRAY}{data.hex()}{_Colors.RESET}")
    
    def _log_recv(self, response: Response):
        """Log incoming response if verbose mode enabled."""
        if not self.verbose or response is None:
            return
        
        name = _packet_type_name(response.packet_type)
        
        # Color based on response type
        if response.is_ack:
            color = _Colors.GREEN
            detail = ""
        elif response.is_nack:
            color = _Colors.RED
            err_name = CoreError.name(response.error_code)
            detail = f" {_Colors.RED}[{err_name}]{_Colors.RESET}"
        elif response.is_init_ready:
            color = _Colors.GREEN
            detail = f" {_Colors.GRAY}[{len(response.payload)}B payload]{_Colors.RESET}"
        else:
            color = _Colors.YELLOW
            if response.payload:
                detail = f" {_Colors.GRAY}[{response.payload.hex()}]{_Colors.RESET}"
            else:
                detail = ""
        
        print(f"  {_Colors.MAGENTA}← RX:{_Colors.RESET} {color}{_Colors.BOLD}{name}{_Colors.RESET}{detail}")
    
    def _log_timeout(self):
        """Log timeout if verbose mode enabled."""
        if self.verbose:
            print(f"  {_Colors.MAGENTA}← RX:{_Colors.RESET} {_Colors.RED}TIMEOUT{_Colors.RESET}")
    
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
        
        self._log_send(data)
        
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
                    response = Response(packet_type, payload, packet_data)
                    self._log_recv(response)
                    return response
            
            time.sleep(0.01)
        
        self._log_timeout()
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
