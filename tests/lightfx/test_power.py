"""
LightFX Power Monitoring Tests
"""

import pytest

from tests.framework import (
    ScaleFXConnection, LightFxCommands,
    LightFxPacket
)
from tests.framework.protocol import read_u16_le, read_i16_le


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxPower:
    """Power monitoring command tests."""
    
    def test_power_status_request(self, lightfx: ScaleFXConnection):
        """POWER_STATUS should return power readings."""
        response = lightfx.send_and_wait(LightFxCommands.power_status())
        
        assert response is not None, "No response"
        assert response.packet_type == LightFxPacket.POWER_STATUS_RESP, \
            f"Expected POWER_STATUS_RESP, got 0x{response.packet_type:02X}"
    
    def test_power_status_payload(self, lightfx: ScaleFXConnection):
        """POWER_STATUS response should contain valid readings."""
        response = lightfx.send_and_wait(LightFxCommands.power_status())
        
        assert response is not None
        if response.packet_type != LightFxPacket.POWER_STATUS_RESP:
            pytest.skip("Power monitoring not available")
        
        # Payload: [voltage:u16(mV)][current:i16(mA)][power:u16(mW)][available:u8]
        if len(response.payload) >= 7:
            voltage_mv = read_u16_le(response.payload, 0)
            current_ma = read_i16_le(response.payload, 2)
            power_mw = read_u16_le(response.payload, 4)
            available = response.payload[6]
            
            print(f"Voltage: {voltage_mv}mV, Current: {current_ma}mA, "
                  f"Power: {power_mw}mW, INA226 available: {available}")
            
            # Sanity checks
            assert 0 <= voltage_mv <= 60000, f"Voltage out of range: {voltage_mv}mV"
            assert -30000 <= current_ma <= 30000, f"Current out of range: {current_ma}mA"
    
    def test_power_status_ina226_not_present(self, lightfx: ScaleFXConnection):
        """Handle case where INA226 is not connected."""
        response = lightfx.send_and_wait(LightFxCommands.power_status())
        
        if response is None:
            pytest.skip("No response - power monitoring may not be implemented")
        
        if response.is_nack:
            # OK - device reports power monitoring not available
            pass
        else:
            assert response.packet_type == LightFxPacket.POWER_STATUS_RESP
