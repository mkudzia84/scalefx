"""
GunFX Command Handlers

GunFX-specific CLI commands:
- Trigger control (on/off with RPM)
- Servo control and configuration
- Smoke heater and fan control
"""

from typing import List, Dict, Tuple, Callable

from tests.framework import GunFxCommands
from ..base import CommandHandlerBase, CommandInfo, ControllerType


class GunFxCommandHandler(CommandHandlerBase):
    """
    Handler for GunFX-specific commands.
    
    These commands are only available when connected to a GunFX controller.
    """
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return GunFX command registry."""
        return {
            'gfx.trigger': (self.cmd_trigger, CommandInfo(
                'gfx.trigger', 'gfx.trigger on <rpm> | gfx.trigger off [delay_ms]',
                'Control firing (1-3000 RPM)', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.servo': (self.cmd_servo, CommandInfo(
                'gfx.servo', 'gfx.servo set <id> <pulse_us>',
                'Set servo position (1-3, 500-2500µs)', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.servo.config': (self.cmd_servo_config, CommandInfo(
                'gfx.servo.config', 'gfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.servo.recoil': (self.cmd_servo_recoil, CommandInfo(
                'gfx.servo.recoil', 'gfx.servo.recoil <id> <jerk_us> <variance_us>',
                'Configure recoil effect', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.smoke': (self.cmd_smoke, CommandInfo(
                'gfx.smoke', 'gfx.smoke heat on|off',
                'Control smoke heater', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.smoke.config': (self.cmd_smoke_config, CommandInfo(
                'gfx.smoke.config', 'gfx.smoke.config [key=value ...]',
                'Configure smoke fan (keys: pulsing,speed,high,low,pulse_ms,spindown_ms)',
                requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.smoke.reset': (self.cmd_smoke_reset, CommandInfo(
                'gfx.smoke.reset', 'gfx.smoke.reset',
                'Clear smoke error states (disconnect/overcurrent)', requires_init=True, controller=ControllerType.GUNFX)),
            'gfx.smoke.limit': (self.cmd_smoke_limit, CommandInfo(
                'gfx.smoke.limit', 'gfx.smoke.limit heater|fan <mA>',
                'Set overcurrent protection limit (0=disable)', requires_init=True, controller=ControllerType.GUNFX)),
        }
    
    # =========================================================================
    # Trigger Commands
    # =========================================================================
    
    def cmd_trigger(self, args: List[str]):
        """GunFX trigger control."""
        if not args:
            self.print_error("Usage: gfx.trigger on <rpm> | gfx.trigger off [delay_ms]")
            return
        
        subcmd = args[0].lower()
        
        if subcmd == 'on':
            if len(args) < 2:
                self.print_error("Usage: gfx.trigger on <rpm>")
                return
            try:
                rpm = int(args[1])
                packet = GunFxCommands.trigger_on(rpm)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    self.print_ok(f"Trigger ON at {rpm} RPM")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid RPM value")
                
        elif subcmd == 'off':
            delay = int(args[1]) if len(args) > 1 else 3000
            packet = GunFxCommands.trigger_off(delay)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Trigger OFF (spin-down: {delay}ms)")
            else:
                self._print_ack_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'on' or 'off'")
    
    # =========================================================================
    # Servo Commands
    # =========================================================================
    
    def cmd_servo(self, args: List[str]):
        """GunFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = GunFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} → {pulse}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_servo_config(self, args: List[str]):
        """GunFX servo configuration."""
        if len(args) < 3:
            self.print_error("Usage: gfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            
            packet = GunFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    def cmd_servo_recoil(self, args: List[str]):
        """GunFX servo recoil configuration."""
        if len(args) < 3:
            self.print_error("Usage: gfx.servo.recoil <id> <jerk_us> <variance_us>")
            return
        
        try:
            servo_id = int(args[0])
            jerk = int(args[1])
            variance = int(args[2])
            packet = GunFxCommands.servo_recoil(servo_id, jerk, variance)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} recoil: jerk {jerk}µs, variance ±{variance}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid recoil parameters")
    
    # =========================================================================
    # Smoke Commands
    # =========================================================================
    
    def cmd_smoke(self, args: List[str]):
        """GunFX smoke heater control."""
        if len(args) < 2 or args[0].lower() != 'heat':
            self.print_error("Usage: gfx.smoke heat on|off")
            return
        
        on = args[1].lower() in ('on', '1', 'true', 'yes')
        packet = GunFxCommands.smoke_heat(on)
        success, response = self.conn.send_expect_ack(packet)
        if success:
            self.print_ok(f"Smoke heater {'ON' if on else 'OFF'}")
        else:
            self._print_ack_response(response)
    
    def cmd_smoke_config(self, args: List[str]):
        """GunFX smoke fan configuration (key=value syntax, all optional)."""
        if not args:
            self.print_error(
                "Usage: gfx.smoke.config [key=value ...]\n"
                "  Keys: pulsing (0|1), speed (0-255), high (0-255),\n"
                "        low (0-255), pulse_ms (0=auto, or 1-10000),\n"
                "        spindown_ms (0-60000)\n"
                "  Example: gfx.smoke.config pulsing=1 speed=200\n"
                "  Unspecified params use defaults")
            return
        
        # Defaults match GunFxSmokeConfig struct
        pulsing = False
        speed = 255
        high = 255
        low = 80
        pulse_ms = 0       # 0 = auto-calculate from RPM
        spindown_ms = 5000
        
        try:
            for arg in args:
                if '=' not in arg:
                    self.print_error(f"Invalid parameter '{arg}'. Use key=value format.")
                    return
                key, val = arg.split('=', 1)
                key = key.lower()
                if key == 'pulsing':
                    pulsing = val in ('1', 'true', 'on')
                elif key == 'speed':
                    speed = int(val)
                elif key == 'high':
                    high = int(val)
                elif key == 'low':
                    low = int(val)
                elif key in ('pulse_ms', 'pulse'):
                    pulse_ms = int(val)
                elif key in ('spindown_ms', 'spindown'):
                    spindown_ms = int(val)
                else:
                    self.print_error(f"Unknown parameter '{key}'")
                    return
            
            packet = GunFxCommands.smoke_settings(pulsing, speed, high, low, pulse_ms, spindown_ms)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                mode = "pulsing" if pulsing else "constant"
                pulse_info = "auto" if pulse_ms == 0 else f"{pulse_ms}ms"
                self.print_ok(f"Smoke: {mode}, speed={speed}, high/low={high}/{low}, "
                              f"pulse={pulse_info}, spindown={spindown_ms}ms")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid parameter value (must be numeric)")
    
    def cmd_smoke_reset(self, args: List[str]):
        """Clear smoke error states."""
        packet = GunFxCommands.smoke_reset()
        success, response = self.conn.send_expect_ack(packet)
        if success:
            self.print_ok("Smoke errors cleared")
        else:
            self._print_ack_response(response)

    def cmd_smoke_limit(self, args: List[str]):
        """Set overcurrent protection limit."""
        if len(args) < 2:
            self.print_error("Usage: gfx.smoke.limit heater|fan <mA>  (0=disable)")
            return

        ch_name = args[0].lower()
        if ch_name in ('heater', '0'):
            channel = 0
        elif ch_name in ('fan', '1'):
            channel = 1
        else:
            self.print_error("Channel must be 'heater' (0) or 'fan' (1)")
            return

        try:
            limit_mA = int(args[1])
            packet = GunFxCommands.smoke_current_limit(channel, limit_mA)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                ch_label = 'Heater' if channel == 0 else 'Fan'
                if limit_mA == 0:
                    self.print_ok(f"{ch_label} overcurrent protection disabled")
                else:
                    self.print_ok(f"{ch_label} current limit set to {limit_mA} mA")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid current limit value")
    
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
