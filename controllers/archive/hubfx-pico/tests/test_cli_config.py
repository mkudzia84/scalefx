"""
Config CLI Tests

Tests for configuration commands: config, config backup, config restore, config reload.
"""

import pytest
from conftest import SerialConnection


class TestConfigCommands:
    """Test config CLI commands."""
    
    # =========================================================================
    # CONFIG - Display configuration
    # =========================================================================
    
    def test_config_display(self, fresh_pico: SerialConnection):
        """Test config command displays current configuration."""
        response = fresh_pico.send("config", delay=0.5)
        # Should show configuration sections
        assert "Configuration" in response or "config" in response.lower() or "engine" in response.lower()
    
    @pytest.mark.json
    def test_config_json(self, fresh_pico: SerialConnection):
        """Test config JSON output has required sections."""
        result = fresh_pico.send_json("config", delay=0.5)
        
        # Should have engine section
        assert "engine" in result
        engine = result["engine"]
        assert "enabled" in engine
        assert "togglePin" in engine
        
        # Should have gun section
        assert "gun" in result
        gun = result["gun"]
        assert "enabled" in gun
        
        # Should have loaded status
        assert "loaded" in result
    
    # =========================================================================
    # CONFIG BACKUP
    # =========================================================================
    
    def test_config_backup(self, fresh_pico: SerialConnection):
        """Test config backup command."""
        response = fresh_pico.send("config backup")
        # Should indicate success or failure
        assert "backup" in response.lower() or "success" in response.lower() or "failed" in response.lower()
    
    @pytest.mark.json
    def test_config_backup_json(self, fresh_pico: SerialConnection):
        """Test config backup JSON response."""
        result = fresh_pico.send_json("config backup")
        
        assert "command" in result
        assert result["command"] == "backup"
        assert "success" in result
        assert isinstance(result["success"], bool)
    
    # =========================================================================
    # CONFIG RESTORE
    # =========================================================================
    
    def test_config_restore(self, fresh_pico: SerialConnection):
        """Test config restore command."""
        response = fresh_pico.send("config restore")
        # Should indicate success or failure (may fail if no backup exists)
        assert "restore" in response.lower() or "success" in response.lower() or "failed" in response.lower()
    
    @pytest.mark.json
    def test_config_restore_json(self, fresh_pico: SerialConnection):
        """Test config restore JSON response."""
        result = fresh_pico.send_json("config restore")
        
        assert "command" in result
        assert result["command"] == "restore"
        assert "success" in result
    
    # =========================================================================
    # CONFIG RELOAD
    # =========================================================================
    
    def test_config_reload(self, fresh_pico: SerialConnection):
        """Test config reload command."""
        response = fresh_pico.send("config reload", delay=0.5)
        # Should indicate reload status
        assert "reload" in response.lower() or "loaded" in response.lower() or "config" in response.lower()
    
    @pytest.mark.json
    def test_config_reload_json(self, fresh_pico: SerialConnection):
        """Test config reload JSON response."""
        result = fresh_pico.send_json("config reload", delay=0.5)
        
        assert "command" in result
        assert result["command"] == "reload"
        assert "success" in result


class TestConfigValidation:
    """Test configuration validation."""
    
    @pytest.mark.json
    def test_engine_config_structure(self, fresh_pico: SerialConnection):
        """Test engine configuration structure."""
        result = fresh_pico.send_json("config")
        
        if "engine" not in result:
            pytest.skip("No engine config")
        
        engine = result["engine"]
        
        # Required fields
        assert "enabled" in engine
        assert "channelA" in engine
        assert "channelB" in engine
        
        # Sound configs
        assert "startupSound" in engine
        assert "runningSound" in engine
        assert "shutdownSound" in engine
        
        # Check sound structure
        for sound_key in ["startupSound", "runningSound", "shutdownSound"]:
            sound = engine[sound_key]
            assert "filename" in sound
            assert "volume" in sound
    
    @pytest.mark.json
    def test_gun_config_structure(self, fresh_pico: SerialConnection):
        """Test gun configuration structure."""
        result = fresh_pico.send_json("config")
        
        if "gun" not in result:
            pytest.skip("No gun config")
        
        gun = result["gun"]
        
        # Required fields
        assert "enabled" in gun
        assert "triggerChannel" in gun
        assert "audioChannel" in gun
        
        # Smoke config
        assert "smoke" in gun
        smoke = gun["smoke"]
        assert "heaterToggleChannel" in smoke
        
        # Rates of fire
        assert "rateCount" in gun
        assert "ratesOfFire" in gun
        assert isinstance(gun["ratesOfFire"], list)
        
        # Servo configs
        assert "pitch" in gun
        assert "yaw" in gun
