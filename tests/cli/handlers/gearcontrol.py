"""
GearControl Command Handlers

GearControl-specific CLI commands:
- Gear control (deploy, retract, stop, all)
- Servo control and configuration
- Door configuration
- Yaw control and configuration
- Gear configuration (stall current, timeout, flags)
"""

from typing import List, Dict, Tuple, Callable

from tests.framework import GearControlCommands, CommandBuilder, CorePacket
from tests.framework.packets import DoorMode
from ..base import CommandHandlerBase, CommandInfo, ControllerType


# Gear state names for display
GEAR_STATE_NAMES = {
    0: "UNKNOWN",
    1: "DEPLOYED",
    2: "RETRACTED",
    3: "DEPLOYING",
    4: "RETRACTING",
    5: "ERROR",
    6: "CALIBRATING",
}

# Calibration phase names for display
CALIB_PHASE_NAMES = {
    0: "IDLE",
    1: "CLEAR_RUN",
    2: "CLEAR_SETTLE",
    3: "DEPLOY_RUN",
    4: "MID_SETTLE",
    5: "RETRACT_RUN",
    6: "COMPLETE",
    7: "ERROR",
    8: "CANCELLED",
    9: "OPENING_DOORS",
    10: "CLOSING_DOORS",
}


class GearControlCommandHandler(CommandHandlerBase):
    """
    Handler for GearControl-specific commands.
    
    These commands are only available when connected to a GearControl controller.
    """

    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return GearControl command registry."""
        return {
            'gc.deploy': (self.cmd_deploy, CommandInfo(
                'gc.deploy', 'gc.deploy <gear_id> | all',
                'Deploy landing gear (0=nose, 1=left, 2=right, all)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.retract': (self.cmd_retract, CommandInfo(
                'gc.retract', 'gc.retract <gear_id> | all',
                'Retract landing gear',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.stop': (self.cmd_stop, CommandInfo(
                'gc.stop', 'gc.stop <gear_id> | all',
                'Emergency stop motor',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.servo': (self.cmd_servo, CommandInfo(
                'gc.servo', 'gc.servo set <id> <pulse_us>',
                'Set servo position (0-7, 500-2500µs)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.servo.config': (self.cmd_servo_config, CommandInfo(
                'gc.servo.config', 'gc.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits and motion profile',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.gear.config': (self.cmd_gear_config, CommandInfo(
                'gc.gear.config', 'gc.gear.config <id> <flags...> [stall_mA] [timeout_ms]',
                'Configure gear behavior (flags: yaw none)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.door.config': (self.cmd_door_config, CommandInfo(
                'gc.door.config', 'gc.door.config <id> <open0> <close0> <open1> <close1>',
                'Configure door servo positions',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.door.mode': (self.cmd_door_mode, CommandInfo(
                'gc.door.mode', 'gc.door.mode <gear_id> <pre_deploy> [post_deploy] [delay_ms]',
                'Set door modes (none, single, dual-sync, dual-delay, dual-seq)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.yaw.config': (self.cmd_yaw_config, CommandInfo(
                'gc.yaw.config', 'gc.yaw.config <gear_id> <neutral> <min> <max>',
                'Configure yaw servo',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.yaw': (self.cmd_yaw_input, CommandInfo(
                'gc.yaw', 'gc.yaw <position_us>',
                'Set yaw position (active when associated gear deployed)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.calibrate': (self.cmd_calibrate, CommandInfo(
                'gc.calibrate', 'gc.calibrate <gear_id> | all [timeout_s]',
                'Calibrate stall current (runs motor both directions, emits progress)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.calibrate.cancel': (self.cmd_calibrate_cancel, CommandInfo(
                'gc.calibrate.cancel', 'gc.calibrate.cancel <gear_id> | all',
                'Cancel calibration in progress',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.reset': (self.cmd_reset, CommandInfo(
                'gc.reset', 'gc.reset <gear_id> | all',
                'Clear error state (ERROR → UNKNOWN)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.enable': (self.cmd_enable, CommandInfo(
                'gc.enable', 'gc.enable <gear_id> | all',
                'Enable gear channel',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.disable': (self.cmd_disable, CommandInfo(
                'gc.disable', 'gc.disable <gear_id> | all',
                'Disable gear channel (rejects deploy/retract/calibrate)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gc.battery': (self.cmd_battery_config, CommandInfo(
                'gc.battery', 'gc.battery <on|off> [autodeploy]',
                'Enable/disable battery monitoring (off by default, enable when battery connected)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
        }

    # =========================================================================
    # Gear Control Commands
    # =========================================================================

    def cmd_deploy(self, args: List[str]):
        """Deploy landing gear."""
        if not args:
            self.print_error("Usage: gc.deploy <gear_id> | all")
            return

        if args[0].lower() == 'all':
            self._send_ack(GearControlCommands.gear_all(1), "Deploy ALL gears")  # ACTION_DEPLOY
        else:
            try:
                gear_id = int(args[0])
                names = {0: "nose", 1: "left main", 2: "right main"}
                self._send_ack(GearControlCommands.gear_deploy(gear_id),
                               f"Deploy gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_retract(self, args: List[str]):
        """Retract landing gear."""
        if not args:
            self.print_error("Usage: gc.retract <gear_id> | all")
            return

        if args[0].lower() == 'all':
            self._send_ack(GearControlCommands.gear_all(0), "Retract ALL gears")  # ACTION_RETRACT
        else:
            try:
                gear_id = int(args[0])
                names = {0: "nose", 1: "left main", 2: "right main"}
                self._send_ack(GearControlCommands.gear_retract(gear_id),
                               f"Retract gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_stop(self, args: List[str]):
        """Emergency stop motor."""
        if not args:
            self.print_error("Usage: gc.stop <gear_id> | all")
            return

        if args[0].lower() == 'all':
            self._send_ack(GearControlCommands.gear_all(2), "STOP ALL motors")  # ACTION_STOP
        else:
            try:
                gear_id = int(args[0])
                self._send_ack(GearControlCommands.gear_stop(gear_id),
                               f"STOP motor {gear_id}")
            except ValueError:
                self.print_error("Invalid gear ID")

    # =========================================================================
    # Servo Commands
    # =========================================================================

    def cmd_servo(self, args: List[str]):
        """GearControl servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gc.servo set <id> <pulse_us>")
            return

        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            self._send_ack(GearControlCommands.servo_set(servo_id, pulse),
                           f"Servo {servo_id} → {pulse}µs")
        except ValueError:
            self.print_error("Invalid servo parameters")

    def cmd_servo_config(self, args: List[str]):
        """GearControl servo configuration (SRV_SETTINGS pattern)."""
        if len(args) < 3:
            self.print_error("Usage: gc.servo.config <id> <min> <max> [speed] [accel] [decel]")
            self.print_info("  Defaults: speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²")
            return

        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000

            self._send_ack(GearControlCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel),
                           f"Servo {servo_id} configured: range {min_us}-{max_us}µs, "
                           f"speed={speed}µs/s, accel={accel}µs/s², decel={decel}µs/s²")
        except ValueError:
            self.print_error("Invalid servo config parameters")

    # =========================================================================
    # Gear/Door Configuration Commands
    # =========================================================================

    def cmd_gear_config(self, args: List[str]):
        """Configure gear behavior with human-readable flags."""
        if len(args) < 2:
            self.print_error("Usage: gc.gear.config <id> <flags...> [stall_mA] [timeout_ms]")
            self.print_info("  Flags (space-separated):")
            self.print_info("    yaw            This gear has a yaw servo")
            self.print_info("    none           No flags (clear all)")
            self.print_info("  Defaults: stall=500mA, timeout=60000ms (override with calibration)")
            self.print_info("  Example: gc.gear.config 0 yaw")
            return

        FLAG_MAP = {
            'yaw':           0x01,
        }

        try:
            gear_id = int(args[0])
            flags = 0
            numeric_args = []  # stall_mA, timeout_ms
            has_flag_token = False

            for arg in args[1:]:
                lower = arg.lower()
                if lower in FLAG_MAP:
                    flags |= FLAG_MAP[lower]
                    has_flag_token = True
                elif lower == 'none':
                    flags = 0
                    has_flag_token = True
                else:
                    # Must be a numeric arg (stall or timeout)
                    numeric_args.append(int(arg))

            if not has_flag_token and len(numeric_args) == 0:
                self.print_error("Specify at least one flag: close-retract, close-deploy, yaw, none")
                return

            # If user only gave numbers (backward compat: first arg could be raw flags value)
            if not has_flag_token and len(numeric_args) >= 1:
                flags = numeric_args.pop(0)

            stall_mA = numeric_args[0] if len(numeric_args) > 0 else 500
            timeout_ms = numeric_args[1] if len(numeric_args) > 1 else 60000

            flag_parts = []
            if flags & 0x01: flag_parts.append("yaw")
            flag_str = ', '.join(flag_parts) if flag_parts else "none"
            self._send_ack(GearControlCommands.gear_config(gear_id, flags, stall_mA, timeout_ms),
                           f"Gear {gear_id}: flags=[{flag_str}], stall={stall_mA}mA, timeout={timeout_ms}ms")
        except ValueError:
            self.print_error("Invalid gear config parameters")

    def cmd_door_config(self, args: List[str]):
        """Configure door servo positions."""
        if len(args) < 5:
            self.print_error("Usage: gc.door.config <gear_id> <open0_us> <close0_us> <open1_us> <close1_us>")
            return

        try:
            gear_id = int(args[0])
            open0 = int(args[1])
            close0 = int(args[2])
            open1 = int(args[3])
            close1 = int(args[4])

            self._send_ack(GearControlCommands.door_config(gear_id, open0, close0, open1, close1),
                           f"Gear {gear_id} doors: A={close0}-{open0}µs, B={close1}-{open1}µs")
        except ValueError:
            self.print_error("Invalid door config parameters")

    def cmd_door_mode(self, args: List[str]):
        """Configure or query door activation modes (two-mode system)."""
        if not args:
            if self._packet_wrapper:
                self.print_error("Usage: gc.door.mode <gear_id> <pre_deploy> [post_deploy] [delay_ms]")
                self.print_info("  Modes: none, single, dual-sync, dual-delay, dual-seq")
                return
            self._show_door_modes()
            return

        if len(args) == 1 and args[0].lower() in ('help', '?'):
            self.print_info("Usage: gc.door.mode [gear_id] [pre_deploy] [post_deploy] [delay_ms]")
            self.print_info("  No args:       show current door modes")
            self.print_info("  pre_deploy:    doors opened before deploy motor, closed after retract motor")
            self.print_info("  post_deploy:   doors closed after deploy motor, opened before retract motor")
            self.print_info("                 (retract is the reverse of deploy; none=skip)")
            self.print_info("  Modes:         none, single, dual-sync, dual-delay, dual-seq")
            self.print_info("  delay_ms:      delay between doors (dual-delay only, default 500)")
            return

        if len(args) < 2:
            self.print_error("Usage: gc.door.mode <gear_id> <pre_deploy> [post_deploy] [delay_ms]")
            return

        MODE_MAP = {
            'none': 0, 'single': 1, 'dual-sync': 2, 'sync': 2,
            'dual-delay': 3, 'delay': 3, 'dual-seq': 4, 'seq': 4,
            'sequential': 4,
        }

        def _parse_mode(s: str) -> int:
            lower = s.lower()
            if lower in MODE_MAP:
                return MODE_MAP[lower]
            return int(s)

        try:
            gear_id = int(args[0])
            mode = _parse_mode(args[1])

            # post_deploy defaults to NONE (skip post-deploy close / pre-retract open)
            post_deploy = 0
            delay_ms = 500

            if len(args) > 2:
                # Check if arg[2] is a mode name or a number that could be a mode
                try:
                    post_deploy = _parse_mode(args[2])
                    delay_ms = int(args[3]) if len(args) > 3 else 500
                except ValueError:
                    # arg[2] might be delay_ms if it's purely numeric and > 4
                    val = int(args[2])
                    if val > 4:
                        delay_ms = val
                    else:
                        post_deploy = val

            mode_name = DoorMode.name(mode)
            post_name = DoorMode.name(post_deploy)
            msg = f"Gear {gear_id} pre-deploy={mode_name} post-deploy={post_name}"
            if mode == DoorMode.DUAL_DELAY or post_deploy == DoorMode.DUAL_DELAY:
                msg += f" (delay={delay_ms}ms)"
            self._send_ack(GearControlCommands.door_mode(gear_id, mode, post_deploy, delay_ms), msg)
        except ValueError:
            self.print_error("Invalid door mode parameters")

    def _show_door_modes(self):
        """Query STATUS and display current door modes per gear."""
        gear_names = ['Nose', 'Left Main', 'Right Main']

        response = self.conn.send_and_wait(CommandBuilder.status_req())
        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.packet_type != CorePacket.STATUS:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")
            return

        # Module data starts after 20-byte core header
        data = response.payload[20:]
        if len(data) < 47:
            self.print_error("STATUS payload too short for door mode data")
            return

        # Packed door modes at bytes 44-46: low nibble = doorPreDeploy, high nibble = doorPostDeploy
        self.print_info("Door modes:")
        for i in range(3):
            packed = data[44 + i]
            mode = packed & 0x0F
            post_deploy = (packed >> 4) & 0x0F
            mode_name = DoorMode.name(mode).lower().replace('_', '-')
            post_name = DoorMode.name(post_deploy).lower().replace('_', '-')
            post_str = f"  post={post_name}" if post_deploy != 0 else "  post=skip"
            self.print_info(f"  Gear {i} ({gear_names[i]:>10}): pre={mode_name}{post_str}")

    # =========================================================================
    # Yaw Commands
    # =========================================================================

    def cmd_yaw_config(self, args: List[str]):
        """Configure yaw servo."""
        if len(args) < 4:
            self.print_error("Usage: gc.yaw.config <gear_id> <neutral_us> <min_us> <max_us>")
            return

        try:
            gear_id = int(args[0])
            neutral = int(args[1])
            min_us = int(args[2])
            max_us = int(args[3])

            self._send_ack(GearControlCommands.yaw_config(gear_id, neutral, min_us, max_us),
                           f"Yaw configured: gear={gear_id}, neutral={neutral}µs, range={min_us}-{max_us}µs")
        except ValueError:
            self.print_error("Invalid yaw config parameters")

    def cmd_yaw_input(self, args: List[str]):
        """Set yaw position."""
        if not args:
            self.print_error("Usage: gc.yaw <position_us>")
            return

        try:
            position = int(args[0])
            self._send_ack(GearControlCommands.yaw_input(position),
                           f"Yaw → {position}µs")
        except ValueError:
            self.print_error("Invalid yaw position")

    # =========================================================================
    # Calibration Commands
    # =========================================================================

    def cmd_calibrate(self, args: List[str]):
        """Start stall current calibration for a gear or all gears."""
        if not args:
            self.print_error("Usage: gc.calibrate <gear_id> | all [timeout_s]")
            self.print_info("  Runs motor in both directions to detect stall current.")
            self.print_info("  Server emits GEAR_CALIB_STATUS packets during calibration.")
            self.print_info("  Optional timeout_s: abort calibration after N seconds (0=no timeout).")
            self.print_info("  Use 'gc.calibrate.cancel <gear_id> | all' to abort.")
            return

        names = {0: "nose", 1: "left main", 2: "right main"}

        if args[0].lower() == 'all':
            timeout_s = int(args[1]) if len(args) > 1 else 0
            timeout_str = f" (timeout={timeout_s}s)" if timeout_s > 0 else ""
            for gear_id in range(3):
                self._send_ack(GearControlCommands.gear_calibrate(gear_id, timeout_s),
                               f"Calibrating gear {gear_id} ({names.get(gear_id, '?')}){timeout_str}")
            return

        try:
            gear_id = int(args[0])
            timeout_s = int(args[1]) if len(args) > 1 else 0
            timeout_str = f" (timeout={timeout_s}s)" if timeout_s > 0 else ""
            self._send_ack(GearControlCommands.gear_calibrate(gear_id, timeout_s),
                           f"Calibrating gear {gear_id} ({names.get(gear_id, '?')}) - progress updates incoming{timeout_str}")
        except ValueError:
            self.print_error("Invalid gear ID or timeout")

    def cmd_calibrate_cancel(self, args: List[str]):
        """Cancel stall current calibration for a gear or all gears."""
        if not args:
            self.print_error("Usage: gc.calibrate.cancel <gear_id> | all")
            return

        names = {0: "nose", 1: "left main", 2: "right main"}

        if args[0].lower() == 'all':
            for gear_id in range(3):
                self._send_ack(GearControlCommands.gear_calib_cancel(gear_id),
                               f"Cancelled calibration for gear {gear_id} ({names.get(gear_id, '?')})")
            return

        try:
            gear_id = int(args[0])
            self._send_ack(GearControlCommands.gear_calib_cancel(gear_id),
                           f"Cancelled calibration for gear {gear_id} ({names.get(gear_id, '?')})")
        except ValueError:
            self.print_error("Invalid gear ID")

    # =========================================================================
    # Error Reset and Channel Enable/Disable Commands
    # =========================================================================

    def cmd_reset(self, args: List[str]):
        """Clear error state for a gear or all gears."""
        if not args:
            self.print_error("Usage: gc.reset <gear_id> | all")
            return

        names = {0: "nose", 1: "left main", 2: "right main"}

        if args[0].lower() == 'all':
            for gear_id in range(3):
                self._send_ack(GearControlCommands.gear_reset(gear_id),
                               f"Reset gear {gear_id} ({names.get(gear_id, '?')})")
            return

        try:
            gear_id = int(args[0])
            self._send_ack(GearControlCommands.gear_reset(gear_id),
                           f"Reset gear {gear_id} ({names.get(gear_id, '?')})")
        except ValueError:
            self.print_error("Invalid gear ID")

    def cmd_enable(self, args: List[str]):
        """Enable gear channel."""
        if not args:
            self.print_error("Usage: gc.enable <gear_id> | all")
            return

        names = {0: "nose", 1: "left main", 2: "right main"}

        if args[0].lower() == 'all':
            for gear_id in range(3):
                self._send_ack(GearControlCommands.gear_enable(gear_id, True),
                               f"Enabled gear {gear_id} ({names.get(gear_id, '?')})")
            return

        try:
            gear_id = int(args[0])
            self._send_ack(GearControlCommands.gear_enable(gear_id, True),
                           f"Enabled gear {gear_id} ({names.get(gear_id, '?')})")
        except ValueError:
            self.print_error("Invalid gear ID")

    def cmd_disable(self, args: List[str]):
        """Disable gear channel (rejects deploy/retract/calibrate)."""
        if not args:
            self.print_error("Usage: gc.disable <gear_id> | all")
            return

        names = {0: "nose", 1: "left main", 2: "right main"}

        if args[0].lower() == 'all':
            for gear_id in range(3):
                self._send_ack(GearControlCommands.gear_enable(gear_id, False),
                               f"Disabled gear {gear_id} ({names.get(gear_id, '?')})")
            return

        try:
            gear_id = int(args[0])
            self._send_ack(GearControlCommands.gear_enable(gear_id, False),
                           f"Disabled gear {gear_id} ({names.get(gear_id, '?')})")
        except ValueError:
            self.print_error("Invalid gear ID")

    # =========================================================================
    # Battery Configuration Commands
    # =========================================================================

    def cmd_battery_config(self, args: List[str]):
        """Enable/disable battery monitoring and auto-deploy."""
        if not args:
            self.print_error("Usage: gc.battery <on|off> [autodeploy]")
            self.print_info("  on           Enable battery voltage monitoring")
            self.print_info("  off          Disable monitoring (default at boot)")
            self.print_info("  on autodeploy  Enable monitoring + auto-deploy on low voltage")
            return

        value = args[0].lower()
        if value in ('on', '1', 'true', 'yes', 'enable'):
            enabled = True
        elif value in ('off', '0', 'false', 'no', 'disable'):
            enabled = False
        else:
            self.print_error(f"Invalid value '{args[0]}' — use 'on' or 'off'")
            return

        # Check for optional autodeploy flag
        auto_deploy = False
        if len(args) > 1 and args[1].lower() == 'autodeploy':
            auto_deploy = True

        if enabled:
            state = "ENABLED"
            if auto_deploy:
                state += " + auto-deploy"
        else:
            state = "DISABLED"
        self._send_ack(GearControlCommands.battery_config(enabled, auto_deploy),
                       f"Battery monitoring: {state}")

