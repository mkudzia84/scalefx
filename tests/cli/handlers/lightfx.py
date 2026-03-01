"""
LightFX Command Handlers

LightFX-specific CLI commands:
- LED direct control and sequences
- Servo control and configuration
- Power monitoring (INA226)
"""

from typing import List, Dict, Tuple, Callable

from tests.framework import LightFxCommands, LightFxPacket
from ..base import CommandHandlerBase, CommandInfo, ControllerType
from .. import parsers


class LightFxCommandHandler(CommandHandlerBase):
    """
    Handler for LightFX-specific commands.
    
    These commands are only available when connected to a LightFX controller.
    """
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return LightFX command registry."""
        return {
            'lightfx.led': (self.cmd_led, CommandInfo(
                'lightfx.led', 'lightfx.led set <ch> <brightness> | lightfx.led off [ch]',
                'Control LED (1-8, 0-255)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.led.seq': (self.cmd_led_seq, CommandInfo(
                'lightfx.led.seq', 'lightfx.led.seq clear|start|stop|restart <ch>',
                'Control LED sequences', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.led.seq.add': (self.cmd_led_seq_add, CommandInfo(
                'lightfx.led.seq.add', 'lightfx.led.seq.add <ch> <event> <params...>',
                'Add sequence event (on/off/flash/fadein/fadeout)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.led.seq.status': (self.cmd_led_seq_status, CommandInfo(
                'lightfx.led.seq.status', 'lightfx.led.seq.status <ch>',
                'Get LED sequence status', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.led.seq.queue': (self.cmd_led_seq_queue, CommandInfo(
                'lightfx.led.seq.queue', 'lightfx.led.seq.queue <ch>',
                'List LED sequence event queue', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.led.status': (self.cmd_led_status, CommandInfo(
                'lightfx.led.status', 'lightfx.led.status',
                'Get all LED channel statuses', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.servo': (self.cmd_servo, CommandInfo(
                'lightfx.servo', 'lightfx.servo set <id> <pulse_us>',
                'Set servo position (1-3)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.servo.config': (self.cmd_servo_config, CommandInfo(
                'lightfx.servo.config', 'lightfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.power': (self.cmd_power, CommandInfo(
                'lightfx.power', 'lightfx.power',
                'Request power status (INA226)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lightfx.power.config': (self.cmd_power_config, CommandInfo(
                'lightfx.power.config', 'lightfx.power.config <shunt_mohm> <max_current_ma>',
                'Configure INA226 (shunt in mΩ, max current in mA)', requires_init=True, controller=ControllerType.LIGHTFX)),
        }
    
    # =========================================================================
    # LED Direct Control
    # =========================================================================
    
    def cmd_led(self, args: List[str]):
        """LightFX LED control."""
        if not args:
            self.print_error("Usage: lightfx.led set <ch> <brightness> | lightfx.led off [ch]")
            return
        
        subcmd = args[0].lower()
        
        if subcmd == 'set':
            if len(args) < 3:
                self.print_error("Usage: lightfx.led set <channel> <brightness>")
                return
            try:
                ch = int(args[1])
                brightness = int(args[2])
                packet = LightFxCommands.led_set(ch, brightness)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    pct = (brightness / 255) * 100
                    self.print_ok(f"LED {ch} → {brightness} ({pct:.0f}%)")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid LED parameters")
                
        elif subcmd == 'off':
            ch = int(args[1]) if len(args) > 1 else 0
            packet = LightFxCommands.led_off(ch)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"LED {ch}" if ch > 0 else "All LEDs"
                self.print_ok(f"{target} OFF")
            else:
                self._print_ack_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'set' or 'off'")
    
    # =========================================================================
    # LED Sequence Control
    # =========================================================================
    
    def cmd_led_seq(self, args: List[str]):
        """LightFX LED sequence control."""
        if not args:
            self.print_error("Usage: lightfx.led.seq clear|start|stop|restart <ch>")
            return
        
        subcmd = args[0].lower()
        ch = int(args[1]) if len(args) > 1 else 0
        
        if subcmd == 'clear':
            packet = LightFxCommands.led_seq_clear(ch)
            msg = f"LED {ch} sequence cleared"
        elif subcmd == 'start':
            packet = LightFxCommands.led_seq_start(ch)
            msg = f"LED {ch} sequence started"
        elif subcmd == 'stop':
            packet = LightFxCommands.led_seq_stop(ch)
            msg = f"LED {ch} sequence stopped"
        elif subcmd == 'restart':
            packet = LightFxCommands.led_seq_restart(ch)
            msg = f"LED {ch} sequence restarted"
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'clear', 'start', 'stop', or 'restart'")
            return
        
        success, response = self.conn.send_expect_ack(packet)
        if success:
            self.print_ok(msg)
        else:
            self._print_ack_response(response)
    
    def cmd_led_seq_add(self, args: List[str]):
        """LightFX LED sequence add event."""
        if len(args) < 2:
            self.print_error("Usage: lightfx.led.seq.add <ch> <event> <params...>")
            self.print_info("Events: on, off, flash, fadein, fadeout")
            return
        
        try:
            ch = int(args[0])
            event = args[1].lower()
            
            if event == 'on':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> on <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_on(ch, duration, brightness)
                msg = f"LED {ch}: ON for {duration}ms at brightness {brightness}"
                
            elif event == 'off':
                if len(args) < 3:
                    self.print_error("Usage: lightfx.led.seq.add <ch> off <duration_ms>")
                    return
                duration = int(args[2])
                packet = LightFxCommands.led_seq_add_off(ch, duration)
                msg = f"LED {ch}: OFF for {duration}ms"
                
            elif event == 'flash':
                if len(args) < 5:
                    self.print_error("Usage: lightfx.led.seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
                    return
                interval = int(args[2])
                duration = int(args[3])
                brightness = int(args[4])
                duty = int(args[5]) if len(args) > 5 else 50
                packet = LightFxCommands.led_seq_add_flash(ch, interval, duration, brightness, duty)
                msg = f"LED {ch}: FLASH {interval}ms for {duration}ms, {duty}% duty"
                
            elif event == 'fadein':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> fadein <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_in(ch, duration, brightness)
                msg = f"LED {ch}: FADE IN over {duration}ms to {brightness}"
                
            elif event == 'fadeout':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> fadeout <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_out(ch, duration, brightness)
                msg = f"LED {ch}: FADE OUT over {duration}ms from {brightness}"
            else:
                self.print_error(f"Unknown event: {event}")
                self.print_info("Available: on, off, flash, fadein, fadeout")
                return
            
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Sequence event added: {msg}")
            else:
                self._print_ack_response(response)
            
        except (ValueError, IndexError) as e:
            self.print_error(f"Invalid parameters: {e}")
    
    def cmd_led_seq_status(self, args: List[str]):
        """LightFX LED sequence status."""
        if not args:
            self.print_error("Usage: lightfx.led.seq.status <ch>")
            return
        
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_seq_status(ch)
            response = self.conn.send_and_receive(packet)
            
            if response is None:
                self.print_error("No response (timeout)")
                return
            
            if response.packet_type == LightFxPacket.LED_SEQ_STATUS_RESP:
                status = parsers.parse_led_seq_status(response.payload)
                if status:
                    status_str = "PLAYING" if status['playing'] else "STOPPED"
                    self.print_info(f"LED {status['channel']} Sequence Status:")
                    print(f"  Status:        {status_str}")
                    print(f"  Event Count:   {status['event_count']}")
                    print(f"  Current Index: {status['current_index']}")
                    print(f"  Loop Count:    {status['loop_count']}")
                else:
                    self.print_error("Invalid response payload")
            else:
                self._print_ack_response(response)
                
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_led_seq_queue(self, args: List[str]):
        """LightFX LED sequence queue listing."""
        if not args:
            self.print_error("Usage: lightfx.led.seq.queue <ch>")
            return
        
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_seq_queue(ch)
            response = self.conn.send_and_receive(packet)
            
            if response is None:
                self.print_error("No response (timeout)")
                return
            
            if response.packet_type == LightFxPacket.LED_SEQ_QUEUE_RESP:
                queue = parsers.parse_led_seq_queue(response.payload)
                if queue:
                    status_str = "PLAYING" if queue['playing'] else "STOPPED"
                    self.print_info(f"LED {queue['channel']} Sequence Queue ({status_str}, {queue['count']} events, @ index {queue['current_index']}):")
                    
                    for event in queue['events']:
                        marker = " ← current" if event['index'] == queue['current_index'] else ""
                        print(f"  [{event['index']}] {event['type_name']}: {event['duration']}ms (param={event['param1']}){marker}")
                    
                    if queue['count'] == 0:
                        print("  (empty)")
                else:
                    self.print_error("Invalid response payload")
            else:
                self._print_ack_response(response)
                
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_led_status(self, args: List[str]):
        """LightFX all LED channel status."""
        packet = LightFxCommands.led_status()
        response = self.conn.send_and_receive(packet)
        
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.packet_type == LightFxPacket.LED_STATUS_RESP:
            channels = parsers.parse_led_status(response.payload)
            self.print_info("LED Channel Status:")
            
            for ch in channels:
                brightness = ch['brightness']
                status = "▶" if ch['seq_playing'] else "■"
                bar = "█" * (brightness // 32) + "░" * (8 - brightness // 32)
                print(f"  CH{ch['channel']}: {bar} {brightness:3d}/255 | Seq: {status} ({ch['seq_count']} events)")
        else:
            self._print_ack_response(response)
    
    # =========================================================================
    # Servo Commands
    # =========================================================================
    
    def cmd_servo(self, args: List[str]):
        """LightFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: lightfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = LightFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} → {pulse}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_servo_config(self, args: List[str]):
        """LightFX servo configuration."""
        if len(args) < 3:
            self.print_error("Usage: lightfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            
            packet = LightFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    # =========================================================================
    # Power Commands
    # =========================================================================
    
    def cmd_power(self, args: List[str]):
        """Request power status (LightFX)."""
        response = self.conn.send_and_wait(LightFxCommands.power_status())
        
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.is_nack:
            self._print_ack_response(response)
            return
        
        status = parsers.parse_power_status(response.payload)
        if status:
            self.print_ok("Power Status:")
            print(f"  Voltage: {status['voltage_mv']/1000:.2f}V ({status['voltage_mv']}mV)")
            print(f"  Current: {status['current_ma']}mA")
            print(f"  Power:   {status['power_mw']/1000:.2f}W ({status['power_mw']}mW)")
            print(f"  INA226:  {'Available' if status['available'] else 'Not detected'}")
            
            if 'shunt_mohm' in status:
                print(f"  Config:  Shunt={status['shunt_mohm']}mΩ ({status['shunt_mohm']/1000:.3f}Ω), MaxI={status['max_current_ma']}mA ({status['max_current_ma']/1000:.2f}A)")
        else:
            self.print_info(f"Raw: {response.payload.hex()}")
    
    def cmd_power_config(self, args: List[str]):
        """Configure INA226 power monitor."""
        if len(args) < 2:
            self.print_error("Usage: lightfx.power.config <shunt_mohm> <max_current_ma>")
            self.print_info("  shunt_mohm: Shunt resistor value in milliohms (e.g., 100 for 0.1Ω)")
            self.print_info("  max_current_ma: Maximum expected current in mA (e.g., 3200 for 3.2A)")
            return
        
        try:
            shunt_mohm = int(args[0])
            max_current_ma = int(args[1])
            
            if shunt_mohm <= 0 or max_current_ma <= 0:
                self.print_error("Values must be positive")
                return
            
            packet = LightFxCommands.power_config(shunt_mohm, max_current_ma)
            success, response = self.conn.send_expect_ack(packet)
            
            if success:
                self.print_ok(f"INA226 configured: Shunt={shunt_mohm}mΩ ({shunt_mohm/1000:.3f}Ω), MaxI={max_current_ma}mA ({max_current_ma/1000:.2f}A)")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid parameters - must be integers")
    
    # =========================================================================
    # Response Handling
    # =========================================================================
    
    def _print_ack_response(self, response):
        """Print ACK/NACK response."""
        if response is None:
            self.print_error("No response (timeout)")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
