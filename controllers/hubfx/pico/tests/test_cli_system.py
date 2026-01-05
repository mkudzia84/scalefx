"""
System CLI Tests

Tests for system-level commands: help, version, status, ping, etc.
"""

import pytest
import json
from conftest import SerialConnection


class TestSystemCommands:
    """Test system CLI commands."""
    
    # =========================================================================
    # PING - Basic connectivity test
    # =========================================================================
    
    def test_ping_text(self, fresh_pico: SerialConnection):
        """Test ping command returns pong."""
        response = fresh_pico.send("ping")
        assert "pong" in response.lower()
    
    @pytest.mark.json
    def test_ping_json(self, fresh_pico: SerialConnection):
        """Test ping command with JSON output."""
        result = fresh_pico.send_json("ping")
        assert result.get("pong") == True
        assert "uptimeMs" in result
        assert isinstance(result["uptimeMs"], int)
    
    # =========================================================================
    # VERSION - Firmware version info
    # =========================================================================
    
    def test_version_text(self, fresh_pico: SerialConnection):
        """Test version command returns firmware info."""
        response = fresh_pico.send("version")
        assert "Firmware" in response or "firmware" in response.lower()
        # Build number may be omitted if zero or shown in parentheses
        assert "RP2040" in response or "Pico" in response
    
    @pytest.mark.json
    def test_version_json(self, fresh_pico: SerialConnection):
        """Test version command JSON output has required fields."""
        result = fresh_pico.send_json("version")
        
        assert "firmware" in result
        assert result["firmware"].startswith("v")
        assert "build" in result
        assert isinstance(result["build"], int)
        assert result["platform"] == "RP2040"
        assert "cpuFrequencyMHz" in result
        assert "freeRamBytes" in result
    
    def test_version_alias_ver(self, fresh_pico: SerialConnection):
        """Test 'ver' alias works same as 'version'."""
        response = fresh_pico.send("ver")
        assert "Firmware" in response or "firmware" in response.lower()
    
    # =========================================================================
    # STATUS - System status
    # =========================================================================
    
    def test_status_text(self, fresh_pico: SerialConnection):
        """Test status command returns system info."""
        response = fresh_pico.send("status")
        assert "System Status" in response or "status" in response.lower()
        assert "RAM" in response.upper() or "Firmware" in response
    
    @pytest.mark.json
    def test_status_json(self, fresh_pico: SerialConnection):
        """Test status command JSON output structure."""
        result = fresh_pico.send_json("status")
        
        # System section
        assert "system" in result
        system = result["system"]
        assert "firmware" in system
        assert "uptimeMs" in system
        assert "freeRamBytes" in system
        
        # SD card section
        assert "sdCard" in result
        assert "initialized" in result["sdCard"]
        
        # Slaves array
        assert "slaves" in result
        assert isinstance(result["slaves"], list)
    
    def test_status_alias_sysinfo(self, fresh_pico: SerialConnection):
        """Test 'sysinfo' alias works same as 'status'."""
        response = fresh_pico.send("sysinfo")
        assert "System" in response or "status" in response.lower()
    
    # =========================================================================
    # HELP - Command help
    # =========================================================================
    
    def test_help_text(self, fresh_pico: SerialConnection):
        """Test help command lists available commands."""
        response = fresh_pico.send("help", delay=0.5)
        
        # Should list major command groups
        assert "help" in response.lower()
        assert "version" in response.lower()
        assert "status" in response.lower()
    
    @pytest.mark.json
    def test_help_json(self, fresh_pico: SerialConnection):
        """Test help command JSON lists command groups."""
        result = fresh_pico.send_json("help")
        
        assert "commands" in result
        commands = result["commands"]
        assert isinstance(commands, list)
        assert len(commands) > 0
    
    def test_help_alias_question(self, fresh_pico: SerialConnection):
        """Test '?' alias works as help."""
        response = fresh_pico.send("?", delay=0.5)
        assert "help" in response.lower() or "commands" in response.lower()
    
    # =========================================================================
    # CLEAR - Screen clear (just verify no error)
    # =========================================================================
    
    def test_clear(self, fresh_pico: SerialConnection):
        """Test clear command executes without error."""
        response = fresh_pico.send("clear")
        # Clear sends escape codes, should not return error
        assert "error" not in response.lower()
        assert "unknown" not in response.lower()
    
    def test_clear_alias_cls(self, fresh_pico: SerialConnection):
        """Test 'cls' alias works as clear."""
        response = fresh_pico.send("cls")
        assert "error" not in response.lower()


class TestUnknownCommands:
    """Test handling of invalid/unknown commands."""
    
    def test_unknown_command(self, fresh_pico: SerialConnection):
        """Test that unknown commands report error gracefully."""
        response = fresh_pico.send("notarealcommand")
        assert "unknown" in response.lower() or "error" in response.lower()
    
    def test_empty_command(self, fresh_pico: SerialConnection):
        """Test empty command is handled."""
        response = fresh_pico.send("")
        # Should either be empty or show help, not crash
        assert "error" not in response.lower() or len(response) == 0


class TestJsonFlag:
    """Test JSON flag parsing across commands."""
    
    @pytest.mark.json
    def test_long_json_flag(self, fresh_pico: SerialConnection):
        """Test --json flag is recognized."""
        result = fresh_pico.send_json("ping --json")
        assert result.get("pong") == True
    
    @pytest.mark.json
    def test_short_json_flag(self, fresh_pico: SerialConnection):
        """Test -j short flag is recognized."""
        response = fresh_pico.send("ping -j")
        assert "{" in response  # Should be JSON
        data = json.loads(response.strip().split('\n')[-1])
        assert data.get("pong") == True
    
    @pytest.mark.json
    def test_json_flag_position(self, fresh_pico: SerialConnection):
        """Test --json flag works at end of command."""
        result = fresh_pico.send_json("version --json")
        assert "firmware" in result
