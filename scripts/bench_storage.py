"""
Storage Throughput Benchmark for ScaleFX HubFX

Measures upload (write) and download (read) speeds for both sync and burst
modes across a range of file sizes against the SD card.

Usage:
    python scripts/bench_storage.py [PORT] [--sizes SIZES] [--burst-only] [--sync-only]

Examples:
    python scripts/bench_storage.py COM16
    python scripts/bench_storage.py COM16 --sizes 10,100,1000
    python scripts/bench_storage.py COM16 --burst-only
"""
import sys
import os
import time
import hashlib
import argparse

# Allow running from repo root
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from tests.framework.connection import ScaleFXConnection
from tests.framework.protocol import build_packet, crc16_ccitt, read_u16_le, read_u32_le
from tests.framework.packets import (
    CorePacket, CoreError, HubFxPacket, HubFxStorage, HubFxError, StreamPacket
)
from tests.framework.commands import HubFxCommands


# =============================================================================
# Configuration
# =============================================================================

BENCH_DIR = '/_bench'               # Remote directory for test files
CHUNK_SIZE = 2044                   # MAX_PAYLOAD_SIZE(2048) - 4 (seq+crc header)
DEFAULT_SIZES_KB = [10, 100, 500, 1000, 5000]  # File sizes to test


# =============================================================================
# Helpers
# =============================================================================

def generate_test_data(size_bytes: int) -> bytes:
    """Generate pseudo-random test data of given size."""
    # Repeating pattern for fast generation + easy visual inspection
    block = bytes(range(256)) * 8  # 2KB block
    repeats = (size_bytes // len(block)) + 1
    return (block * repeats)[:size_bytes]


def fmt_speed(bytes_count: int, elapsed_s: float) -> str:
    """Format speed as KB/s."""
    if elapsed_s <= 0:
        return "∞"
    speed = bytes_count / elapsed_s / 1024
    return f"{speed:.1f} KB/s"


def fmt_size(size_bytes: int) -> str:
    """Format file size for display."""
    if size_bytes < 1024:
        return f"{size_bytes} B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes / 1024:.1f} KB"
    else:
        return f"{size_bytes / (1024 * 1024):.1f} MB"


def progress_bar(current: int, total: int, width: int = 30) -> str:
    """Simple progress bar string."""
    pct = current / total if total > 0 else 1.0
    filled = int(width * pct)
    bar = '█' * filled + '░' * (width - filled)
    return f"[{bar}] {pct * 100:5.1f}%"


# =============================================================================
# Upload (Write) Benchmark
# =============================================================================

def bench_upload(conn: ScaleFXConnection, data: bytes, remote_path: str,
                 burst: bool = False) -> dict:
    """
    Upload data to device and measure throughput.

    Returns dict with: success, elapsed_s, speed_kbs, md5_match, crc_errors
    """
    file_size = len(data)
    mode = HubFxStorage.UPLOAD_BURST if burst else HubFxStorage.UPLOAD_SYNC
    local_md5 = hashlib.md5(data).hexdigest()

    # --- UPLOAD_BEGIN ---
    pkt = HubFxCommands.file_upload_begin(remote_path, file_size,
                                           target=HubFxStorage.TARGET_SD,
                                           mode=mode)
    ok, resp = conn.send_expect_ack(pkt, timeout=10.0)
    if not ok:
        code = resp.error_code if resp else 0
        return {'success': False, 'error': f"BEGIN failed: {HubFxError.name(code)}"}

    # --- UPLOAD_DATA chunks ---
    offset = 0
    seq = 0
    t_start = time.perf_counter()

    while offset < file_size:
        chunk = data[offset:offset + CHUNK_SIZE]
        pkt = HubFxCommands.file_upload_data(seq, chunk)

        if burst:
            if not conn.send(pkt, flush=False):
                return {'success': False, 'error': f"Send failed at seq {seq}"}
        else:
            ok, resp = conn.send_expect_ack(pkt, timeout=10.0)
            if not ok:
                code = resp.error_code if resp else 0
                # Cancel on failure
                conn.send_expect_ack(HubFxCommands.file_upload_cancel(), timeout=5.0)
                return {'success': False, 'error': f"DATA seq {seq} failed: {HubFxError.name(code)}"}

        offset += len(chunk)
        seq += 1

    # Flush OS TX buffer before END (burst mode)
    if burst:
        try:
            conn._serial.flush()
        except Exception:
            pass
        time.sleep(0.1)  # Let firmware finish processing buffered chunks

    # --- UPLOAD_END ---
    pkt = HubFxCommands.file_upload_end()
    ok, resp = conn.send_expect_ack(pkt, timeout=15.0)
    t_end = time.perf_counter()

    elapsed = t_end - t_start
    result = {
        'success': ok,
        'elapsed_s': elapsed,
        'speed_kbs': file_size / elapsed / 1024 if elapsed > 0 else 0,
        'md5_match': False,
        'crc_errors': 0,
    }

    if ok and resp and len(resp.payload) >= 16:
        remote_md5 = resp.payload[:16].hex()
        result['md5_match'] = (remote_md5 == local_md5)
        if len(resp.payload) >= 18:
            result['crc_errors'] = int.from_bytes(resp.payload[16:18], 'little')
    elif not ok:
        code = resp.error_code if resp else 0
        result['error'] = f"END failed: {HubFxError.name(code)}"

    return result


# =============================================================================
# Download (Read) Benchmark
# =============================================================================

def bench_download(conn: ScaleFXConnection, remote_path: str,
                   expected_size: int) -> dict:
    """
    Download a file from device and measure throughput.

    Returns dict with: success, elapsed_s, speed_kbs, size_match, crc_ok
    """
    pkt = HubFxCommands.file_download(remote_path, target=HubFxStorage.TARGET_SD)
    tag = conn.next_tag()
    tagged = conn._inject_tag(pkt, tag)

    t_start = time.perf_counter()
    if not conn.send(tagged):
        return {'success': False, 'error': "Send failed"}

    # Receive stream: BEGIN → DATA* → END
    data = bytearray()
    total_expected = 0
    crc_errors = 0

    while True:
        response = conn._wait_for_tag(tag, timeout=30.0)
        if response is None:
            return {'success': False, 'error': "Stream timeout"}

        if response.is_nack:
            code = response.error_code
            return {'success': False, 'error': f"NACK: {HubFxError.name(code)}"}

        if response.packet_type == StreamPacket.STREAM_BEGIN:
            if len(response.payload) >= 4:
                total_expected = read_u32_le(response.payload, 0)

        elif response.packet_type == StreamPacket.STREAM_DATA:
            if len(response.payload) < 4:
                continue
            crc_rx = read_u16_le(response.payload, 2)
            chunk = response.payload[4:]
            if crc16_ccitt(chunk) != crc_rx:
                crc_errors += 1
            data.extend(chunk)

        elif response.packet_type == StreamPacket.STREAM_END:
            break

    t_end = time.perf_counter()
    elapsed = t_end - t_start

    return {
        'success': True,
        'elapsed_s': elapsed,
        'speed_kbs': len(data) / elapsed / 1024 if elapsed > 0 else 0,
        'size_match': len(data) == expected_size,
        'crc_errors': crc_errors,
        'bytes_received': len(data),
    }


# =============================================================================
# Cleanup
# =============================================================================

def delete_file(conn: ScaleFXConnection, path: str) -> bool:
    """Delete a file on the device."""
    pkt = HubFxCommands.file_delete(path, target=HubFxStorage.TARGET_SD)
    ok, _ = conn.send_expect_ack(pkt, timeout=10.0)
    return ok


def ensure_bench_dir(conn: ScaleFXConnection):
    """Create the benchmark directory if it doesn't exist."""
    pkt = HubFxCommands.file_mkdir(BENCH_DIR, target=HubFxStorage.TARGET_SD)
    conn.send_expect_ack(pkt, timeout=5.0)  # Ignore error if already exists


def cleanup_bench_dir(conn: ScaleFXConnection):
    """Delete the benchmark directory and all contents."""
    pkt = HubFxCommands.file_delete(BENCH_DIR, target=HubFxStorage.TARGET_SD)
    conn.send_expect_ack(pkt, timeout=10.0)


# =============================================================================
# Main Benchmark Runner
# =============================================================================

def run_benchmark(port: str, sizes_kb: list, modes: list):
    """Run the full storage benchmark suite."""
    conn = ScaleFXConnection(port=port)

    print("=" * 70)
    print("  ScaleFX Storage Throughput Benchmark")
    print("=" * 70)
    print(f"  Port:   {port}")
    print(f"  Sizes:  {', '.join(fmt_size(s * 1024) for s in sizes_kb)}")
    print(f"  Modes:  {', '.join(modes)}")
    print()

    # Connect
    print("  Connecting...", end=' ', flush=True)
    if not conn.connect(init=False):
        print("FAILED")
        return 1
    print("OK")

    # Initialize
    print("  Initializing...", end=' ', flush=True)
    if not conn.initialize():
        print("FAILED")
        conn.close()
        return 1

    dev = conn.device_name or "Unknown"
    ver = conn.device_version or "?"
    build = conn.device_build or 0
    print(f"OK ({dev} v{ver} build {build})")
    print()

    # Create benchmark directory
    ensure_bench_dir(conn)

    # Results table
    results = []

    for size_kb in sizes_kb:
        size_bytes = size_kb * 1024
        data = generate_test_data(size_bytes)
        remote_path = f"{BENCH_DIR}/test_{size_kb}k.bin"

        for mode in modes:
            burst = (mode == 'burst')
            label = f"{fmt_size(size_bytes):>8s} {mode:>5s}"
            print(f"  {label}  ", end='', flush=True)

            # --- Upload ---
            print("W:", end='', flush=True)
            up = bench_upload(conn, data, remote_path, burst=burst)

            if up['success']:
                md5_ok = "✓" if up.get('md5_match') else "✗"
                crc_note = f" ({up['crc_errors']} CRC errs)" if up.get('crc_errors', 0) > 0 else ""
                print(f" {up['speed_kbs']:7.1f} KB/s md5={md5_ok}{crc_note}", end='  ', flush=True)
            else:
                print(f" FAILED ({up.get('error', '?')})", end='  ', flush=True)
                # Try to clean up
                conn.send_expect_ack(HubFxCommands.file_upload_cancel(), timeout=2.0)
                results.append({
                    'size_kb': size_kb, 'mode': mode,
                    'write_kbs': 0, 'read_kbs': 0,
                    'write_ok': False, 'read_ok': False,
                })
                print()
                continue

            # --- Download ---
            print("R:", end='', flush=True)
            down = bench_download(conn, remote_path, size_bytes)

            if down['success']:
                size_ok = "✓" if down.get('size_match') else "✗"
                crc_note = f" ({down['crc_errors']} CRC errs)" if down.get('crc_errors', 0) > 0 else ""
                print(f" {down['speed_kbs']:7.1f} KB/s size={size_ok}{crc_note}")
            else:
                print(f" FAILED ({down.get('error', '?')})")

            results.append({
                'size_kb': size_kb, 'mode': mode,
                'write_kbs': up['speed_kbs'] if up['success'] else 0,
                'read_kbs': down['speed_kbs'] if down['success'] else 0,
                'write_ok': up['success'],
                'read_ok': down['success'],
                'md5_match': up.get('md5_match', False),
                'crc_errors': up.get('crc_errors', 0) + down.get('crc_errors', 0),
            })

            # Clean up test file
            delete_file(conn, remote_path)

    # Cleanup
    cleanup_bench_dir(conn)

    # Summary table
    print()
    print("=" * 70)
    print("  RESULTS SUMMARY")
    print("=" * 70)
    print(f"  {'Size':>8s}  {'Mode':>5s}  {'Write KB/s':>11s}  {'Read KB/s':>10s}  {'MD5':>3s}  {'CRC Errs':>8s}")
    print(f"  {'─' * 8}  {'─' * 5}  {'─' * 11}  {'─' * 10}  {'─' * 3}  {'─' * 8}")

    for r in results:
        w = f"{r['write_kbs']:7.1f}" if r['write_ok'] else "  FAIL"
        rd = f"{r['read_kbs']:7.1f}" if r['read_ok'] else "  FAIL"
        md5 = " ✓ " if r.get('md5_match') else " ✗ "
        crc = str(r.get('crc_errors', 0))
        print(f"  {fmt_size(r['size_kb'] * 1024):>8s}  {r['mode']:>5s}  {w:>8s}     {rd:>7s}     {md5}  {crc:>8s}")

    # Average speeds
    write_speeds = [r['write_kbs'] for r in results if r['write_ok']]
    read_speeds = [r['read_kbs'] for r in results if r['read_ok']]

    print(f"  {'─' * 8}  {'─' * 5}  {'─' * 11}  {'─' * 10}  {'─' * 3}  {'─' * 8}")
    if write_speeds:
        print(f"  {'Avg':>8s}  {'':>5s}  {sum(write_speeds)/len(write_speeds):7.1f}     "
              f"{sum(read_speeds)/len(read_speeds) if read_speeds else 0:7.1f}")
    print()

    conn.close()
    return 0


# =============================================================================
# CLI
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='ScaleFX Storage Throughput Benchmark',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python scripts/bench_storage.py COM16
  python scripts/bench_storage.py COM16 --sizes 10,100,500
  python scripts/bench_storage.py COM16 --burst-only
  python scripts/bench_storage.py COM16 --sync-only
""")
    parser.add_argument('port', nargs='?', default='COM16',
                        help='Serial port (default: COM16)')
    parser.add_argument('--sizes', type=str, default=None,
                        help='Comma-separated file sizes in KB (default: 10,100,500,1000,5000)')
    parser.add_argument('--burst-only', action='store_true',
                        help='Only test burst mode')
    parser.add_argument('--sync-only', action='store_true',
                        help='Only test sync mode')

    args = parser.parse_args()

    # Parse sizes
    if args.sizes:
        sizes_kb = [int(s.strip()) for s in args.sizes.split(',')]
    else:
        sizes_kb = DEFAULT_SIZES_KB

    # Determine modes
    if args.burst_only:
        modes = ['burst']
    elif args.sync_only:
        modes = ['sync']
    else:
        modes = ['sync', 'burst']

    sys.exit(run_benchmark(args.port, sizes_kb, modes))


if __name__ == '__main__':
    main()
