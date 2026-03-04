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

from tests.framework import GearControlCommands
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
            'gearcontrol.deploy': (self.cmd_deploy, CommandInfo(
                'gearcontrol.deploy', 'gearcontrol.deploy <gear_id> | all',
                'Deploy landing gear (0=nose, 1=left, 2=right, all)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.retract': (self.cmd_retract, CommandInfo(
                'gearcontrol.retract', 'gearcontrol.retract <gear_id> | all',
                'Retract landing gear',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.stop': (self.cmd_stop, CommandInfo(
                'gearcontrol.stop', 'gearcontrol.stop <gear_id> | all',
                'Emergency stop motor',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.servo': (self.cmd_servo, CommandInfo(
                'gearcontrol.servo', 'gearcontrol.servo set <id> <pulse_us>',
                'Set servo position (0-7, 500-2500µs)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.servo.config': (self.cmd_servo_config, CommandInfo(
                'gearcontrol.servo.config', 'gearcontrol.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits and motion profile (matches GunFX/LightFX pattern)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.gear.config': (self.cmd_gear_config, CommandInfo(
                'gearcontrol.gear.config', 'gearcontrol.gear.config <id> <flags> <stall_mA> <timeout_ms>',
                'Configure gear behavior',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.door.config': (self.cmd_door_config, CommandInfo(
                'gearcontrol.door.config', 'gearcontrol.door.config <id> <open0> <close0> <open1> <close1>',
                'Configure door servo positions',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.door.mode': (self.cmd_door_mode, CommandInfo(
                'gearcontrol.door.mode', 'gearcontrol.door.mode <gear_id> <mode> [delay_ms]',
                'Set door mode (0=none, 1=single, 2=dual-sync, 3=dual-delay, 4=dual-seq)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.yaw.config': (self.cmd_yaw_config, CommandInfo(
                'gearcontrol.yaw.config', 'gearcontrol.yaw.config <gear_id> <neutral> <min> <max>',
                'Configure yaw servo',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.yaw': (self.cmd_yaw_input, CommandInfo(
                'gearcontrol.yaw', 'gearcontrol.yaw <position_us>',
                'Set yaw position (active when associated gear deployed)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.calibrate': (self.cmd_calibrate, CommandInfo(
                'gearcontrol.calibrate', 'gearcontrol.calibrate <gear_id>',
                'Calibrate stall current (runs motor both directions, emits progress)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.calibrate.cancel': (self.cmd_calibrate_cancel, CommandInfo(
                'gearcontrol.calibrate.cancel', 'gearcontrol.calibrate.cancel <gear_id>',
                'Cancel calibration in progress',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
            'gearcontrol.battery': (self.cmd_battery_config, CommandInfo(
                'gearcontrol.battery', 'gearcontrol.battery <on|off> [autodeploy]',
                'Enable/disable battery monitoring (off by default, enable when battery connected)',
                requires_init=True, controller=ControllerType.GEARCONTROL)),
        }

    # =========================================================================
    # Gear Control Commands
    # =========================================================================

    def cmd_deploy(self, args: List[str]):
        """Deploy landing gear."""
        if not args:
            self.print_error("Usage: gearcontrol.deploy <gear_id> | all")
            return

        if args[0].lower() == 'all':
            packet = GearControlCommands.gear_all(1)  # ACTION_DEPLOY
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok("Deploy ALL gears")
            else:
                self._print_ack_response(response)
        else:
            try:
                gear_id = int(args[0])
                packet = GearControlCommands.gear_deploy(gear_id)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    names = {0: "nose", 1: "left main", 2: "right main"}
                    self.print_ok(f"Deploy gear {gear_id} ({names.get(gear_id, '?')})")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_retract(self, args: List[str]):
        """Retract landing gear."""
        if not args:
            self.print_error("Usage: gearcontrol.retract <gear_id> | all")
            return

        if args[0].lower() == 'all':
            packet = GearControlCommands.gear_all(0)  # ACTION_RETRACT
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok("Retract ALL gears")
            else:
                self._print_ack_response(response)
        else:
            try:
                gear_id = int(args[0])
                packet = GearControlCommands.gear_retract(gear_id)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    names = {0: "nose", 1: "left main", 2: "right main"}
                    self.print_ok(f"Retract gear {gear_id} ({names.get(gear_id, '?')})")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_stop(self, args: List[str]):
        """Emergency stop motor."""
        if not args:
            self.print_error("Usage: gearcontrol.stop <gear_id> | all")
            return

        if args[0].lower() == 'all':
            packet = GearControlCommands.gear_all(2)  # ACTION_STOP
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok("STOP ALL motors")
            else:
                self._print_ack_response(response)
        else:
            try:
                gear_id = int(args[0])
                packet = GearControlCommands.gear_stop(gear_id)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    self.print_ok(f"STOP motor {gear_id}")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid gear ID")

    # =========================================================================
    # Servo Commands
    # =========================================================================

    def cmd_servo(self, args: List[str]):
        """GearControl servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gearcontrol.servo set <id> <pulse_us>")
            return

        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = GearControlCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} → {pulse}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")

    def cmd_servo_config(self, args: List[str]):
        """GearControl servo configuration (SRV_SETTINGS pattern)."""
        if len(args) < 3:
            self.print_error("Usage: gearcontrol.servo.config <id> <min> <max> [speed] [accel] [decel]")
            self.print_info("  Defaults: speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²")
            return

        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000

            packet = GearControlCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} configured: range {min_us}-{max_us}µs, "
                              f"speed={speed}µs/s, accel={accel}µs/s², decel={decel}µs/s²")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")

    # =========================================================================
    # Gear/Door Configuration Commands
    # =========================================================================

    def cmd_gear_config(self, args: List[str]):
        """Configure gear behavior."""
        if len(args) < 4:
            self.print_error("Usage: gearcontrol.gear.config <id> <flags> <stall_mA> <timeout_ms>")
            self.print_info("  Flags: bit0=close doors on retract, bit1=close on deploy, bit2=has yaw")
            return

        try:
            gear_id = int(args[0])
            flags = int(args[1])
            stall_mA = int(args[2])
            timeout_ms = int(args[3])

            packet = GearControlCommands.gear_config(gear_id, flags, stall_mA, timeout_ms)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                flag_parts = []
                if flags & 0x01: flag_parts.append("close-on-retract")
                if flags & 0x02: flag_parts.append("close-on-deploy")
                if flags & 0x04: flag_parts.append("has-yaw")
                flag_str = ', '.join(flag_parts) if flag_parts else "none"
                self.print_ok(f"Gear {gear_id}: stall={stall_mA}mA, timeout={timeout_ms}ms, flags=[{flag_str}]")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid gear config parameters")

    def cmd_door_config(self, args: List[str]):
        """Configure door servo positions."""
        if len(args) < 5:
            self.print_error("Usage: gearcontrol.door.config <gear_id> <open0_us> <close0_us> <open1_us> <close1_us>")
            return

        try:
            gear_id = int(args[0])
            open0 = int(args[1])
            close0 = int(args[2])
            open1 = int(args[3])
            close1 = int(args[4])

            packet = GearControlCommands.door_config(gear_id, open0, close0, open1, close1)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Gear {gear_id} doors: A={close0}-{open0}µs, B={close1}-{open1}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid door config parameters")

    def cmd_door_mode(self, args: List[str]):
        """Configure door activation mode."""
        if not args:
            self.print_error("Usage: gearcontrol.door.mode <gear_id> <mode> [delay_ms]")
            self.print_info("  Modes: 0=none, 1=single, 2=dual-sync, 3=dual-delay, 4=dual-seq")
            self.print_info("  delay_ms: delay between doors (dual-delay only, default 500)")
            return

        if len(args) < 2:
            self.print_error("Usage: gearcontrol.door.mode <gear_id> <mode> [delay_ms]")
            return

        try:
            gear_id = int(args[0])

            # Accept mode as number or name
            mode_str = args[1].lower()
            mode_map = {
                'none': 0, 'single': 1, 'dual-sync': 2, 'sync': 2,
                'dual-delay': 3, 'delay': 3, 'dual-seq': 4, 'seq': 4,
                'sequential': 4,
            }
            if mode_str in mode_map:
                mode = mode_map[mode_str]
            else:
                mode = int(args[1])

            delay_ms = int(args[2]) if len(args) > 2 else 500

            packet = GearControlCommands.door_mode(gear_id, mode, delay_ms)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                mode_name = DoorMode.name(mode)
                msg = f"Gear {gear_id} door mode: {mode_name}"
                if mode == DoorMode.DUAL_DELAY:
                    msg += f" (delay={delay_ms}ms)"
                self.print_ok(msg)
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid door mode parameters")

    # =========================================================================
    # Yaw Commands
    # =========================================================================

    def cmd_yaw_config(self, args: List[str]):
        """Configure yaw servo."""
        if len(args) < 4:
            self.print_error("Usage: gearcontrol.yaw.config <gear_id> <neutral_us> <min_us> <max_us>")
            return

        try:
            gear_id = int(args[0])
            neutral = int(args[1])
            min_us = int(args[2])
            max_us = int(args[3])

            packet = GearControlCommands.yaw_config(gear_id, neutral, min_us, max_us)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Yaw configured: gear={gear_id}, neutral={neutral}µs, range={min_us}-{max_us}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid yaw config parameters")

    def cmd_yaw_input(self, args: List[str]):
        """Set yaw position."""
        if not args:
            self.print_error("Usage: gearcontrol.yaw <position_us>")
            return

        try:
            position = int(args[0])
            packet = GearControlCommands.yaw_input(position)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Yaw → {position}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid yaw position")

    # =========================================================================
    # Calibration Commands
    # =========================================================================

    def cmd_calibrate(self, args: List[str]):
        """Start stall current calibration for a gear."""
        if not args:
            self.print_error("Usage: gearcontrol.calibrate <gear_id>")
            self.print_info("  Runs motor in both directions to detect stall current.")
            self.print_info("  Server emits GEAR_CALIB_STATUS packets during calibration.")
            self.print_info("  Use 'gearcontrol.calibrate.cancel <gear_id>' to abort.")
            return

        try:
            gear_id = int(args[0])
            packet = GearControlCommands.gear_calibrate(gear_id)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                names = {0: "nose", 1: "left main", 2: "right main"}
                self.print_ok(f"Calibrating gear {gear_id} ({names.get(gear_id, '?')}) - progress updates incoming")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid gear ID")

    def cmd_calibrate_cancel(self, args: List[str]):
        """Cancel stall current calibration for a gear."""
        if not args:
            self.print_error("Usage: gearcontrol.calibrate.cancel <gear_id>")
            return

        try:
            gear_id = int(args[0])
            packet = GearControlCommands.gear_calib_cancel(gear_id)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                names = {0: "nose", 1: "left main", 2: "right main"}
                self.print_ok(f"Cancelled calibration for gear {gear_id} ({names.get(gear_id, '?')})")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid gear ID")

    # =========================================================================
    # Battery Configuration Commands
    # =========================================================================

    def cmd_battery_config(self, args: List[str]):
        """Enable/disable battery monitoring and auto-deploy."""
        if not args:
            self.print_error("Usage: gearcontrol.battery <on|off> [autodeploy]")
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

        packet = GearControlCommands.battery_config(enabled, auto_deploy)
        success, response = self.conn.send_expect_ack(packet)
        if success:
            if enabled:
                state = "ENABLED"
                if auto_deploy:
                    state += " + auto-deploy"
            else:
                state = "DISABLED"
            self.print_ok(f"Battery monitoring: {state}")
        else:
            self._print_ack_response(response)

    # =========================================================================
    # Response Handling
    # =========================================================================

    def _print_ack_response(self, response):
        """Print ACK/NACK response."""
        from .. import parsers

        if response is None:
            self.print_error("No response (timeout)")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
