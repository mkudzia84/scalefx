"""Quick firmware recovery: send UPLOAD_CANCEL + IDENTIFY to unstick firmware."""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def cobs_encode(data):
    out = bytearray()
    code_idx = 0
    code = 1
    out.append(0)
    for b in data:
        if b == 0:
            out[code_idx] = code
            code_idx = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx = len(out)
                out.append(0)
                code = 1
    out[code_idx] = code
    return bytes(out)

def build_packet(ptype, tag=1):
    raw = bytes([ptype, tag, 0x00, 0x00])
    raw += bytes([crc8(raw)])
    return cobs_encode(raw) + b'\x00'

s = serial.Serial(PORT, 6000000, timeout=0.1, write_timeout=1.0)
time.sleep(0.5)

# Drain
while s.in_waiting:
    s.read(s.in_waiting)
    time.sleep(0.01)

print("Sending UPLOAD_CANCEL (0xA3)...")
s.write(build_packet(0xA3, tag=1))
s.flush()
time.sleep(1)
while s.in_waiting:
    data = s.read(s.in_waiting)
    print(f"  RX: {data.hex()}")

print("Sending IDENTIFY (0xFE)...")
s.write(build_packet(0xFE, tag=2))
s.flush()
time.sleep(1)
while s.in_waiting:
    data = s.read(s.in_waiting)
    print(f"  IDENTIFY RX ({len(data)}B): {data[:64].hex()}")

s.close()
print("Done")
