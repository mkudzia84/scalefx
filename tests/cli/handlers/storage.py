"""
Storage Handler - Reusable file operations for SD/flash targets.

Provides a complete set of file management commands (list, tree, upload,
download, delete, mkdir, info, cat, cancel) parameterized by storage target.
Used by HubFX handler to expose sd.* and flash.* command groups.

Upload supports two modes:
- **sync** (default): ACK per chunk with CRC retry.  Reliable but slower.
- **stream** (--stream): Raw binary streaming with segment-based ACKs.
  Bypasses COBS framing for data plane.  Maximum throughput.
"""

import os
import struct
import time
import hashlib
from typing import List, Dict, Tuple, Callable, Optional

from tests.framework import (
    HubFxCommands, HubFxPacket, HubFxError, HubFxStorage, StreamPacket,
)
from tests.framework.protocol import read_u16_le, read_u32_le, crc16_ccitt
from ..base import (
    CommandHandlerBase, CommandInfo, ControllerType,
    Fore, Style, Spinner, format_progress_bar,
)
from .. import parsers


class StorageHandler(CommandHandlerBase):
    """
    Reusable file-operation handler for a single storage target.

    Instances are created by a parent handler (e.g. HubFxCommandHandler)
    for each target (SD card, flash).  Connection and cancel-event
    lifecycle is managed by the parent via ``set_connection()`` /
    ``set_cancel_event()``.
    """

    def __init__(self, prefix: str, target: int, target_name: str,
                 group_name: str, controller_type: str):
        """
        Args:
            prefix:          Command prefix (e.g. "sd", "flash")
            target:          HubFxStorage.TARGET_SD or TARGET_FLASH
            target_name:     Display name ("SD" or "flash")
            group_name:      Help-group label ("SD Card & Files")
            controller_type: ControllerType constant
        """
        super().__init__()
        self._prefix = prefix
        self._target = target
        self._target_name = target_name
        self._group_name = group_name
        self._ct = controller_type

    # --------------------------------------------------------------------- #
    #  Display helper
    # --------------------------------------------------------------------- #

    @property
    def _dp(self) -> str:
        """Display prefix for paths (e.g. '' for SD, 'flash:' for flash)."""
        if self._target != HubFxStorage.TARGET_SD:
            return f"{self._target_name.lower()}:"
        return ""

    # --------------------------------------------------------------------- #
    #  Command registry
    # --------------------------------------------------------------------- #

    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Generate prefixed command entries for this storage target."""
        p = self._prefix
        g = self._group_name
        ct = self._ct
        return {
            f'{p}.ls': (self.cmd_list, CommandInfo(
                f'{p}.ls', f'{p}.ls [path]',
                'List directory contents',
                requires_init=True, controller=ct, group=g)),
            f'{p}.tree': (self.cmd_tree, CommandInfo(
                f'{p}.tree', f'{p}.tree [path]',
                'Show recursive directory tree',
                requires_init=True, controller=ct, group=g)),
            f'{p}.rm': (self.cmd_delete, CommandInfo(
                f'{p}.rm', f'{p}.rm <path>',
                'Delete a file or directory (recursive)',
                requires_init=True, controller=ct, group=g)),
            f'{p}.mkdir': (self.cmd_mkdir, CommandInfo(
                f'{p}.mkdir', f'{p}.mkdir <path>',
                'Create a directory',
                requires_init=True, controller=ct, group=g)),
            f'{p}.info': (self.cmd_info, CommandInfo(
                f'{p}.info', f'{p}.info <path>',
                'Show file or directory info',
                requires_init=True, controller=ct, group=g)),
            f'{p}.cat': (self.cmd_cat, CommandInfo(
                f'{p}.cat', f'{p}.cat <path>',
                'Display file contents',
                requires_init=True, controller=ct, group=g)),
            f'{p}.download': (self.cmd_download, CommandInfo(
                f'{p}.download', f'{p}.download <remote_path> <local_path>',
                'Download file or directory',
                requires_init=True, controller=ct, group=g)),
            f'{p}.upload': (self.cmd_upload, CommandInfo(
                f'{p}.upload', f'{p}.upload <local_path> <remote_path> [--stream]',
                'Upload file or directory (--stream for raw binary streaming)',
                requires_init=True, controller=ct, group=g)),
            f'{p}.cancel': (self.cmd_cancel, CommandInfo(
                f'{p}.cancel', f'{p}.cancel',
                'Cancel active upload',
                requires_init=True, controller=ct, group=g)),
        }

    # --------------------------------------------------------------------- #
    #  Thin command wrappers (parse args → delegate to _do_*)
    # --------------------------------------------------------------------- #

    def cmd_list(self, args: List[str]):
        if not self._require_init():
            return
        self._do_list(args[0] if args else "/")

    def cmd_tree(self, args: List[str]):
        if not self._require_init():
            return
        path = args[0] if args else "/"
        self.print_info(f"Tree {self._dp}{path} ...")
        self._do_tree(path)

    def cmd_delete(self, args: List[str]):
        if not self._require_init():
            return
        if not args:
            self.print_error(f"Usage: {self._prefix}.rm <path>")
            return
        self._do_delete(args[0])

    def cmd_mkdir(self, args: List[str]):
        if not self._require_init():
            return
        if not args:
            self.print_error(f"Usage: {self._prefix}.mkdir <path>")
            return
        self._do_mkdir(args[0])

    def cmd_info(self, args: List[str]):
        if not self._require_init():
            return
        if not args:
            self.print_error(f"Usage: {self._prefix}.info <path>")
            return
        self._do_info(args[0])

    def cmd_cat(self, args: List[str]):
        if not self._require_init():
            return
        if not args:
            self.print_error(f"Usage: {self._prefix}.cat <path>")
            return
        self._do_cat(args[0])

    def cmd_download(self, args: List[str]):
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error(f"Usage: {self._prefix}.download <remote_path> <local_path>")
            return
        self._do_download_dispatch(args[0], args[1])

    def cmd_upload(self, args: List[str]):
        if not self._require_init():
            return
        stream = '--stream' in args
        args = [a for a in args if a != '--stream']
        if len(args) < 2:
            self.print_error(f"Usage: {self._prefix}.upload <local_path> <remote_path> [--stream]")
            return
        self._do_upload_dispatch(args[0], args[1], stream=stream)

    def cmd_cancel(self, args: List[str]):
        if not self._require_init():
            return
        self._do_upload_cancel()

    # --------------------------------------------------------------------- #
    #  Core file operations
    # --------------------------------------------------------------------- #

    def _do_list(self, path: str):
        """Stream FILE_LIST and render formatted listing."""
        display = f"{self._dp}{path}"
        self.print_info(f"Listing {display} ...")
        packet = HubFxCommands.file_list(path, target=self._target)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return
        result = self._receive_stream(tag, timeout=10.0)
        if result is None:
            return
        data, _ = result
        text = data.decode('utf-8', errors='replace')
        self._format_listing(text, display)

    def _do_delete(self, path: str):
        """Send FILE_DELETE and report result."""
        packet = HubFxCommands.file_delete(path, target=self._target)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, f"Deleted: {self._dp}{path}")

    def _do_mkdir(self, path: str):
        """Send FILE_MKDIR and report result."""
        packet = HubFxCommands.file_mkdir(path, target=self._target)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, f"Created: {self._dp}{path}")

    def _do_info(self, path: str):
        """Send FILE_INFO and display result."""
        packet = HubFxCommands.file_info(path, target=self._target)
        response = self.conn.send_and_wait(packet)
        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return
        if response.packet_type == HubFxPacket.FILE_INFO_RESP and len(response.payload) >= 6:
            exists = response.payload[0]
            is_dir = response.payload[1]
            size = read_u32_le(response.payload, 2)
            print(f"\n  {Fore.CYAN}{self._dp}{path}{Style.RESET_ALL}")
            if exists:
                kind = "directory" if is_dir else "file"
                print(f"    Type: {kind}")
                if not is_dir:
                    print(f"    Size: {size:,} bytes")
            else:
                print(f"    {Fore.RED}Not found{Style.RESET_ALL}")
            print()
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _do_cat(self, path: str):
        """Stream FILE_DOWNLOAD and print contents as text."""
        self.print_info(f"Reading {self._dp}{path} ...")
        packet = HubFxCommands.file_download(path, target=self._target)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return
        result = self._receive_stream(tag, timeout=30.0)
        if result is None:
            return
        data, end_info = result
        text = data.decode('utf-8', errors='replace')
        print()
        print(text)
        total = end_info.get('total_bytes', len(data))
        print(f"\n    ({total} bytes)")

    def _do_tree(self, path: str):
        """Send FILE_TREE request and render the result."""
        packet = HubFxCommands.file_tree(path, target=self._target)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return
        result = self._receive_stream(tag, timeout=30.0)
        if result is None:
            return
        data, _ = result
        text = data.decode('utf-8', errors='replace')
        root_label = f"{self._dp}{path}"
        self._render_tree(text, root_label)

    # --------------------------------------------------------------------- #
    #  Upload cancel
    # --------------------------------------------------------------------- #

    def _do_upload_cancel(self):
        """Send FILE_UPLOAD_CANCEL and drain serial buffers."""
        cancel_pkt = HubFxCommands.file_upload_cancel()
        success, response = self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
        if success:
            self.print_ok(f"Upload cancelled on {self._target_name}")
        elif response and response.error_code == HubFxError.NO_UPLOAD_ACTIVE:
            self.print_info(f"No upload active on {self._target_name}")
        else:
            code = response.error_code if response else 0
            self.print_error(f"Cancel failed: {HubFxError.name(code)}")
        self.conn.drain()

    # --------------------------------------------------------------------- #
    #  Dispatch helpers (single vs recursive)
    # --------------------------------------------------------------------- #

    def _do_download_dispatch(self, remote_path: str, local_path: str):
        """Route to recursive or single download based on remote type."""
        info = self._get_remote_file_info(remote_path)
        if info and info[0] and info[1]:
            self._do_download_recursive(remote_path, local_path)
        else:
            self._do_download(remote_path, local_path)

    def _do_upload_dispatch(self, local_path: str, remote_path: str,
                            stream: bool = False):
        """Route to recursive or single upload based on local type."""
        if os.path.isdir(local_path):
            self._do_upload_recursive(local_path, remote_path, stream=stream)
        else:
            self._do_upload(local_path, remote_path, stream=stream)

    # --------------------------------------------------------------------- #
    #  Single-file upload (sync + stream)
    # --------------------------------------------------------------------- #

    def _do_upload(self, local_path: str, remote_path: str,
                   stream: bool = False) -> bool:
        """
        Upload one file.

        Modes:
        - **sync** (default): ACK per chunk, CRC retry.
        - **stream** (--stream): Raw binary streaming with segment-based ACKs.
          Bypasses COBS framing for the data plane.  Server reads raw bytes
          from UART and sends a COBS-framed FILE_UPLOAD_PROGRESS after each
          512 KB segment.  Maximum throughput.

        Returns True on success.
        """

        # Read local file
        try:
            with open(local_path, 'rb') as f:
                file_data = f.read()
        except IOError as e:
            self.print_error(f"Cannot read local file: {e}")
            return False

        file_size = len(file_data)
        mode_str = "stream" if stream else "sync"
        self.print_info(
            f"Uploading {local_path} ({file_size} bytes) → "
            f"{self._target_name}:{remote_path} [{mode_str}]")

        # Begin upload
        upload_mode = HubFxStorage.UPLOAD_STREAM if stream else HubFxStorage.UPLOAD_SYNC

        packet = HubFxCommands.file_upload_begin(
            remote_path, file_size,
            target=self._target, mode=upload_mode)
        success, response = self.conn.send_expect_ack(packet, timeout=10.0)
        if not success:
            if response:
                code = response.error_code
                self.print_error(f"Upload begin failed: {HubFxError.name(code)}")
            else:
                self.print_error("Upload begin failed (timeout)")
            return False

        start_time = time.time()

        # Parse stream params from ACK payload (stream mode)
        segment_size = 524288  # default 512 KB
        segment_count = 0
        if stream and response and response.payload and len(response.payload) >= 6:
            segment_size = int.from_bytes(response.payload[:4], 'little')
            segment_count = int.from_bytes(response.payload[4:6], 'little')
            self.print_info(
                f"Stream: {segment_count} segments × "
                f"{segment_size // 1024} KB")

        try:
            if stream:
                # ---- Stream mode: raw binary, segment-based ACKs ----
                ok = self._do_upload_stream(
                    file_data, file_size, segment_size,
                    segment_count, start_time)
                if not ok:
                    return False
                local_md5 = hashlib.md5(file_data)

            else:
                # ---- Sync mode: COBS-framed, ACK per chunk with retry ----
                local_md5 = hashlib.md5()
                chunk_size = 2044  # MAX_PAYLOAD_SIZE(2048) - 4 (seq+crc16 header)
                offset = 0
                seq = 0
                max_retries = 3

                while offset < file_size:
                    if self.cancel_requested:
                        print()
                        self.print_warning("Upload interrupted — cancelling...")
                        cancel_pkt = HubFxCommands.file_upload_cancel()
                        self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                        self.conn.drain()
                        return False

                    chunk = file_data[offset:offset + chunk_size]
                    packet = HubFxCommands.file_upload_data(seq, chunk)
                    local_md5.update(chunk)

                    sent = False
                    for retry in range(max_retries):
                        success, response = self.conn.send_expect_ack(
                            packet, timeout=10.0)
                        if success:
                            sent = True
                            break
                        if response and response.error_code == 0x02:  # CRC_ERROR
                            self.print_warning(
                                f"CRC error on segment {seq}, "
                                f"retrying ({retry + 1}/{max_retries})")
                            continue
                        code = response.error_code if response else 0
                        self.print_error(
                            f"Upload failed at segment {seq}: "
                            f"{HubFxError.name(code)}")
                        cancel_pkt = HubFxCommands.file_upload_cancel()
                        self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                        self.conn.drain()
                        return False

                    if not sent:
                        self.print_error(
                            f"Upload failed: max retries on segment {seq}")
                        cancel_pkt = HubFxCommands.file_upload_cancel()
                        self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                        self.conn.drain()
                        return False

                    offset += len(chunk)
                    seq += 1
                    bar = format_progress_bar(offset, file_size,
                                              start_time=start_time)
                    print(f"\r{bar}  ", end='', flush=True)

        except KeyboardInterrupt:
            print()
            self.print_warning("Upload interrupted — cancelling...")
            cancel_pkt = HubFxCommands.file_upload_cancel()
            self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
            self.conn.drain()
            return False

        print()  # Newline after progress bar

        # End upload — response includes MD5 hash.
        # Proportional timeout: accounts for remaining OS TX buffer drainage
        # plus firmware processing (writer drain, file close, MD5 compute).
        wire_estimate = file_size / (300 * 1024)
        end_timeout = max(15.0, wire_estimate + 10.0)
        packet = HubFxCommands.file_upload_end()
        t_end_send = time.time()
        self.print_info(
            f"[dbg] sending UPLOAD_END "
            f"t+{t_end_send - start_time:.1f}s "
            f"timeout={end_timeout:.1f}s")
        success, response = self.conn.send_expect_ack(packet, timeout=end_timeout)
        t_end_recv = time.time()

        # --- Diagnostic dump on any failure ---
        if not success:
            self.print_warning(
                f"[dbg] UPLOAD_END failed: "
                f"response={'NACK 0x' + format(response.error_code, '02X') if response and response.is_nack else 'timeout' if response is None else 'type=0x' + format(response.packet_type, '02X')} "
                f"waited={t_end_recv - t_end_send:.1f}s")

        elapsed = time.time() - start_time
        if success:
            speed = file_size / elapsed if elapsed > 0 else 0
            self.print_ok(
                f"Uploaded {file_size} bytes → "
                f"{self._target_name}:{remote_path} "
                f"in {elapsed:.1f}s ({speed / 1024:.1f} KB/s)")

            # MD5 verification
            if response and len(response.payload) >= 16:
                remote_md5 = response.payload[:16].hex()
                local_md5_hex = local_md5.hexdigest()
                if remote_md5 == local_md5_hex:
                    self.print_ok(f"MD5 verified: {remote_md5}")
                else:
                    self.print_error(
                        f"MD5 MISMATCH! local={local_md5_hex} "
                        f"remote={remote_md5}")
            return True
        else:
            if response:
                if response.is_nack:
                    code = response.error_code
                    self.print_error(
                        f"Upload end failed: {HubFxError.name(code)}")
                else:
                    self.print_error(
                        f"Upload end failed: unexpected response "
                        f"type=0x{response.packet_type:02X} "
                        f"tag={response.tag}")
            else:
                self.print_error("Upload end failed (timeout)")
            # Ensure server clears upload state
            cancel_pkt = HubFxCommands.file_upload_cancel()
            self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
            self.conn.drain()
            return False

    # --------------------------------------------------------------------- #
    #  Stream upload (raw binary, segment-based ACKs)
    # --------------------------------------------------------------------- #

    def _do_upload_stream(self, file_data: bytes, file_size: int,
                          segment_size: int, segment_count: int,
                          start_time: float) -> bool:
        """
        Send raw binary data in segments with server flow control.

        After UPLOAD_BEGIN ACK, sends raw bytes (no COBS framing) directly
        to the serial port.  The server reads Serial.readBytes() in its
        processStream() loop and sends a COBS-framed FILE_UPLOAD_PROGRESS
        after each segment boundary.

        Returns True on success (caller sends UPLOAD_END).
        Returns False on failure (caller should cancel).
        """
        SEGMENT_TIMEOUT = 15.0   # seconds to wait for segment ACK
        WRITE_CHUNK = 32768      # 32 KB per serial write call

        offset = 0
        seg_idx = 0

        while offset < file_size:
            if self.cancel_requested:
                print()
                self.print_warning("Upload interrupted — cancelling...")
                cancel_pkt = HubFxCommands.file_upload_cancel()
                self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                self.conn.drain()
                return False

            # Determine this segment's size
            remaining = file_size - offset
            this_seg = min(segment_size, remaining)

            # Send raw binary in write-sized chunks
            seg_sent = 0
            while seg_sent < this_seg:
                chunk_len = min(WRITE_CHUNK, this_seg - seg_sent)
                chunk = file_data[offset:offset + chunk_len]
                try:
                    self.conn._serial.write(chunk)
                except Exception as e:
                    print()
                    self.print_error(f"Serial write failed: {e}")
                    cancel_pkt = HubFxCommands.file_upload_cancel()
                    self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                    self.conn.drain()
                    return False

                offset += chunk_len
                seg_sent += chunk_len

                # Update progress bar
                bar = format_progress_bar(offset, file_size,
                                          start_time=start_time)
                print(f"\r{bar}  ", end='', flush=True)

            # Flush serial TX buffer before waiting for ACK
            try:
                self.conn._serial.flush()
            except Exception:
                pass

            # Wait for segment ACK from server
            progress = self._wait_for_upload_progress(SEGMENT_TIMEOUT)
            if progress is None:
                print()
                self.print_error(
                    f"Timeout waiting for segment ACK "
                    f"(segment {seg_idx})")
                cancel_pkt = HubFxCommands.file_upload_cancel()
                self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                self.conn.drain()
                return False

            # Parse segment ACK payload:
            # [segment_idx:u16LE][bytes_received:u32LE][ring_fill_pct:u8]
            p = progress.payload
            ring_fill = 0
            if len(p) >= 7:
                ack_seg_idx = int.from_bytes(p[0:2], 'little')
                bytes_received = int.from_bytes(p[2:6], 'little')
                ring_fill = p[6]

                # Display diagnostics
                bar = format_progress_bar(offset, file_size,
                                          start_time=start_time)
                diag = (f"  [seg={ack_seg_idx}/{segment_count} "
                        f"rx={bytes_received:,}B "
                        f"ring={ring_fill}%]")
                print(f"\r{bar}{diag}  ", end='', flush=True)

            # Flow control: throttle based on ring buffer fill level
            if ring_fill > 50:
                import time as _time
                delay_s = (ring_fill - 50) * 0.06  # ~60ms per pct above 50%
                _time.sleep(delay_s)

            seg_idx += 1

        return True

    def _wait_for_upload_progress(self, timeout: float):
        """
        Wait for a FILE_UPLOAD_PROGRESS packet from the server.

        Reads packets until we get the right type or timeout.
        Dispatches async/log packets to callbacks, stashes
        other tag-correlated responses.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            response = self.conn.receive(timeout=remaining)
            if response is None:
                break
            if response.packet_type == HubFxPacket.FILE_UPLOAD_PROGRESS:
                return response
            # NACK during stream = server error
            if response.is_nack:
                code = response.error_code
                self.print_error(
                    f"Server error during stream: {HubFxError.name(code)}")
                return None
            # Dispatch async packets (log messages, etc.)
            if response.tag == 0:
                for cb in self.conn._callbacks:
                    try:
                        cb(response)
                    except Exception:
                        pass
            else:
                # Stash for later
                self.conn._pending[response.tag] = response
        return None

    # --------------------------------------------------------------------- #
    #  Single-file download
    # --------------------------------------------------------------------- #

    def _do_download(self, remote_path: str, local_path: str,
                     quiet: bool = False) -> bool:
        """Download one file.  Returns True on success."""
        if not quiet:
            self.print_info(f"Downloading {self._target_name}:{remote_path} ...")

        packet = HubFxCommands.file_download(remote_path, target=self._target)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return False

        result = self._receive_stream(tag, timeout=60.0,
                                      show_progress=(not quiet))
        if result is None:
            return False

        data, end_info = result
        try:
            os.makedirs(os.path.dirname(os.path.abspath(local_path)),
                        exist_ok=True)
            with open(local_path, 'wb') as f:
                f.write(data)
            total = end_info.get('total_bytes', len(data))
            if not quiet:
                self.print_ok(f"Downloaded {total} bytes → {local_path}")
            return True
        except IOError as e:
            self.print_error(f"Failed to write local file: {e}")
            return False

    # --------------------------------------------------------------------- #
    #  Recursive upload / download
    # --------------------------------------------------------------------- #

    def _do_upload_recursive(self, local_dir: str, remote_dir: str,
                             stream: bool = False):
        """Recursively upload a local directory."""
        if not os.path.isdir(local_dir):
            self.print_error(f"Not a directory: {local_dir}")
            return

        all_dirs: List[str] = []
        all_files: List[Tuple[str, str, int]] = []
        for root, dirs, files in os.walk(local_dir):
            rel_root = os.path.relpath(root, local_dir).replace('\\', '/')
            if rel_root == '.':
                remote_base = remote_dir
            else:
                remote_base = f"{remote_dir.rstrip('/')}/{rel_root}"
            all_dirs.append(remote_base)
            for f in files:
                local_file = os.path.join(root, f)
                remote_file = f"{remote_base.rstrip('/')}/{f}"
                file_size = os.path.getsize(local_file)
                all_files.append((local_file, remote_file, file_size))

        total_files = len(all_files)
        total_bytes = sum(s for _, _, s in all_files)
        self.print_info(
            f"Uploading directory {local_dir} → "
            f"{self._target_name}:{remote_dir} "
            f"({total_files} files, {total_bytes:,} bytes)")

        for d in all_dirs:
            packet = HubFxCommands.file_mkdir(d, target=self._target)
            self.conn.send_expect_ack(packet, timeout=5.0)

        start_time = time.time()
        uploaded = 0
        uploaded_bytes = 0
        failed = 0

        for i, (local_file, remote_file, file_size) in enumerate(all_files, 1):
            if self.cancel_requested:
                self.print_warning(
                    f"Interrupted after {uploaded}/{total_files} files — "
                    f"remaining files skipped")
                break

            print(f"  [{i}/{total_files}] {remote_file} ({file_size:,} bytes)")
            ok = self._do_upload(local_file, remote_file,
                                  stream=stream)
            if ok:
                uploaded += 1
                uploaded_bytes += file_size
                bar = format_progress_bar(uploaded_bytes, total_bytes,
                                          start_time=start_time)
                print(f"  Overall: {bar}")
            else:
                failed += 1
                if self.cancel_requested:
                    self.print_warning(
                        f"Interrupted after {uploaded}/{total_files} files — "
                        f"remaining files skipped")
                    break

        elapsed = time.time() - start_time
        speed = uploaded_bytes / elapsed if elapsed > 0 else 0
        status = "Uploaded" if uploaded == total_files else "Partial upload"
        self.print_ok(
            f"{status}: {uploaded}/{total_files} files "
            f"({uploaded_bytes:,} bytes) "
            f"in {elapsed:.1f}s ({speed / 1024:.1f} KB/s)"
            + (f", {failed} failed" if failed else ""))

    def _do_download_recursive(self, remote_dir: str, local_dir: str):
        """Recursively download a remote directory."""
        self.print_info(f"Scanning {self._target_name}:{remote_dir} ...")
        entries = self._get_remote_tree_entries(remote_dir)
        if entries is None:
            self.print_error("Failed to list remote directory")
            return
        if not entries:
            self.print_info("Remote directory is empty")
            return

        path_stack: List[str] = []
        files_to_download: List[Tuple[str, str, int]] = []
        dirs_to_create: List[str] = []

        for depth, is_dir, name, size in entries:
            while len(path_stack) > depth:
                path_stack.pop()
            path_stack.append(name)
            rel_path = '/'.join(path_stack)
            if is_dir:
                local_path = os.path.join(
                    local_dir, rel_path.replace('/', os.sep))
                dirs_to_create.append(local_path)
            else:
                remote_path = f"{remote_dir.rstrip('/')}/{rel_path}"
                local_path = os.path.join(
                    local_dir, rel_path.replace('/', os.sep))
                files_to_download.append((remote_path, local_path, size))

        total_files = len(files_to_download)
        total_bytes = sum(s for _, _, s in files_to_download)
        self.print_info(
            f"Downloading {total_files} files ({total_bytes:,} bytes) "
            f"→ {local_dir}")

        os.makedirs(local_dir, exist_ok=True)
        for d in dirs_to_create:
            os.makedirs(d, exist_ok=True)

        start_time = time.time()
        downloaded = 0
        failed = 0

        for i, (remote_path, local_path, size) in enumerate(
                files_to_download, 1):
            print(f"  [{i}/{total_files}] {remote_path} ({size:,} bytes)")
            if self._do_download(remote_path, local_path, quiet=True):
                downloaded += 1
            else:
                failed += 1

        elapsed = time.time() - start_time
        speed = total_bytes / elapsed if elapsed > 0 else 0
        if failed:
            self.print_warning(
                f"Downloaded {downloaded}/{total_files} files "
                f"({failed} failed) in {elapsed:.1f}s")
        else:
            self.print_ok(
                f"Downloaded {downloaded} files ({total_bytes:,} bytes) "
                f"in {elapsed:.1f}s ({speed / 1024:.1f} KB/s)")

    # --------------------------------------------------------------------- #
    #  Remote query helpers
    # --------------------------------------------------------------------- #

    def _get_remote_file_info(self, path: str
                              ) -> Optional[Tuple[bool, bool, int]]:
        """Query FILE_INFO. Returns (exists, is_dir, size) or None."""
        packet = HubFxCommands.file_info(path, target=self._target)
        response = self.conn.send_and_wait(packet)
        if response is None or response.is_nack:
            return None
        if (response.packet_type == HubFxPacket.FILE_INFO_RESP
                and len(response.payload) >= 6):
            exists = bool(response.payload[0])
            is_dir = bool(response.payload[1])
            size = read_u32_le(response.payload, 2)
            return (exists, is_dir, size)
        return None

    def _get_remote_tree_entries(self, path: str
                                 ) -> Optional[List[Tuple[int, bool, str, int]]]:
        """Fetch FILE_TREE → list of (depth, is_dir, name, size)."""
        packet = HubFxCommands.file_tree(path, target=self._target)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return None
        result = self._receive_stream(tag, timeout=30.0)
        if result is None:
            return None
        data, _ = result
        text = data.decode('utf-8', errors='replace')
        entries = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith('ERROR'):
                continue
            parts = line.split(' ', 3)
            if len(parts) < 4:
                continue
            try:
                depth = int(parts[0])
                is_dir = parts[1] == 'd'
                name = parts[2]
                size = int(parts[3])
                entries.append((depth, is_dir, name, size))
            except (ValueError, IndexError):
                continue
        return entries

    # --------------------------------------------------------------------- #
    #  Stream reception
    # --------------------------------------------------------------------- #

    def _receive_stream(self, tag: int, timeout: float = 10.0,
                        show_progress: bool = False
                        ) -> Optional[Tuple[bytes, dict]]:
        """
        Receive a complete stream (BEGIN + DATA chunks + END).

        Returns (data_bytes, end_info) or None on error.
        """
        data = bytearray()
        total_expected = 0
        crc_errors = 0
        start_time = time.time() if show_progress else 0

        while True:
            response = self.conn._wait_for_tag(tag, timeout=timeout)
            if response is None:
                # Diagnostic: dump serial state to understand why no response
                rx_waiting = 0
                rx_buf_len = len(self.conn._rx_buffer) if hasattr(self.conn, '_rx_buffer') else -1
                pending_tags = list(self.conn._pending.keys()) if hasattr(self.conn, '_pending') else []
                try:
                    rx_waiting = self.conn._serial.in_waiting
                except Exception:
                    pass
                self.print_error(
                    f"Stream timeout (waited {timeout:.0f}s, "
                    f"rx_waiting={rx_waiting}, "
                    f"rx_buf={rx_buf_len}, "
                    f"pending_tags={pending_tags})")
                return None
            if response.is_nack:
                code = response.error_code
                self.print_error(
                    f"NACK: {HubFxError.name(code)} (0x{code:02X})")
                return None

            if response.packet_type == StreamPacket.STREAM_BEGIN:
                if len(response.payload) >= 4:
                    total_expected = read_u32_le(response.payload, 0)

            elif response.packet_type == StreamPacket.STREAM_DATA:
                if len(response.payload) < 4:
                    continue
                seq = read_u16_le(response.payload, 0)
                crc = read_u16_le(response.payload, 2)
                chunk = response.payload[4:]
                computed = crc16_ccitt(chunk)
                if computed != crc:
                    crc_errors += 1
                    self.print_warning(f"CRC mismatch on segment {seq}")
                data.extend(chunk)

                if show_progress and total_expected > 0:
                    bar = format_progress_bar(len(data), total_expected,
                                              start_time=start_time)
                    print(f"\r{bar}  ", end='', flush=True)

            elif response.packet_type == StreamPacket.STREAM_END:
                if show_progress and total_expected > 0:
                    print()

                end_info: dict = {}
                if len(response.payload) >= 8:
                    end_info['total_segs'] = read_u16_le(
                        response.payload, 0)
                    end_info['total_bytes'] = read_u32_le(
                        response.payload, 2)
                    end_info['crc_all'] = read_u16_le(
                        response.payload, 6)
                end_info['crc_errors'] = crc_errors

                if 'crc_all' in end_info:
                    computed_all = crc16_ccitt(data)
                    if computed_all != end_info['crc_all']:
                        self.print_warning(
                            "Overall CRC mismatch on stream")

                return (bytes(data), end_info)

        return None

    # --------------------------------------------------------------------- #
    #  Formatting helpers
    # --------------------------------------------------------------------- #

    @staticmethod
    def _format_size(size: int) -> str:
        """Format file size in human-readable units."""
        if size < 1024:
            return f"{size} B"
        elif size < 1024 * 1024:
            return f"{size / 1024:.1f} KB"
        elif size < 1024 * 1024 * 1024:
            return f"{size / (1024 * 1024):.1f} MB"
        else:
            return f"{size / (1024 * 1024 * 1024):.1f} GB"

    def _format_listing(self, text: str, root_path: str):
        """Format and print a directory listing."""
        lines = []
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith('ERROR'):
                self.print_error(line)
                continue
            parts = line.split('\t', 2)
            if len(parts) < 3:
                parts = line.split(' ', 2)
            if len(parts) < 3:
                lines.append(('?', line, 0))
                continue
            type_char = parts[0]
            name = parts[1]
            try:
                size = int(parts[2])
            except ValueError:
                size = 0
            lines.append((type_char, name, size))

        if not lines:
            print(f"\n  {Fore.CYAN}{root_path}{Style.RESET_ALL}")
            print(f"  (empty)")
            print()
            return

        max_name = max(len(name) for _, name, _ in lines)
        print(f"\n  {Fore.CYAN}{root_path}{Style.RESET_ALL}")
        for type_char, name, size in lines:
            if type_char == 'd':
                display = (f"    {Fore.BLUE}"
                           f"{name + '/':<{max_name + 1}}"
                           f"{Style.RESET_ALL}")
            else:
                display = f"    {name:<{max_name + 1}}"
            size_str = self._format_size(size) if type_char == 'f' else ''
            print(f"{display}  {size_str}")
        print()

    def _render_tree(self, text: str, root_path: str, prefix: str = ""):
        """Render POSIX-style tree output with box-drawing characters."""
        entries = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith('ERROR'):
                if line.startswith('ERROR'):
                    self.print_error(line)
                continue
            parts = line.split('\t', 3)
            if len(parts) < 4:
                parts = line.split(' ', 3)
            if len(parts) < 4:
                continue
            try:
                depth = int(parts[0])
                is_dir = parts[1] == 'd'
                name = parts[2]
                size = int(parts[3])
                entries.append((depth, is_dir, name, size))
            except (ValueError, IndexError):
                continue

        if not entries:
            print(f"\n  {prefix}{Fore.CYAN}{root_path}{Style.RESET_ALL}")
            print(f"  (empty)")
            print()
            return

        # Determine last-sibling status for each entry
        is_last_at_depth = []
        for i, (depth, *_) in enumerate(entries):
            last = True
            for j in range(i + 1, len(entries)):
                jd = entries[j][0]
                if jd == depth:
                    last = False
                    break
                if jd < depth:
                    break
            is_last_at_depth.append(last)

        print(f"\n  {prefix}{Fore.CYAN}{root_path}{Style.RESET_ALL}")
        depth_continues: dict = {}
        dir_count = file_count = 0

        for i, (depth, is_dir, name, size) in enumerate(entries):
            is_last = is_last_at_depth[i]

            line_prefix = "  "
            for d in range(depth):
                if depth_continues.get(d, False):
                    line_prefix += "│   "
                else:
                    line_prefix += "    "

            connector = "└── " if is_last else "├── "

            if is_dir:
                dir_count += 1
                display = f"{Fore.BLUE}{name}/{Style.RESET_ALL}"
            else:
                file_count += 1
                display = f"{name} ({size:,})"

            print(f"{line_prefix}{connector}{display}")

            depth_continues[depth] = not is_last
            for d in list(depth_continues.keys()):
                if d > depth:
                    del depth_continues[d]

        summary_parts = []
        if dir_count:
            suffix = 'ies' if dir_count != 1 else 'y'
            summary_parts.append(f"{dir_count} director{suffix}")
        if file_count:
            suffix = 's' if file_count != 1 else ''
            summary_parts.append(f"{file_count} file{suffix}")
        print(f"\n  {', '.join(summary_parts)}")
        print()

    # --------------------------------------------------------------------- #
    #  Utility
    # --------------------------------------------------------------------- #

    def _print_ack_nack(self, success: bool, response, ok_msg: str):
        """Print ACK/NACK result."""
        if success:
            self.print_ok(ok_msg)
        elif response is not None:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(
                f"NACK: {name} (0x{code:02X})"
                + (f" - {msg}" if msg else ""))
        else:
            self.print_error("No response (timeout)")
