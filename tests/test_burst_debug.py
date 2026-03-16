#!/usr/bin/env python3
"""Minimal burst upload test for a single small file with verbose logging."""

import sys, os, time, hashlib
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tests.framework.connection import ScaleFXConnection
from tests.framework.packets import HubFxPacket, HubFxError, HubFxStorage, CoreError
from tests.framework.commands import HubFxCommands

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"

conn = ScaleFXConnection(port=PORT, verbose=False)
conn.connect(init=True)

# Cancel any stale upload
cancel = HubFxCommands.file_upload_cancel()
conn.send_expect_ack(cancel, timeout=5.0)
conn.drain()
time.sleep(0.5)

# Upload a tiny file (21 KB)
fpath = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "media", "sounds", "2A42", "gun_550rpm.wav")
with open(fpath, 'rb') as f:
    data = f.read()
print(f"\n=== Uploading {len(data)} bytes in burst mode ===")

# BEGIN
pkt = HubFxCommands.file_upload_begin("/test_burst.wav", len(data),
                                       mode=HubFxStorage.UPLOAD_BURST)
ok, resp = conn.send_expect_ack(pkt, timeout=10.0)
print(f"BEGIN: ok={ok}")
if not ok:
    if resp:
        print(f"  type=0x{resp.packet_type:02X} err={resp.error_code} "
              f"nack={resp.is_nack}")
    conn.close()
    sys.exit(1)

# DATA chunks with pacing
md5 = hashlib.md5()
offset = 0
seq = 0
CHUNK = 2044
while offset < len(data):
    chunk = data[offset:offset + CHUNK]
    pkt = HubFxCommands.file_upload_data(seq, chunk)
    md5.update(chunk)
    conn.send(pkt, flush=True)
    # No delay — testing with 64KB RX buffer
    offset += len(chunk)
    seq += 1
print(f"Sent {seq} chunks, flushing...")

try:
    conn._serial.flush()
except Exception:
    pass
wait = 0.5 + seq * 0.003
print(f"Waiting {wait:.2f}s for server...")
time.sleep(wait)

# END
pkt = HubFxCommands.file_upload_end()
ok, resp = conn.send_expect_ack(pkt, timeout=15.0)
print(f"END: ok={ok}")
if ok and resp and len(resp.payload) >= 16:
    remote_md5 = resp.payload[:16].hex()
    local_md5 = md5.hexdigest()
    print(f"  Local  MD5: {local_md5}")
    print(f"  Remote MD5: {remote_md5}")
    print(f"  Match: {local_md5 == remote_md5}")
elif resp:
    print(f"  type=0x{resp.packet_type:02X} nack={resp.is_nack} "
          f"err=0x{resp.error_code:02X} ({HubFxError.name(resp.error_code)}) "
          f"payload={resp.payload.hex() if resp.payload else 'empty'}")
else:
    print("  No response (timeout)")

# Cleanup
delete_pkt = HubFxCommands.file_delete("/test_burst.wav")
conn.send_expect_ack(delete_pkt, timeout=5.0)
conn.close()
print("Done")
