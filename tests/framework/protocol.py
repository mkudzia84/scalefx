"""
COBS Protocol Implementation

Consistent Overhead Byte Stuffing (COBS) encoding/decoding and CRC-8 calculation.
"""

from typing import List, Optional


def crc8(data: bytes, poly: int = 0x07, init: int = 0x00) -> int:
    """
    Calculate CRC-8 checksum.
    
    Args:
        data: Input bytes
        poly: Polynomial (default 0x07)
        init: Initial value (default 0x00)
    
    Returns:
        CRC-8 checksum byte
    """
    crc = init
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ poly) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """
    COBS encode data.
    
    Replaces all 0x00 bytes with distance codes, prepends overhead byte.
    Result contains no 0x00 bytes (except optional delimiter).
    
    Args:
        data: Raw data to encode
    
    Returns:
        COBS encoded bytes (without 0x00 delimiter)
    """
    if len(data) == 0:
        return bytes([0x01])
    
    output = bytearray()
    code_idx = 0
    code = 1
    output.append(0)  # Placeholder for first code
    
    for byte in data:
        if byte == 0:
            output[code_idx] = code
            code_idx = len(output)
            output.append(0)  # Placeholder for next code
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
    return bytes(output)


def cobs_decode(data: bytes) -> Optional[bytes]:
    """
    COBS decode data.
    
    Args:
        data: COBS encoded bytes (without 0x00 delimiter)
    
    Returns:
        Decoded bytes, or None if invalid
    """
    if len(data) == 0:
        return None
    
    output = bytearray()
    idx = 0
    
    while idx < len(data):
        code = data[idx]
        if code == 0:
            return None  # Invalid: 0x00 in encoded data
        
        idx += 1
        
        for i in range(1, code):
            if idx >= len(data):
                return None  # Truncated
            output.append(data[idx])
            idx += 1
        
        if code < 0xFF and idx < len(data):
            output.append(0)
    
    # Remove trailing zero if present
    if len(output) > 0 and output[-1] == 0:
        output = output[:-1]
    
    return bytes(output)


def build_packet(packet_type: int, payload: bytes = b'') -> bytes:
    """
    Build a complete COBS-encoded packet.
    
    Packet structure (before COBS):
        [type:u8][len:u8][payload:0-64][crc8:u8]
    
    Args:
        packet_type: Packet type byte
        payload: Payload bytes (0-64)
    
    Returns:
        COBS encoded packet with 0x00 delimiter
    """
    if len(payload) > 64:
        raise ValueError(f"Payload too large: {len(payload)} > 64")
    
    raw = bytes([packet_type, len(payload)]) + payload
    raw += bytes([crc8(raw)])
    
    encoded = cobs_encode(raw)
    return encoded + b'\x00'


def parse_packet(data: bytes) -> Optional[tuple]:
    """
    Parse a COBS-encoded packet.
    
    Args:
        data: COBS encoded packet (with or without 0x00 delimiter)
    
    Returns:
        Tuple of (packet_type, payload) or None if invalid
    """
    # Remove delimiter if present
    if data.endswith(b'\x00'):
        data = data[:-1]
    
    decoded = cobs_decode(data)
    if decoded is None or len(decoded) < 3:
        return None
    
    packet_type = decoded[0]
    length = decoded[1]
    
    if len(decoded) != 3 + length:
        return None  # Length mismatch
    
    payload = decoded[2:2+length]
    received_crc = decoded[-1]
    
    # Verify CRC
    expected_crc = crc8(decoded[:-1])
    if received_crc != expected_crc:
        return None  # CRC mismatch
    
    return (packet_type, payload)


# Payload helper functions
def u16_le(value: int) -> bytes:
    """Pack unsigned 16-bit little-endian."""
    return bytes([value & 0xFF, (value >> 8) & 0xFF])


def i16_le(value: int) -> bytes:
    """Pack signed 16-bit little-endian."""
    if value < 0:
        value = value + 0x10000
    return u16_le(value)


def u32_le(value: int) -> bytes:
    """Pack unsigned 32-bit little-endian."""
    return bytes([
        value & 0xFF,
        (value >> 8) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 24) & 0xFF
    ])


def read_u16_le(data: bytes, offset: int = 0) -> int:
    """Read unsigned 16-bit little-endian."""
    return data[offset] | (data[offset + 1] << 8)


def read_i16_le(data: bytes, offset: int = 0) -> int:
    """Read signed 16-bit little-endian."""
    value = read_u16_le(data, offset)
    if value >= 0x8000:
        value -= 0x10000
    return value


def read_u32_le(data: bytes, offset: int = 0) -> int:
    """Read unsigned 32-bit little-endian."""
    return (data[offset] |
            (data[offset + 1] << 8) |
            (data[offset + 2] << 16) |
            (data[offset + 3] << 24))
