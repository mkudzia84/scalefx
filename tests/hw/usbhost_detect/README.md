# usbhost_detect — minimal ESP32-S3 USB-host device probe

A "dumb" firmware that brings up the USB-OTG **host** and printf's every device
that connects (`>>> MOUNT addr=.. VID=.. PID=..`) to a **UART0 console** at
115200 (CH343 → COMxx, plain text — NOT the COBS wire). Use it to isolate
"the HubFX won't enumerate a plugged-in expander" down to hardware vs firmware.

Key: the console is on **UART** (`CONFIG_ESP_CONSOLE_NONE`/UART, USB-Serial-JTAG
OFF) so it does NOT contend with the USB-OTG internal PHY (GPIO19/20). If this
detects a device but the hub doesn't, the hub's console/PHY config or its
`usb.begin()`+`usb.init()` wiring is the culprit (see the CLAUDE.md USB-host
gotcha — both were bugs, fixed 2026-06-07).

Build + flash + watch:
```
cd tests/hw/usbhost_detect && pio run -e esp32s3 -t upload --upload-port COMxx
# then read COMxx at 115200 (any serial monitor) and plug a USB CDC device in
```
