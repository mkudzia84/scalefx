#!/usr/bin/env python3
"""
Standalone burst upload test tool for troubleshooting stream mode timing.

Connects to a ScaleFX HubFX controller, generates dummy data, and uploads
it via burst (stream) mode to /tests/ on the SD card.  Provides detailed
timing diagnostics at every stage.

Usage:
    python scripts/test_burst_upload.py --port COM16
    python scripts/test_burst_upload.py --port COM16 --size 2M
    python scripts/test_burst_upload.py --port COM16 --size 500K --throttle 300
    python scripts/test_burst_upload.py --port COM16 --size 17M --throttle 0
"""

import argparse
import hashlib
import os
import struct
import sys
import time

import serial

# ---------------------------------------------------------------------------
# Protocol helpers (self-contained — no dependency on tests.framework)
# ---------------------------------------------------------------------------

CRC8_POLY = 0x07

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ CRC8_POLY) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def cobs_encode(data: bytes) -> bytes:
    if not data:
        return bytes([0x01])
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

def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        for _ in range(1, code):
            if i >= len(data):
                return bytes(out)
            out.append(data[i])
            i += 1
        if code < 0xFF and i < len(data):
            out.append(0)
    if out and out[-1] == 0:
        out = out[:-1]
    return bytes(out)

def build_packet(ptype: int, payload: bytes = b'', tag: int = 0) -> bytes:
    plen = len(payload)
    header = bytes([ptype, tag]) + struct.pack('<H', plen)
    raw = header + payload
    raw += bytes([crc8(raw)])
    return cobs_encode(raw) + b'\x00'

def parse_packet(data: bytes):
    """Returns (ptype, tag, payload) or None."""
    if not data:
        return None
    if data[-1:] == b'\x00':
        data = data[:-1]
    decoded = cobs_decode(data)
    if len(decoded) < 5:
        return None
    ptype = decoded[0]
    tag = decoded[1]
    plen = struct.unpack('<H', decoded[2:4])[0]
    if len(decoded) < 4 + plen + 1:
        return None
    payload = decoded[4:4 + plen]
    expected_crc = crc8(decoded[:4 + plen])
    if decoded[4 + plen] != expected_crc:
        return None
    return (ptype, tag, payload)

# Packet type constants (must match CorePacket in packets.py / core.h)
INIT        = 0xF0
KEEPALIVE   = 0xF2
INIT_READY  = 0xF3
STATUS      = 0xF4
ACK         = 0xF6
NACK        = 0xF7
IDENTIFY    = 0xFE

FILE_UPLOAD_BEGIN  = 0xA0
FILE_UPLOAD_END    = 0xA2
FILE_UPLOAD_CANCEL = 0xA3

# HubFxStorage
TARGET_SD     = 0
UPLOAD_STREAM = 3

# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

class Connection:
    """Minimal serial connection with tag correlation."""

    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.ser: serial.Serial = None
        self._rx_buf = bytearray()
        self._tag = 0

    def open(self):
        self.ser = serial.Serial(
            port=self.port, baudrate=self.baud,
            timeout=0.1, write_timeout=1.0)
        try:
            self.ser.set_buffer_size(rx_size=131072, tx_size=131072)
        except Exception:
            pass
        # Wait for ESP32 boot output to finish
        time.sleep(0.5)
        self._drain()

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def _drain(self):
        while self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
            time.sleep(0.01)
        self._rx_buf.clear()

    def next_tag(self) -> int:
        self._tag = (self._tag % 255) + 1
        return self._tag

    def _inject_tag(self, packet: bytes, tag: int) -> bytes:
        parsed = parse_packet(packet)
        if not parsed:
            return packet
        ptype, _, payload = parsed
        return build_packet(ptype, payload, tag=tag)

    def send_tagged(self, packet: bytes, timeout: float = 5.0):
        """Send packet with auto tag, wait for matching response.
        Returns (ptype, tag, payload) or None."""
        tag = self.next_tag()
        data = self._inject_tag(packet, tag)
        self.ser.write(data)
        self.ser.flush()
        return self._wait_tag(tag, timeout)

    def _wait_tag(self, tag: int, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            pkt = self._read_packet(deadline - time.time())
            if pkt is None:
                return None
            ptype, ptag, payload = pkt
            if ptag == tag:
                return pkt
            # else discard (async/other tags)
        return None

    def _read_packet(self, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if self.ser.in_waiting:
                    self._rx_buf.extend(self.ser.read(self.ser.in_waiting))
            except Exception:
                return None
            if 0x00 in self._rx_buf:
                idx = self._rx_buf.index(0x00)
                raw = bytes(self._rx_buf[:idx])
                self._rx_buf = self._rx_buf[idx + 1:]
                parsed = parse_packet(raw + b'\x00')
                if parsed:
                    return parsed
            time.sleep(0.001)
        return None

# ---------------------------------------------------------------------------
# Protocol actions
# ---------------------------------------------------------------------------

def do_identify(conn: Connection) -> dict:
    """Send IDENTIFY and parse response."""
    pkt = build_packet(IDENTIFY)
    resp = conn.send_tagged(pkt, timeout=5.0)
    if resp is None:
        return None
    ptype, _, payload = resp
    if ptype not in (INIT_READY, IDENTIFY):
        return None
    return _parse_init_ready(payload)

def do_init(conn: Connection) -> dict:
    """Send INIT and parse INIT_READY."""
    pkt = build_packet(INIT)
    resp = conn.send_tagged(pkt, timeout=5.0)
    if resp is None:
        return None
    ptype, _, payload = resp
    if ptype != INIT_READY:
        return None
    return _parse_init_ready(payload)

def _parse_init_ready(payload: bytes) -> dict:
    try:
        off = 0
        nl = payload[off]; off += 1
        name = payload[off:off+nl].decode(); off += nl
        vl = payload[off]; off += 1
        ver = payload[off:off+vl].decode(); off += vl
        pl = payload[off]; off += 1
        plat = payload[off:off+pl].decode(); off += pl
        cpu = struct.unpack_from('<I', payload, off)[0]; off += 4
        ram = struct.unpack_from('<I', payload, off)[0]; off += 4
        build = struct.unpack_from('<I', payload, off)[0]; off += 4
        return dict(name=name, version=ver, platform=plat,
                    cpu_mhz=cpu, free_ram=ram, build=build)
    except Exception:
        return None

def upload_begin(conn: Connection, path: str, size: int) -> bool:
    """Send UPLOAD_BEGIN (stream mode). Returns True on ACK."""
    path_bytes = path.encode('utf-8')
    payload = struct.pack('<I', size) + bytes([len(path_bytes)]) + path_bytes
    payload += bytes([TARGET_SD, UPLOAD_STREAM])
    pkt = build_packet(FILE_UPLOAD_BEGIN, payload)
    resp = conn.send_tagged(pkt, timeout=10.0)
    if resp is None:
        print("  [FAIL] UPLOAD_BEGIN: no response (timeout)")
        return False
    ptype, _, rpayload = resp
    if ptype == ACK:
        return True
    if ptype == NACK:
        code = rpayload[0] if rpayload else 0
        print(f"  [FAIL] UPLOAD_BEGIN: NACK 0x{code:02X}")
    else:
        print(f"  [FAIL] UPLOAD_BEGIN: unexpected 0x{ptype:02X}")
    return False

def upload_end(conn: Connection, timeout: float = 15.0):
    """Send UPLOAD_END. Returns (success, payload) or (False, None)."""
    pkt = build_packet(FILE_UPLOAD_END)
    resp = conn.send_tagged(pkt, timeout=timeout)
    if resp is None:
        return (False, None)
    ptype, _, payload = resp
    if ptype == ACK:
        return (True, payload)
    if ptype == NACK:
        code = payload[0] if payload else 0
        print(f"  [FAIL] UPLOAD_END: NACK 0x{code:02X}")
    else:
        print(f"  [FAIL] UPLOAD_END: unexpected 0x{ptype:02X}")
    return (False, None)

def upload_cancel(conn: Connection):
    pkt = build_packet(FILE_UPLOAD_CANCEL)
    conn.send_tagged(pkt, timeout=5.0)

# ---------------------------------------------------------------------------
# Throttled write helper
# ---------------------------------------------------------------------------

def throttled_write(ser: serial.Serial, data: bytes, block_size: int,
                    max_rate_kbps: int, on_progress=None) -> float:
    """
    Write data to serial port with optional throughput throttling.

    Args:
        ser:           Open serial port
        data:          Raw bytes to write
        block_size:    Bytes per write() call
        max_rate_kbps: Max KB/s (0 = unlimited)
        on_progress:   Callback(offset, total, elapsed)

    Returns:
        Seconds elapsed for the write loop
    """
    total = len(data)
    offset = 0
    bytes_written = 0  # actual bytes accepted by serial driver
    short_writes = 0
    t0 = time.time()

    while offset < total:
        end = min(offset + block_size, total)
        chunk = data[offset:end]

        t_write_start = time.time()
        n = ser.write(chunk)
        t_write_end = time.time()

        if n != len(chunk):
            short_writes += 1
            print(f"\n  [WARN] Short write: sent {n}/{len(chunk)} at offset {offset}")
        bytes_written += n
        offset = end

        if on_progress:
            on_progress(offset, total, time.time() - t0)

        # Throttle: sleep if we're ahead of the target rate
        if max_rate_kbps > 0:
            elapsed = time.time() - t0
            target_elapsed = offset / (max_rate_kbps * 1024)
            if elapsed < target_elapsed:
                time.sleep(target_elapsed - elapsed)

    elapsed = time.time() - t0
    if short_writes > 0:
        print(f"\n  [WARN] {short_writes} short writes! bytes_written={bytes_written} vs expected={total}")
    return elapsed, bytes_written

# ---------------------------------------------------------------------------
# Progress bar
# ---------------------------------------------------------------------------

def progress_bar(offset, total, elapsed):
    pct = offset / total * 100 if total else 0
    bar_len = 40
    filled = int(bar_len * offset / total)
    bar = '█' * filled + '░' * (bar_len - filled)
    speed = offset / elapsed / 1024 if elapsed > 0 else 0
    print(f"\r  [{bar}] {pct:5.1f}%  {offset:>10,}/{total:,}  {speed:.0f} KB/s  ", end='', flush=True)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_size(s: str) -> int:
    """Parse size like '2M', '500K', '1024'."""
    s = s.strip().upper()
    if s.endswith('M'):
        return int(float(s[:-1]) * 1024 * 1024)
    if s.endswith('K'):
        return int(float(s[:-1]) * 1024)
    return int(s)

def generate_dummy_data(size: int) -> bytes:
    """Generate repeating pattern data."""
    # 256-byte pattern repeated
    pattern = bytes(range(256))
    repeats = (size // 256) + 1
    return (pattern * repeats)[:size]

def main():
    parser = argparse.ArgumentParser(
        description='Burst upload test tool for ScaleFX HubFX')
    parser.add_argument('--port', '-p', required=True, help='Serial port (e.g. COM16)')
    parser.add_argument('--baud', '-b', type=int, default=6_000_000, help='Baud rate (default: 6000000)')
    parser.add_argument('--size', '-s', default='2M', help='Upload size (e.g. 500K, 2M, 17M)')
    parser.add_argument('--throttle', '-t', type=int, default=300,
                        help='Max write speed in KB/s (0=unlimited, default=300)')
    parser.add_argument('--block', type=int, default=65536,
                        help='Write block size in bytes (default=65536)')
    parser.add_argument('--drain-delay', type=float, default=1.0,
                        help='Seconds to wait after write loop before UPLOAD_END (default=1.0)')
    parser.add_argument('--end-timeout', type=float, default=30.0,
                        help='UPLOAD_END response timeout in seconds (default=30)')
    parser.add_argument('--remote-dir', default='/tests',
                        help='Remote directory on SD (default=/tests)')
    parser.add_argument('--skip-init', action='store_true',
                        help='Skip INIT (use IDENTIFY only)')
    args = parser.parse_args()

    file_size = parse_size(args.size)
    remote_path = f"{args.remote_dir}/burst_test_{file_size}.bin"

    print("=" * 70)
    print("  ScaleFX Burst Upload Test")
    print("=" * 70)
    print(f"  Port:          {args.port}")
    print(f"  Baud:          {args.baud:,}")
    print(f"  File size:     {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")
    print(f"  Write block:   {args.block:,} bytes")
    print(f"  Throttle:      {'unlimited' if args.throttle == 0 else f'{args.throttle} KB/s'}")
    print(f"  Drain delay:   {args.drain_delay}s")
    print(f"  End timeout:   {args.end_timeout}s")
    print(f"  Remote path:   {remote_path}")
    print("=" * 70)

    # Generate data
    print("\n[1] Generating dummy data...")
    t0 = time.time()
    data = generate_dummy_data(file_size)
    local_md5 = hashlib.md5(data).hexdigest()
    print(f"  Generated {len(data):,} bytes in {time.time()-t0:.2f}s")
    print(f"  MD5: {local_md5}")

    # Connect
    print(f"\n[2] Connecting to {args.port} @ {args.baud:,} baud...")
    conn = Connection(args.port, args.baud)
    try:
        conn.open()
    except Exception as e:
        print(f"  [FAIL] {e}")
        return 1

    # Identify
    print("\n[3] Identifying device...")
    info = do_identify(conn)
    if info:
        print(f"  Name:     {info['name']}")
        print(f"  Version:  {info['version']}")
        print(f"  Platform: {info['platform']}")
        print(f"  CPU:      {info['cpu_mhz']} MHz")
        print(f"  RAM:      {info['free_ram']:,} bytes free")
        print(f"  Build:    {info['build']}")
    else:
        print("  [WARN] IDENTIFY failed, trying INIT...")
        args.skip_init = False

    # Init
    if not args.skip_init:
        print("\n[4] Sending INIT...")
        info2 = do_init(conn)
        if info2:
            print(f"  INIT_READY received (build {info2.get('build', '?')})")
        else:
            print("  [FAIL] No INIT_READY response")
            conn.close()
            return 1
    else:
        print("\n[4] Skipping INIT (--skip-init)")

    # Upload begin
    print(f"\n[5] UPLOAD_BEGIN → {remote_path} ({file_size:,} bytes, stream mode)...")
    if not upload_begin(conn, remote_path, file_size):
        conn.close()
        return 1
    print("  ACK received — server is in stream mode")

    # Write data
    throttle_str = f"throttled to {args.throttle} KB/s" if args.throttle else "unlimited"
    print(f"\n[6] Writing {file_size:,} bytes ({throttle_str})...")
    t_write_start = time.time()

    write_elapsed, bytes_written = throttled_write(
        conn.ser, data, args.block, args.throttle,
        on_progress=progress_bar)

    t_write_end = time.time()
    actual_rate = file_size / write_elapsed / 1024 if write_elapsed > 0 else 0
    print()  # newline after progress
    print(f"  Write loop: {write_elapsed:.2f}s, {actual_rate:.0f} KB/s actual")
    print(f"  Bytes accepted by serial driver: {bytes_written:,} / {file_size:,}")
    if bytes_written != file_size:
        print(f"  [FAIL] Serial driver rejected {file_size - bytes_written:,} bytes!")

    # Flush
    print(f"\n[7] Flushing serial TX buffer...")
    t_flush0 = time.time()
    try:
        conn.ser.flush()
    except Exception:
        pass
    t_flush1 = time.time()
    print(f"  flush() took {t_flush1 - t_flush0:.3f}s")

    # Drain delay
    print(f"\n[8] Waiting {args.drain_delay}s for wire drain + firmware processing...")
    rx_before = conn.ser.in_waiting if conn.ser else 0
    print(f"  RX bytes waiting before sleep: {rx_before}")
    time.sleep(args.drain_delay)
    rx_after_drain = conn.ser.in_waiting if conn.ser else 0
    print(f"  RX bytes waiting after sleep:  {rx_after_drain}")

    # Check for firmware messages that arrived during drain
    if rx_after_drain > 0:
        peek = conn.ser.read(min(rx_after_drain, 128))
        print(f"  RX peek ({len(peek)}B): {peek[:64].hex(' ')}{'...' if len(peek)>64 else ''}")
        # Put it back into the rx buffer for tag matching
        conn._rx_buf.extend(peek)

    # Upload end
    t_total_now = time.time() - t_write_start
    print(f"\n[9] Sending UPLOAD_END (t+{t_total_now:.1f}s since write start, timeout={args.end_timeout}s)...")
    t_end_start = time.time()
    success, end_payload = upload_end(conn, timeout=args.end_timeout)
    t_end_done = time.time()
    end_waited = t_end_done - t_end_start

    total_elapsed = t_end_done - t_write_start

    if success:
        print(f"  ACK received in {end_waited:.2f}s")
        if end_payload and len(end_payload) >= 16:
            remote_md5 = end_payload[:16].hex()
            md5_match = "✓ MATCH" if remote_md5 == local_md5 else "✗ MISMATCH"
            print(f"  Remote MD5: {remote_md5}  {md5_match}")
            print(f"  Local MD5:  {local_md5}")
        if end_payload and len(end_payload) >= 18:
            crc_errors = struct.unpack_from('<H', end_payload, 16)[0]
            if crc_errors:
                print(f"  CRC errors reported: {crc_errors}")
        speed = file_size / total_elapsed / 1024
        print(f"\n  Total: {file_size:,} bytes in {total_elapsed:.1f}s ({speed:.0f} KB/s)")
        print("  RESULT: SUCCESS")
    else:
        print(f"  UPLOAD_END failed after {end_waited:.1f}s")
        rx_pending = conn.ser.in_waiting if conn.ser else 0
        print(f"  RX bytes pending now: {rx_pending}")
        if rx_pending > 0:
            dump = conn.ser.read(min(rx_pending, 256))
            print(f"  RX dump ({len(dump)}B): {dump[:64].hex(' ')}{'...' if len(dump)>64 else ''}")

        # Wait for firmware stall timeout (5s) + margin, then check recovery
        print(f"\n[10] Waiting 8s for firmware stall timeout to fire...")
        time.sleep(8)
        rx_after_stall = conn.ser.in_waiting if conn.ser else 0
        print(f"  RX bytes after stall wait: {rx_after_stall}")
        if rx_after_stall > 0:
            dump2 = conn.ser.read(min(rx_after_stall, 256))
            print(f"  RX dump: {dump2[:64].hex(' ')}{'...' if len(dump2)>64 else ''}")
            conn._rx_buf.extend(dump2)

        print(f"  Sending IDENTIFY to check firmware recovery...")
        info = do_identify(conn)
        if info:
            print(f"  Firmware recovered: {info['name']} v{info['version']} build {info['build']}")
        else:
            print(f"  Firmware still unresponsive — stuck in stream mode")
            # Try waiting even longer in case the stall timer simply hasn't fired
            print(f"  Waiting another 10s...")
            time.sleep(10)
            conn._drain()
            info2 = do_identify(conn)
            if info2:
                print(f"  Firmware recovered after extended wait: {info2['name']}")
            else:
                print(f"  Firmware permanently stuck")

        print(f"\n  RESULT: FAILED")

    # Summary
    print("\n" + "=" * 70)
    print("  Timing Summary")
    print("=" * 70)
    print(f"  Write loop:     {write_elapsed:.2f}s")
    print(f"  Flush:          {t_flush1 - t_flush0:.3f}s")
    print(f"  Drain delay:    {args.drain_delay}s")
    print(f"  UPLOAD_END RTT: {end_waited:.2f}s")
    print(f"  Total:          {total_elapsed:.1f}s")
    print(f"  Effective rate:  {file_size / total_elapsed / 1024:.0f} KB/s" if total_elapsed > 0 else "")
    print("=" * 70)

    # Cleanup
    if not success:
        print("\n  Sending UPLOAD_CANCEL to clean up...")
        upload_cancel(conn)
        conn._drain()

    conn.close()
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
