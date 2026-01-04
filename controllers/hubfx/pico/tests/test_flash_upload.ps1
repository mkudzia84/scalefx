# Flash Upload Test
# Tests flow-controlled flash upload functionality

Write-Host "`n=== Flash Upload Test ===" -ForegroundColor Cyan

$ErrorActionPreference = "Stop"
$testFile = Join-Path $PSScriptRoot "test_data.txt"

# Create test file
"This is a test file for flash upload.`nLine 2`nLine 3`nLine 4`nLine 5" | Out-File -FilePath $testFile -Encoding ASCII -NoNewline

Write-Host "`n[1] Initializing flash..." -ForegroundColor Yellow
python -c @"
import serial, time
s = serial.Serial('COM10', 115200, timeout=2)
time.sleep(0.5)
s.write(b'flash init\r\n')
time.sleep(1)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.close()
"@

Write-Host "`n[2] Uploading test file..." -ForegroundColor Yellow
python (Join-Path $PSScriptRoot "..\scripts\upload_file.py") $testFile "/test.txt" "COM10" "flash"

Write-Host "`n[3] Verifying upload..." -ForegroundColor Yellow
python -c @"
import serial, time
s = serial.Serial('COM10', 115200, timeout=2)
time.sleep(0.5)
s.write(b'flash ls\r\n')
time.sleep(0.5)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.write(b'flash cat /test.txt\r\n')
time.sleep(0.5)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.close()
"@

# Cleanup
Remove-Item $testFile -Force -ErrorAction SilentlyContinue

Write-Host "`n=== Test Complete ===" -ForegroundColor Cyan
