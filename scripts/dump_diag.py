"""Quick script to dump diagnostic log from HubFX without split-screen UI."""
import sys
import struct
import time
sys.path.insert(0, '.')
from tests.framework.protocol import build_packet, parse_packet
from tests.framework.packets import CorePacket

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM14'
BAUD = 1000000

def main():
    ser = serial.Serial(PORT, BAUD, timeout=2)
    time.sleep(0.5)
    
    # Drain any pending data
    if ser.in_waiting:
        ser.read(ser.in_waiting)
    
    # Build DIAG_HISTORY packet
    packet = build_packet(CorePacket.DIAG_HISTORY, b'', tag=1)
    ser.write(packet)
    
    # Read all responses for up to 3 seconds
    deadline = time.time() + 3.0
    log_count = 0
    got_ack = False
    
    rx_buffer = b''
    while time.time() < deadline:
        avail = ser.in_waiting
        if avail > 0:
            rx_buffer += ser.read(avail)
        else:
            # Small blocking read to avoid busy-loop
            chunk = ser.read(1)
            if chunk:
                rx_buffer += chunk
            elif got_ack:
                break
            continue
        
        # Parse COBS frames (delimited by 0x00)
        while b'\x00' in rx_buffer:
            idx = rx_buffer.index(b'\x00')
            frame = rx_buffer[:idx]
            rx_buffer = rx_buffer[idx+1:]
            
            if len(frame) < 4:
                continue
            
            # parse_packet handles COBS decoding; pass frame directly
            parsed = parse_packet(frame)
            if not parsed:
                continue
            
            ptype, tag, payload = parsed
            
            if ptype == CorePacket.LOG_MESSAGE:
                log_count += 1
                if len(payload) >= 5:
                    level = payload[0]
                    ts_ms = struct.unpack('<I', payload[1:5])[0]
                    msg = payload[5:].decode('utf-8', errors='replace')
                    level_names = {0: 'TRACE', 1: 'DEBUG', 2: 'INFO', 3: 'WARN', 4: 'ERROR'}
                    lname = level_names.get(level, f'L{level}')
                    secs = ts_ms // 1000
                    ms = ts_ms % 1000
                    print(f"[{secs:6d}.{ms:03d}] {lname:5s} {msg}")
                else:
                    print(f"  LOG (short): {payload.hex()}")
            elif ptype == CorePacket.ACK:
                got_ack = True
                if len(payload) >= 2:
                    count = struct.unpack('<H', payload[:2])[0]
                    print(f"\n--- ACK: {count} messages in history ---")
                else:
                    print(f"\n--- ACK ---")
            elif ptype == CorePacket.NACK:
                got_ack = True
                code = payload[0] if payload else 0xFF
                print(f"\n--- NACK: 0x{code:02X} ---")
            else:
                print(f"  [ptype=0x{ptype:02X} tag={tag} len={len(payload)}]")
    
    ser.close()
    print(f"\nTotal log messages: {log_count}")

if __name__ == '__main__':
    main()
