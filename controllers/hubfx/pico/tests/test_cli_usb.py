"""
Tests for USB Host CLI commands

Tests the 'usb' command group for USB Host functionality.
These tests mock the serial responses since USB Host depends
on physical device connections.
"""

import pytest
import json
from conftest import SerialConnection


# ============================================================================
# USB CLI Tests
# ============================================================================

class TestUsbStatus:
    """Test USB status command"""
    
    def test_usb_status(self, fresh_pico: SerialConnection):
        """usb status shows initialization state"""
        response = fresh_pico.send("usb status")
        assert "initialized" in response.lower() or "USB Host" in response
    
    def test_usb_status_json(self, fresh_pico: SerialConnection):
        """usb status --json returns valid JSON"""
        response = fresh_pico.send("usb status --json")
        data = json.loads(response.strip().split('\n')[-1])
        assert "initialized" in data

class TestUsbList:
    """Test USB device listing"""
    
    def test_usb_list(self, fresh_pico: SerialConnection):
        """usb list shows device list or empty message"""
        response = fresh_pico.send("usb list")
        # Should either show devices or "No devices connected"
        assert "device" in response.lower() or "USB" in response or "cdc" in response.lower()
    
    def test_usb_alone(self, fresh_pico: SerialConnection):
        """usb alone should behave like usb list"""
        response = fresh_pico.send("usb")
        assert "device" in response.lower() or "USB" in response or "cdc" in response.lower()
    
    def test_usb_list_json(self, fresh_pico: SerialConnection):
        """usb list --json returns valid JSON"""
        response = fresh_pico.send("usb --json")
        data = json.loads(response.strip().split('\n')[-1])
        # Should have cdcDeviceCount or error
        assert "cdcDeviceCount" in data or "error" in data

class TestUsbInfo:
    """Test USB device info command"""
    
    def test_usb_info_no_index(self, fresh_pico: SerialConnection):
        """usb info without index shows usage"""
        response = fresh_pico.send("usb info")
        assert "usage" in response.lower() or "index" in response.lower()
    
    def test_usb_info_invalid_index(self, fresh_pico: SerialConnection):
        """usb info with invalid index shows error"""
        response = fresh_pico.send("usb info 255")
        # Should show "no device" error since index 255 is unlikely to exist
        assert "no" in response.lower() or "error" in response.lower()

class TestUsbHelp:
    """Test USB help integration"""
    
    def test_help_includes_usb(self, fresh_pico: SerialConnection):
        """Main help should include USB commands"""
        response = fresh_pico.send("help")
        assert "usb" in response.lower()
