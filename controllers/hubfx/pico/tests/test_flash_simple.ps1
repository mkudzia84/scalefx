# Simple Flash Test
# Quick test of flash init and basic operations

Write-Host "`n=== Simple Flash Test ===" -ForegroundColor Cyan

Write-Host "`nTesting flash init..." -ForegroundColor Yellow
python -c @"
import serial, time
s = serial.Serial('COM10', 115200, timeout=2)
time.sleep(0.5)
s.write(b'flash init\r\n')
time.sleep(1)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.write(b'flash info\r\n')
time.sleep(0.5)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.write(b'flash ls\r\n')
time.sleep(0.5)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.close()
"@

Write-Host "`n=== Test Complete ===" -ForegroundColor Cyan
