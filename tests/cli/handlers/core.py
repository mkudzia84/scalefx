"""
Core Command Handlers

Core CLI commands that are always available:
- Connection management (connect, disconnect, ports)
- Protocol commands (init, shutdown, reboot, bootsel, status, keepalive)
- Utility commands (help, raw)
"""

from typing import List, Dict, Tuple, Callable, Optional

from tests.framework import (
    ScaleFXConnection, CommandBuilder,
    CorePacket, CoreError, GunFxError, LightFxError,
    GearControlPacket,
    find_ports, find_scalefx_ports
)
from ..base import CommandHandlerBase, CommandInfo, ControllerType, Fore, Style
from .. import parsers


class CoreCommandHandler(CommandHandlerBase):
    """
    Handler for core CLI commands.
    
    These commands are always available regardless of connection state
    or controller type.
    """
    
    def __init__(self):
        super().__init__()
        self._on_controller_detected: Optional[Callable[[str, str, str], None]] = None
        self._on_quit: Optional[Callable[[], None]] = None
    
    def on_controller_detected(self, callback: Callable[[str, str, str], None]):
        """Set callback for when controller type is detected: (type, name, version)."""
        self._on_controller_detected = callback
    
    def on_quit(self, callback: Callable[[], None]):
        """Set callback for quit command."""
        self._on_quit = callback
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return core command registry."""
        return {
            'help': (self.cmd_help, CommandInfo('help', 'help [command]', 'Show available commands')),
            '?': (self.cmd_help, CommandInfo('?', '?', 'Show available commands')),
            'quit': (self.cmd_quit, CommandInfo('quit', 'quit', 'Exit the CLI')),
            'exit': (self.cmd_quit, CommandInfo('exit', 'exit', 'Exit the CLI')),
            'ports': (self.cmd_ports, CommandInfo('ports', 'ports', 'List available serial ports')),
            'connect': (self.cmd_connect, CommandInfo('connect', 'connect [port]', 'Connect to controller')),
            'disconnect': (self.cmd_disconnect, CommandInfo('disconnect', 'disconnect', 'Disconnect from controller')),
            'reconnect': (self.cmd_reconnect, CommandInfo('reconnect', 'reconnect', 'Reconnect serial port (recover from hang/corruption)')),
            'raw': (self.cmd_raw, CommandInfo('raw', 'raw <hex>', 'Send raw hex bytes')),
        }
    
    def get_protocol_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return protocol command registry (requires connection)."""
        return {
            'init': (self.cmd_init, CommandInfo('init', 'init', 'Initialize connection (sends INIT packet)')),
            'identify': (self.cmd_identify, CommandInfo('identify', 'identify', 'Query board info without triggering INIT')),
            'shutdown': (self.cmd_shutdown, CommandInfo('shutdown', 'shutdown', 'Safe shutdown', requires_init=True)),
            'reboot': (self.cmd_reboot, CommandInfo('reboot', 'reboot', 'Reboot device', requires_init=True)),
            'bootsel': (self.cmd_bootsel, CommandInfo('bootsel', 'bootsel', 'Enter USB bootloader (Pico only)', requires_init=True)),
            'status': (self.cmd_status, CommandInfo('status', 'status', 'Request device status', requires_init=True)),
            'keepalive': (self.cmd_keepalive, CommandInfo('keepalive', 'keepalive', 'Send keepalive ping', requires_init=True)),
            'i2c.scan': (self.cmd_i2c_scan, CommandInfo('i2c.scan', 'i2c.scan', 'Scan I2C bus for expected and extra devices', requires_init=True)),
            'diag': (self.cmd_diag, CommandInfo('diag', 'diag', 'Retrieve diagnostic log history (non-draining)', requires_init=True)),
        }
    
    # =========================================================================
    # Help Command
    # =========================================================================
    
    def cmd_help(self, args: List[str]):
        """Show available commands (placeholder - CLI overrides this)."""
        print("Help: Use parent CLI's help command.")
    
    # =========================================================================
    # Connection Commands
    # =========================================================================
    
    def cmd_quit(self, args: List[str]):
        """Exit CLI."""
        if self.conn and self.conn.is_connected:
            self.print_info("Disconnecting...")
            self.conn.close()
        if self._on_quit:
            self._on_quit()
        print("Goodbye!")
    
    def cmd_ports(self, args: List[str]):
        """List serial ports."""
        print(f"\n{Fore.YELLOW}Available Ports:{Style.RESET_ALL}")
        
        scalefx = find_scalefx_ports()
        if scalefx:
            print(f"\n  {Fore.GREEN}Detected ScaleFX devices:{Style.RESET_ALL}")
            for port, desc in scalefx:
                print(f"    {port} - {desc}")
        
        all_ports = find_ports()
        if all_ports:
            print(f"\n  All ports:")
            for port in all_ports:
                print(f"    {port}")
        else:
            print("  No serial ports found")
        print()
    
    def cmd_connect(self, args: List[str]):
        """Connect to a controller."""
        if self.conn and self.conn.is_connected:
            self.print_info("Already connected. Use 'disconnect' first.")
            return
        
        port = args[0] if args else None
        if not port:
            scalefx = find_scalefx_ports()
            if scalefx:
                port = scalefx[0][0]
                self.print_info(f"Auto-detected: {port}")
            else:
                self.print_error("No port specified and none detected. Use 'ports' to list.")
                return
        
        self.conn = ScaleFXConnection(port=port)
        self.print_info(f"Connecting to {port}...")
        
        if self.conn.connect(init=False):
            self.print_ok(f"Connected to {port}")
            # Auto-discover controller type via IDENTIFY, then INIT if needed
            self._identify_and_init()
        else:
            self.print_error(f"Failed to connect to {port}")
            self.conn = None
    
    def cmd_disconnect(self, args: List[str]):
        """Disconnect from controller."""
        if not self.conn:
            self.print_info("Not connected")
            return
        
        self.conn.close()
        self.conn = None
        self.controller_type = None
        self.print_ok("Disconnected")
    
    def cmd_reconnect(self, args: List[str]):
        """Reconnect serial port — handled by InteractiveCLI special case."""
        # This stub exists for the command registry. The actual implementation
        # is in InteractiveCLI._cmd_reconnect() which is special-cased in
        # _process_command (needs direct access to listener lifecycle).
        self.print_info("Reconnect is handled by the CLI directly.")
    
    def cmd_raw(self, args: List[str]):
        """Send raw hex bytes."""
        if not self._require_connection():
            return
        
        if not args:
            self.print_error("Usage: raw <hex_bytes>")
            return
        
        try:
            hex_str = ''.join(args)
            data = bytes.fromhex(hex_str)
            self.print_info(f"Sending {len(data)} bytes: {data.hex()}")
            self.conn.send(data)
            response = self.conn.receive()
            self._print_response(response)
        except ValueError:
            self.print_error("Invalid hex string")
    
    # =========================================================================
    # Protocol Commands
    # =========================================================================
    
    def cmd_init(self, args: List[str]):
        """Send INIT command."""
        if not self._require_connection():
            return
        
        response = self.conn.send_and_wait(CommandBuilder.init())
        self._print_response(response)
        
        if response and response.is_init_ready:
            self.conn._initialized = True
    
    def cmd_identify(self, args: List[str]):
        """Send IDENTIFY command (query board info without triggering INIT)."""
        if not self._require_connection():
            return
        
        response = self.conn.send_and_wait(CommandBuilder.identify())
        self._print_response(response)
    
    def _identify_and_init(self):
        """Send IDENTIFY to discover board type, then INIT only if needed.
        
        IDENTIFY (0xFE) returns the same board info as INIT_READY but without
        triggering any initialization callbacks or state changes on the device.
        This avoids the expensive re-initialization that INIT causes on boards
        that auto-initialize on boot (HubFX).
        
        Flow:
          1. Send IDENTIFY → get board name, version, platform, build
          2. Detect controller type from name
          3. If HubFX (autonomous hub): mark as initialized, skip INIT
          4. If slave controller: send INIT to start the device
          5. If IDENTIFY fails (legacy firmware): fall back to INIT
        """
        self.print_info("Identifying controller...")
        # Drain any residual boot output (ESP32 bootloader junk) before sending
        self.conn._drain_serial()
        self.conn._rx_buffer.clear()
        response = self.conn.send_and_wait(CommandBuilder.identify(), timeout=3.0)
        
        if response is None or response.is_nack:
            # IDENTIFY not supported or timeout — fall back to INIT
            self.print_warning("IDENTIFY failed, falling back to INIT...")
            self.cmd_init([])
            return
        
        if not response.is_identify:
            self.print_warning(f"Unexpected response 0x{response.packet_type:02X}, falling back to INIT...")
            self.cmd_init([])
            return
        
        # Parse board info (same format as INIT_READY)
        info = parsers.parse_init_ready(response.payload)
        if info is None:
            self.print_error("Failed to parse IDENTIFY response")
            self.cmd_init([])
            return
        
        # Print board info (once — from IDENTIFY response)
        parsers.print_init_ready_info(info)
        
        if info.controller_type == ControllerType.HUBFX:
            # HubFX auto-initializes on boot — INIT would cause expensive
            # re-initialization (codec reset, engine restart, config reload).
            self.conn._initialized = True
            self.controller_type = info.controller_type
            self.print_info("HubFX detected (autonomous hub — no INIT needed)")
        else:
            # Slave controller — needs INIT to activate its hardware.
            # Set controller_type from IDENTIFY before sending INIT so that
            # if INIT fails, we still know what board we're talking to.
            self.controller_type = info.controller_type
            self.print_info(f"Sending INIT to activate {info.name}...")
            init_response = self.conn.send_and_wait(CommandBuilder.init())
            if init_response and init_response.is_init_ready:
                self.conn._initialized = True
                self.print_ok("Controller initialized")
            else:
                self.print_error("INIT failed — board may need power cycle")
                return
        
        # Common: notify detection callback and show available commands
        if info.controller_type and self._on_controller_detected:
            self._on_controller_detected(info.controller_type, info.name, info.version)
        
        ctrl_cmds = {
            ControllerType.GEARCONTROL: "gc.*",
            ControllerType.GUNFX: "gfx.*",
            ControllerType.HUBFX: "hub.*, gfx.*, lfx.*, gc.*",
            ControllerType.LIGHTFX: "lfx.*",
            ControllerType.NOOP: "core",
        }
        cmds = ctrl_cmds.get(info.controller_type, "")
        if cmds:
            self.print_info(f"{cmds} commands now available")
        print(f"  Type 'help' to see available commands")
    
    def cmd_shutdown(self, args: List[str]):
        """Send SHUTDOWN command."""
        if not self._require_init():
            return
        success, response = self.conn.send_expect_ack(CommandBuilder.shutdown())
        self._print_response(response)
    
    def cmd_reboot(self, args: List[str]):
        """Send REBOOT command."""
        if not self._require_init():
            return
        self.print_info("Sending REBOOT (no response expected)...")
        self.conn.send(CommandBuilder.reboot())
        self.conn._initialized = False
        self.controller_type = None
    
    def cmd_bootsel(self, args: List[str]):
        """Send BOOTSEL command (enters UF2 bootloader on Pico, not supported on ESP32)."""
        if not self._require_init():
            return
        self.print_info("Sending BOOTSEL...")
        response = self.conn.send_and_wait(CommandBuilder.bootsel(), timeout=2.0)
        if response and response.is_nack:
            # Device responded with NACK (e.g., ESP32 — no bootloader mode)
            self._print_response(response)
        else:
            # No response = device entered bootloader (Pico), or timeout
            self.print_info("Device entering bootloader mode (USB mass storage)")
            self.conn._initialized = False
            self.controller_type = None
    
    def cmd_status(self, args: List[str]):
        """Request status."""
        if not self._require_init():
            return
        response = self.conn.send_and_wait(CommandBuilder.status_req())
        self._print_response(response)
    
    def cmd_keepalive(self, args: List[str]):
        """Send keepalive."""
        if not self._require_init():
            return
        success, response = self.conn.send_expect_ack(CommandBuilder.keepalive())
        self._print_response(response)
    
    def cmd_i2c_scan(self, args: List[str]):
        """Scan I2C bus for expected devices and report extras."""
        if not self._require_init():
            return
        
        packet = CommandBuilder.i2c_scan()
        response = self.conn.send_and_wait(packet)
        
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
            return
        
        if response.packet_type == CorePacket.I2C_SCAN_RESULT:
            parsers.parse_i2c_scan_result(response.payload)
        else:
            self.print_error(f"Unexpected response type: 0x{response.packet_type:02X}")
    
    def cmd_diag(self, args: List[str]):
        """Retrieve diagnostic log history (non-draining).
        
        Sends DIAG_HISTORY command which returns all buffered LOG_MESSAGE
        packets (up to 128) without draining the ring buffer.
        
        The listener is stopped during command execution, so LOG_MESSAGE
        packets arrive via _wait_for_tag's async dispatch. We register a
        temporary callback to print them as they arrive before the ACK.
        """
        if not self._require_init():
            return
        
        from tests.framework.protocol import build_packet
        import struct
        
        # Count LOG_MESSAGE packets as they arrive (display is handled by the
        # permanent _print_async_message callback on the connection)
        msg_count = 0
        def on_log(response):
            nonlocal msg_count
            if response.packet_type == CorePacket.LOG_MESSAGE:
                msg_count += 1
        
        self.conn.add_callback(on_log)
        try:
            packet = build_packet(CorePacket.DIAG_HISTORY, b'')
            response = self.conn.send_and_wait(packet, timeout=3.0)
            
            if response is None:
                self.print_error("No response (timeout)")
            elif response.is_ack:
                if len(response.payload) >= 2:
                    count = struct.unpack('<H', response.payload[:2])[0]
                    self.print_ok(f"Received {msg_count} of {count} log messages")
                else:
                    self.print_ok(f"Received {msg_count} log messages")
            elif response.is_nack:
                code = response.error_code
                name = parsers.error_name(code)
                self.print_error(f"NACK: {name} (0x{code:02X})")
            else:
                self.print_error(f"Unexpected response type: 0x{response.packet_type:02X}")
        finally:
            self.conn.remove_callback(on_log)
    
    # =========================================================================
    # Response Handling
    # =========================================================================
    
    def _print_response(self, response):
        """Pretty print a response packet in human-readable format."""
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.is_ack:
            self.print_ok("ACK")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
        elif response.is_init_ready:
            self.print_ok("INIT_READY")
            self._handle_init_ready(response.payload)
        elif response.is_identify:
            self.print_ok("IDENTIFY")
            self._handle_init_ready(response.payload)
        elif response.packet_type == CorePacket.STATUS:
            self.print_ok("STATUS")
            parsers.parse_status_payload(response.payload, self.controller_type)
        elif response.packet_type == CorePacket.ERROR:
            self.print_error("ERROR received")
            parsers.parse_error_payload(response.payload)
        elif response.packet_type == CorePacket.I2C_SCAN_RESULT:
            self.print_ok("I2C_SCAN_RESULT")
            parsers.parse_i2c_scan_result(response.payload)
        elif response.packet_type == GearControlPacket.GEAR_CALIB_STATUS:
            parsers.parse_gear_calib_status(response.payload)
        else:
            # Unknown packet type
            pname = parsers.packet_type_name(response.packet_type)
            self.print_info(f"Response: {pname}")
            if response.payload:
                parsers.parse_generic_payload(response.payload)
    
    def _handle_init_ready(self, payload: bytes):
        """Parse and handle INIT_READY response."""
        info = parsers.parse_init_ready(payload)
        
        if info is None:
            print(f"  Raw: {payload.hex()}")
            return
        
        parsers.print_init_ready_info(info)
        
        # Notify about detected controller type
        if info.controller_type:
            self.controller_type = info.controller_type
            ctrl_names = {
                ControllerType.GEARCONTROL: ("GearControl", "gc.*"),
                ControllerType.GUNFX: ("GunFX", "gfx.*"),
                ControllerType.HUBFX: ("HubFX", "hub.*, gfx.*, lfx.*, gc.*"),
                ControllerType.LIGHTFX: ("LightFX", "lfx.*"),
                ControllerType.NOOP: ("NoOp", "core"),
            }
            name, cmds = ctrl_names.get(info.controller_type, ("Unknown", ""))
            self.print_info(f"Detected {name} controller - {cmds} commands now available")
            
            if self._on_controller_detected:
                self._on_controller_detected(info.controller_type, info.name, info.version)
        else:
            self.print_info(f"Unknown controller type: {info.name}")
        
        print(f"  Type 'help' to see available commands")
