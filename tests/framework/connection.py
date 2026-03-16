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
from typing import Optional, Tuple, Callable, List, Dict
from dataclasses import dataclass

import serial

from .protocol import build_packet, parse_packet, cobs_decode
from .packets import CorePacket, CoreError, TAG_ASYNC


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
        CorePacket.IDENTIFY: "IDENTIFY",
    }
    return names.get(ptype, f"0x{ptype:02X}")


@dataclass
class Response:
    """Response from controller."""
    packet_type: int
    tag: int
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
    def is_identify(self) -> bool:
        return self.packet_type == CorePacket.IDENTIFY
    
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
    
    DEFAULT_BAUD = 6000000
    DEFAULT_TIMEOUT = 2.0
    
    def __init__(self, port: Optional[str] = None, baud: int = DEFAULT_BAUD,
                 timeout: float = DEFAULT_TIMEOUT, verbose: Optional[bool] = None):
        """
        Initialize connection.
        
        Args:
            port: Serial port (e.g., "COM3", "/dev/ttyACM0")
            baud: Baud rate (default 6000000)
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
        self._next_tag = 1  # Wrapping tag counter (1-255, 0 = async)
        self._pending: Dict[int, Response] = {}  # Tag -> out-of-order response
        
        # Device info (populated from INIT_READY response)
        self.device_name: Optional[str] = None
        self.device_version: Optional[str] = None
        self.device_platform: Optional[str] = None
        self.device_cpu_mhz: int = 0
        self.device_free_ram: int = 0
        self.device_build: int = 0
        
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
                    ptype, tag, payload = parsed
                    name = cmd_name or _packet_type_name(ptype)
                    payload_hex = payload.hex() if payload else ""
                    print(f"  {_Colors.CYAN}→ TX:{_Colors.RESET} {_Colors.BOLD}{name}{_Colors.RESET}", end="")
                    if tag:
                        print(f" {_Colors.GRAY}tag={tag}{_Colors.RESET}", end="")
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
        
        tag_str = f" {_Colors.GRAY}tag={response.tag}{_Colors.RESET}" if response.tag else ""
        print(f"  {_Colors.MAGENTA}← RX:{_Colors.RESET} {color}{_Colors.BOLD}{name}{_Colors.RESET}{tag_str}{detail}")
    
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
    
    def next_tag(self) -> int:
        """Get next wrapping correlation tag (1-255). 0 is reserved for async."""
        tag = self._next_tag
        self._next_tag = (self._next_tag % 255) + 1
        return tag
    
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
            # Increase OS serial buffers for pipelined uploads.
            # Default Windows COM port buffer (~4 KB) bottlenecks burst writes.
            # 128 KB TX matches the ESP32 RX buffer, allowing a full window of
            # data to be queued without per-chunk FlushFileBuffers() overhead.
            try:
                self._serial.set_buffer_size(rx_size=131072, tx_size=131072)
            except Exception:
                pass  # set_buffer_size is Windows-only, ignore on other platforms
            time.sleep(0.05)  # Wait for device to be ready
            self._rx_buffer.clear()
            self._pending.clear()
            
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
        self._pending.clear()
    
    def disconnect(self):
        """Close serial connection (alias for close())."""
        self.close()
    
    def initialize(self) -> bool:
        """
        Send INIT command and wait for INIT_READY.
        
        Parses device info (name, version, platform, cpu, RAM, build)
        from the INIT_READY payload.
        
        Returns:
            True if initialization successful
        """
        response = self.send_and_wait(build_packet(CorePacket.INIT))
        if response and response.is_init_ready:
            self._initialized = True
            self._parse_init_ready(response.payload)
            return True
        return False
    
    def _parse_init_ready(self, payload: bytes):
        """
        Parse INIT_READY payload and store device info.
        
        Wire format:
            [nameLen:u8][name][verLen:u8][ver][platLen:u8][plat]
            [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
        """
        try:
            offset = 0
            
            name_len = payload[offset]; offset += 1
            self.device_name = payload[offset:offset+name_len].decode('utf-8', errors='replace')
            offset += name_len
            
            ver_len = payload[offset]; offset += 1
            self.device_version = payload[offset:offset+ver_len].decode('utf-8', errors='replace')
            offset += ver_len
            
            plat_len = payload[offset]; offset += 1
            self.device_platform = payload[offset:offset+plat_len].decode('utf-8', errors='replace')
            offset += plat_len
            
            self.device_cpu_mhz = int.from_bytes(payload[offset:offset+4], 'little')
            offset += 4
            self.device_free_ram = int.from_bytes(payload[offset:offset+4], 'little')
            offset += 4
            self.device_build = int.from_bytes(payload[offset:offset+4], 'little')
        except (IndexError, KeyError):
            pass  # Best-effort parsing
    
    def send(self, data: bytes, flush: bool = True) -> bool:
        """
        Send raw bytes (already COBS encoded with delimiter).
        
        Args:
            data: COBS encoded packet bytes
            flush: If True (default), block until all bytes are physically
                   transmitted. Set to False for burst/pipelined sends where
                   the OS serial driver should buffer writes for throughput.
        
        Returns:
            True if sent successfully
        """
        if not self.is_connected:
            return False
        
        self._log_send(data)
        
        with self._lock:
            try:
                self._serial.write(data)
                if flush:
                    self._serial.flush()
                return True
            except serial.SerialException:
                return False
    
    def receive(self, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Read the next available packet from serial (any tag).
        
        This is the raw read method used by listener threads. For command/response
        correlation, use send_and_wait() which auto-assigns tags and matches.
        
        Args:
            timeout: Override default timeout
        
        Returns:
            Response object, or None if timeout/error
        """
        response = self._read_packet(timeout)
        if response is None:
            self._log_timeout()
        return response
    
    def _read_packet(self, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Low-level: read a single packet from serial.
        
        Returns the next complete packet regardless of tag, or None on timeout.
        Does NOT log timeouts (caller decides).
        
        Args:
            timeout: Read timeout in seconds
        
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
                    packet_type, tag, payload = parsed
                    response = Response(packet_type, tag, payload, packet_data)
                    self._log_recv(response)
                    return response
            
            time.sleep(0.001)
        
        return None
    
    def _wait_for_tag(self, tag: int, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Wait for a response with a specific correlation tag.
        
        While waiting, async packets (tag=0) are dispatched to callbacks,
        and responses with other tags are stashed in self._pending for later.
        
        Args:
            tag: The correlation tag to wait for (1-255)
            timeout: Override default timeout
        
        Returns:
            Response with matching tag, or None on timeout
        """
        # Check if we already have it from a previous read
        if tag in self._pending:
            return self._pending.pop(tag)
        
        timeout = timeout or self.timeout
        deadline = time.time() + timeout
        
        while time.time() < deadline:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            
            response = self._read_packet(remaining)
            if response is None:
                break  # Timeout or error
            
            if response.tag == tag:
                return response
            elif response.tag == TAG_ASYNC:
                # Dispatch async to callbacks
                for cb in self._callbacks:
                    try:
                        cb(response)
                    except Exception:
                        pass
            else:
                # Stash out-of-order response for its tag
                self._pending[response.tag] = response
        
        self._log_timeout()
        return None
    
    def _inject_tag(self, data: bytes, tag: int) -> bytes:
        """
        Re-encode a COBS packet with a new correlation tag.
        
        Decodes the packet, replaces the tag, and re-encodes.
        
        Args:
            data: Original COBS-encoded packet
            tag: New tag value (1-255)
        
        Returns:
            Re-encoded packet with new tag, or original if decode fails
        """
        parsed = parse_packet(data)
        if not parsed:
            return data
        ptype, _old_tag, payload = parsed
        return build_packet(ptype, payload, tag=tag)
    
    def send_and_wait(self, data: bytes, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Send command with auto-assigned correlation tag and wait for matching response.
        
        Assigns a unique tag (1-255), re-encodes the packet with it, sends it,
        then blocks until a response with the same tag arrives or timeout.
        Async packets (tag=0) are dispatched to callbacks while waiting.
        Out-of-order responses with other tags are queued for later retrieval.
        
        Args:
            data: COBS encoded packet (tag will be replaced)
            timeout: Response timeout
        
        Returns:
            Response object with matching tag, or None if failed
        """
        tag = self.next_tag()
        tagged_data = self._inject_tag(data, tag)
        if not self.send(tagged_data):
            return None
        return self._wait_for_tag(tag, timeout)
    
    def send_and_receive(self, data: bytes, timeout: Optional[float] = None) -> Optional[Response]:
        """
        Send command and wait for any matching response (query pattern).
        
        Use for query commands that return typed data response packets
        (e.g., LED_SEQ_STATUS → LED_SEQ_STATUS_RESP). The caller checks
        response.packet_type to determine what was received.
        
        Equivalent to send_and_wait() — named for clarity at call sites
        where the expected response is a data packet, not ACK/NACK.
        
        Args:
            data: COBS encoded packet (tag will be auto-assigned)
            timeout: Response timeout
        
        Returns:
            Response object with matching tag, or None on timeout
        """
        return self.send_and_wait(data, timeout)
    
    def send_expect_ack(self, data: bytes, timeout: Optional[float] = None) -> Tuple[bool, Optional[Response]]:
        """
        Send command and expect ACK response.
        
        Uses tag-matched send_and_wait internally.
        
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
        """Drain any pending data from receive buffer and pending queue."""
        if self.is_connected:
            try:
                while self._serial.in_waiting:
                    self._serial.read(self._serial.in_waiting)
                    time.sleep(0.001)
            except:
                pass
        self._rx_buffer.clear()
        self._pending.clear()
    
    def add_callback(self, callback: Callable[[Response], None]):
        """Add callback for async responses (STATUS, ERROR).
        
        Idempotent — adding the same callback twice is a no-op.
        """
        if callback not in self._callbacks:
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
    
    Detects:
    - Raspberry Pi Pico (RP2040) — USB CDC serial
    - ESP32 dev boards — USB-UART bridge chips (CP210x, CH340, FTDI)
    
    Returns:
        List of (port, description) tuples
    """
    import serial.tools.list_ports
    ports = []
    for port in serial.tools.list_ports.comports():
        # Raspberry Pi Pico / RP2040 CDC
        if 'Pico' in port.description or 'RP2040' in port.description:
            ports.append((port.device, port.description))
        elif port.vid == 0x2E8A:  # Raspberry Pi VID
            ports.append((port.device, port.description))
        # ESP32 USB-UART bridges (CP210x, CH340, FTDI)
        elif port.vid == 0x10C4 and port.pid == 0xEA60:  # Silicon Labs CP210x
            ports.append((port.device, f"{port.description} (CP210x)"))
        elif port.vid == 0x1A86 and port.pid == 0x7523:  # WCH CH340
            ports.append((port.device, f"{port.description} (CH340)"))
        elif port.vid == 0x0403 and port.pid == 0x6001:  # FTDI FT232
            ports.append((port.device, f"{port.description} (FTDI)"))
    return ports
