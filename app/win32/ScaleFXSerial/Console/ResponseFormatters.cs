using System.Text;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Console;

/// <summary>
/// Formats binary response payloads into human-readable text.
/// Harmonized with Python CLI (tests/cli/parsers/) and Go CLI (app/go/cli/parsers_*.go).
/// See instructions/09-CONSOLE-OUTPUT.md for the canonical output schema.
/// </summary>
public static class ResponseFormatters
{
    // ═══════════════════════════════════════════════════════════════════
    // Core
    // ═══════════════════════════════════════════════════════════════════

    public static void FormatInitReady(Response resp, IConsoleOutput o)
    {
        var info = InitReadyInfo.Parse(resp.Payload);
        if (info == null) { o.WriteWarning("Payload too short"); return; }

        o.WriteData("Device", info.DeviceName);
        o.WriteData("Version", $"{info.Version} (build {info.BuildNumber})");
        o.WriteData("Platform", $"{info.Platform} @ {info.CpuMHz} MHz");
        o.WriteData("Free RAM", $"{info.FreeRam:N0} bytes");
    }

    /// <summary>
    /// Formats STATUS response with 20-byte core header + module-specific dispatch.
    /// </summary>
    public static void FormatStatus(Response resp, IConsoleOutput o, string? controllerType = null)
    {
        var p = resp.Payload;
        if (p.Length < 20) { o.WriteWarning("Status payload too short"); return; }

        uint counter = Endian.ReadU32LE(p, 0);
        uint uptime_ms = Endian.ReadU32LE(p, 4);
        uint freeRam = Endian.ReadU32LE(p, 8);
        uint lastAct = Endian.ReadU32LE(p, 12);
        uint keepalives = Endian.ReadU32LE(p, 16);

        // Uptime formatting
        uint totalSecs = uptime_ms / 1000;
        uint hours = totalSecs / 3600;
        uint minutes = (totalSecs % 3600) / 60;
        uint secs = totalSecs % 60;
        string timeStr = hours > 0
            ? $"{hours}h {minutes}m {secs}s"
            : minutes > 0 ? $"{minutes}m {secs}s" : $"{secs}s";

        o.WriteData("Commands", counter.ToString());
        o.WriteData("Uptime", timeStr);
        o.WriteData("Free RAM", $"{freeRam:N0} bytes ({freeRam / 1024.0:F1} KB)");

        // Last activity formatting
        if (lastAct < 1000)
            o.WriteData("Last seen", $"{lastAct}ms ago  (keepalives: {keepalives})");
        else
            o.WriteData("Last seen", $"{lastAct / 1000.0:F1}s ago  (keepalives: {keepalives})");

        // Module data dispatch
        if (p.Length > 20)
        {
            var moduleData = p[20..];
            switch (controllerType)
            {
                case "gunfx":       FormatGunFxModuleStatus(moduleData, o); break;
                case "lightfx":     FormatLightFxModuleStatus(moduleData, o); break;
                case "gearcontrol": FormatGearControlModuleStatus(moduleData, o); break;
                case "hubfx":       FormatHubFxModuleStatus(moduleData, o); break;
                default:
                    o.WriteInfo("Module data:");
                    o.WriteLine($"  {BitConverter.ToString(moduleData).Replace("-", " ")}");
                    break;
            }
        }
    }

    /// <summary>
    /// Formats I2C_SCAN_RES with the correct structured wire format.
    /// Wire: [numExpected:u8][{addr:u8,found:u8,identified:u8}×N][numExtra:u8][{addr:u8}×M]
    /// </summary>
    public static void FormatI2CScan(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("No I2C scan data"); return; }

        int off = 0;
        byte numExpected = p[off++];
        o.WriteInfo("── I2C Bus Scan ───────────────");
        o.WriteData("Expected devices", numExpected.ToString());

        for (int i = 0; i < numExpected && off + 3 <= p.Length; i++)
        {
            byte addr = p[off++];
            byte found = p[off++];
            byte identified = p[off++];

            if (found != 0 && identified != 0)
                o.WriteSuccess($"  0x{addr:X2}: OK (found + verified)");
            else if (found != 0)
                o.WriteWarning($"  0x{addr:X2}: FOUND (ACK but not verified)");
            else
                o.WriteError($"  0x{addr:X2}: MISSING (no ACK)");
        }

        if (off < p.Length)
        {
            byte numExtra = p[off++];
            if (numExtra > 0)
            {
                var extras = new List<string>();
                for (int i = 0; i < numExtra && off < p.Length; i++)
                    extras.Add($"0x{p[off++]:X2}");
                o.WriteInfo($"Other devices: {string.Join(", ", extras)}");
            }
        }
    }

    /// <summary>
    /// Formats LOG_MESSAGE: [level:u8][millis:u32LE][message:str]
    /// </summary>
    public static void FormatLogMessage(Response resp, IConsoleOutput o)
    {
        if (resp.Payload.Length < 5) return;
        byte level = resp.Payload[0];
        uint millis = Endian.ReadU32LE(resp.Payload, 1);
        string msg = resp.Payload.Length > 5
            ? Encoding.UTF8.GetString(resp.Payload, 5, resp.Payload.Length - 5)
            : "";
        string tag = level switch
        {
            0 => "DEBUG", 1 => "INFO", 2 => "WARN",
            3 => "ERROR", _ => $"L{level}"
        };
        uint secs = millis / 1000;
        uint ms = millis % 1000;
        string line = $"[{secs}.{ms:D3}] {tag,-5} {msg}";
        if (level >= 3) o.WriteError(line);
        else if (level >= 2) o.WriteWarning(line);
        else o.WriteInfo(line);
    }

    // ═══════════════════════════════════════════════════════════════════
    // GunFX Module Status (28 bytes)
    // ═══════════════════════════════════════════════════════════════════

    private static readonly string[] SmokeErrorNames =
        ["NONE", "OVERCURRENT", "UNDERCURRENT", "TIMEOUT", "OVERHEAT"];

    private static string SmokeErrorName(byte code) =>
        code < SmokeErrorNames.Length ? SmokeErrorNames[code] : $"UNKNOWN({code})";

    public static void FormatGunFxModuleStatus(ReadOnlySpan<byte> data, IConsoleOutput o)
    {
        if (data.Length < 28) { o.WriteWarning($"GunFX: incomplete ({data.Length} bytes)"); return; }

        byte flags = data[0];
        bool firing = (flags & 0x01) != 0;
        bool flashActive = (flags & 0x02) != 0;
        bool flashFading = (flags & 0x04) != 0;
        bool heaterOn = (flags & 0x08) != 0;
        bool fanOn = (flags & 0x10) != 0;
        bool fanSpindown = (flags & 0x20) != 0;

        byte fanSpeed = data[1];
        ushort fanOffMs = Endian.ReadU16LE(data, 2);
        ushort servo0 = Endian.ReadU16LE(data, 4);
        ushort servo1 = Endian.ReadU16LE(data, 6);
        ushort servo2 = Endian.ReadU16LE(data, 8);
        ushort rpm = Endian.ReadU16LE(data, 10);
        uint shots = Endian.ReadU32LE(data, 12);
        uint heaterMs = Endian.ReadU32LE(data, 16);
        byte heaterError = data[20];
        byte fanError = data[21];
        byte heaterDuty = data[22];
        byte fanDuty = data[23];
        ushort batteryMV = Endian.ReadU16LE(data, 24);
        byte cellCount = data[26];
        byte batteryPct = data[27];

        o.WriteInfo("── GunFX ──────────────────────");

        // State flags
        var stateFlags = new List<string>();
        if (firing) stateFlags.Add("FIRING");
        if (flashActive) stateFlags.Add("FLASH");
        if (flashFading) stateFlags.Add("FADING");
        if (heaterOn) stateFlags.Add("HEATER");
        if (fanOn) stateFlags.Add("FAN");
        if (fanSpindown) stateFlags.Add("SPINDOWN");
        o.WriteData("State", stateFlags.Count > 0 ? string.Join(", ", stateFlags) : "IDLE");

        if (firing) o.WriteData("Fire rate", $"{rpm} RPM");
        o.WriteData("Shots", shots.ToString());

        if (fanOn || fanSpindown)
            o.WriteData("Fan", $"speed={fanSpeed}, off in {fanOffMs}ms");

        if (heaterMs > 0)
            o.WriteData("Heater", $"{heaterMs / 1000.0:F1}s total");

        o.WriteData("Servos", $"[{servo0}µs, {servo1}µs, {servo2}µs]");

        // Smoke errors
        if (heaterError != 0 || fanError != 0)
        {
            o.WriteInfo("── Smoke Errors ──────────────");
            if (heaterError != 0) o.WriteError($"  Heater: {SmokeErrorName(heaterError)}");
            if (fanError != 0) o.WriteError($"  Fan:    {SmokeErrorName(fanError)}");
        }

        // Overcurrent throttle
        if (heaterDuty < 255 || fanDuty < 255)
        {
            o.WriteInfo("── Overcurrent Throttle ──────");
            if (heaterDuty < 255)
                o.WriteWarning($"  Heater: throttled to {heaterDuty * 100 / 255}% (duty {heaterDuty}/255)");
            if (fanDuty < 255)
                o.WriteWarning($"  Fan:    throttled to {fanDuty * 100 / 255}% (duty {fanDuty}/255)");
        }

        // Battery
        float batV = batteryMV / 1000.0f;
        o.WriteData("Battery", $"{batV:F2}V ({batteryMV}mV), {cellCount}S, {batteryPct}%");
    }

    // ═══════════════════════════════════════════════════════════════════
    // LightFX Module Status (24 bytes)
    // ═══════════════════════════════════════════════════════════════════

    private static readonly string[] LandingLightPhaseNames = ["RET", "DEPLOYING", "DEP", "RETRACTING"];

    private static string LandingLightPhaseName(byte phase) =>
        phase < LandingLightPhaseNames.Length ? LandingLightPhaseNames[phase] : $"?({phase})";

    public static void FormatLightFxModuleStatus(ReadOnlySpan<byte> data, IConsoleOutput o)
    {
        if (data.Length < 15) { o.WriteWarning($"LightFX: incomplete ({data.Length} bytes)"); return; }

        // LEDs
        var ledBrightness = data[..8];
        byte seqFlags = data[8];

        // Servos
        ushort servo0 = Endian.ReadU16LE(data, 9);
        ushort servo1 = Endian.ReadU16LE(data, 11);
        ushort servo2 = Endian.ReadU16LE(data, 13);

        // Optional fields
        byte masterBrightness = data.Length >= 19 ? data[18] : (byte)100;
        byte enabledFlags = data.Length >= 20 ? data[19] : (byte)0xFF;

        o.WriteInfo("── LightFX ────────────────────");

        // LED status (compact)
        var ledParts = new List<string>();
        for (int i = 0; i < 8; i++)
        {
            byte bri = ledBrightness[i];
            bool seq = (seqFlags & (1 << i)) != 0;
            bool enabled = (enabledFlags & (1 << i)) != 0;
            if (!enabled)
                ledParts.Add($"ch{i + 1}={bri}[DIS]");
            else if (bri > 0 || seq)
                ledParts.Add($"ch{i + 1}={bri}{(seq ? "▶" : "")}");
        }
        o.WriteData("LEDs", ledParts.Count > 0 ? string.Join(", ", ledParts) : "all off");

        if (masterBrightness < 100)
            o.WriteData("Master", $"{masterBrightness}%");

        o.WriteData("Servos", $"[{servo0}µs, {servo1}µs, {servo2}µs]");

        // Landing lights
        if (data.Length >= 18)
        {
            var llParts = new List<string>();
            for (int i = 0; i < 3; i++)
                llParts.Add($"slot{i + 1}={LandingLightPhaseName(data[15 + i])}");
            o.WriteData("Lights", string.Join(", ", llParts));
        }

        // Battery
        if (data.Length >= 24)
        {
            ushort batMV = Endian.ReadU16LE(data, 20);
            byte cellCount = data[22];
            byte batPct = data[23];
            float batV = batMV / 1000.0f;
            o.WriteData("Battery", $"{batV:F2}V ({batPct}%, {cellCount}S)");
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // GearControl Module Status (53 bytes)
    // ═══════════════════════════════════════════════════════════════════

    private static readonly string[] GearStateNames =
        ["UNKNOWN", "DEPLOYED", "RETRACTED", "DEPLOYING", "RETRACTING", "ERROR", "CALIBRATING"];
    private static readonly string[] DoorStateNames =
        ["CLOSED", "OPENING", "OPEN", "CLOSING"];
    private static readonly string[] DoorModeNames =
        ["NONE", "FULL", "PARTIAL"];
    private static readonly string[] GearErrorReasonNames =
        ["NONE", "STALL", "TIMEOUT", "OVERCURRENT", "DISABLED"];

    private static string GearStateName(byte state) =>
        state < GearStateNames.Length ? GearStateNames[state] : $"?({state})";
    private static string DoorStateName(byte state) =>
        state < DoorStateNames.Length ? DoorStateNames[state] : $"?({state})";
    private static string DoorModeName(byte mode) =>
        mode < DoorModeNames.Length ? DoorModeNames[mode] : $"?({mode})";
    private static string GearErrorReasonName(byte reason) =>
        reason < GearErrorReasonNames.Length ? GearErrorReasonNames[reason] : $"?({reason})";

    public static void FormatGearControlModuleStatus(ReadOnlySpan<byte> data, IConsoleOutput o)
    {
        if (data.Length < 53) { o.WriteWarning($"GearControl: incomplete ({data.Length} bytes)"); return; }

        string[] gearLabels = ["      Nose", " Left Main", "Right Main"];

        o.WriteInfo("── GearControl ────────────────");

        // Per-gear blocks (11 bytes each at offsets 0, 11, 22)
        for (int g = 0; g < 3; g++)
        {
            int off = g * 11;
            byte state = data[off];
            ushort motorMA = Endian.ReadU16LE(data, off + 1);
            ushort door0 = Endian.ReadU16LE(data, off + 3);
            ushort door1 = Endian.ReadU16LE(data, off + 5);
            ushort stallMA = Endian.ReadU16LE(data, off + 7);
            short shunt10uV = Endian.ReadI16LE(data, off + 9);

            byte errorReason = data[39 + g];
            byte configFlags = data[47 + g];
            byte doorState = data[50 + g];
            byte doorModePacked = data[44 + g];
            byte preDoor = (byte)(doorModePacked & 0x0F);
            byte postDoor = (byte)((doorModePacked >> 4) & 0x0F);
            bool enabled = (configFlags & 0x80) != 0;

            // Gear header line
            string stateStr = GearStateName(state);
            string disabledStr = enabled ? "" : "  [DISABLED]";
            string errorStr = errorReason != 0 ? $"  {GearErrorReasonName(errorReason)}" : "";

            if (state == 5) // ERROR
                o.WriteError($"  {gearLabels[g]}: {stateStr}{disabledStr}{errorStr}");
            else
                o.WriteData(gearLabels[g], $"{stateStr}{disabledStr}{errorStr}");

            // Motor/shunt line
            float shuntMV = shunt10uV * 0.01f;
            o.WriteLine($"             motor={motorMA}mA  shunt={shuntMV:F1}mV  stall={stallMA}mA");

            // Door line
            string doorSt = DoorStateName(doorState);
            bool hasYaw = (configFlags & 0x04) != 0;
            string yawStr = hasYaw ? "  yaw" : "";
            o.WriteLine($"             doors=[{door0}µs, {door1}µs]  {doorSt}  pre={DoorModeName(preDoor)}  post={DoorModeName(postDoor)}{yawStr}");
        }

        // Global section
        o.WriteInfo("── Global ─────────────────────");

        ushort yaw = Endian.ReadU16LE(data, 33);
        byte ledFlags = data[35];
        ushort batteryMV = Endian.ReadU16LE(data, 36);
        byte batteryFlags = data[38];
        ushort shuntResistance = Endian.ReadU16LE(data, 42);

        o.WriteData("Yaw", $"{yaw}µs");

        if (shuntResistance > 0)
        {
            float ohms = shuntResistance / 1000.0f;
            float maxCurrent = 81.92f / ohms;
            o.WriteData("Shunt", $"{shuntResistance}mΩ ({ohms}Ω)  max={maxCurrent:F0}mA");
        }

        // Battery
        bool batEnabled = (batteryFlags & 0x01) != 0;
        bool autoDeploy = (batteryFlags & 0x02) != 0;
        bool lowVoltage = (batteryFlags & 0x04) != 0;
        if (batEnabled)
        {
            float batV = batteryMV / 1000.0f;
            var batParts = new List<string> { $"{batV:F1}V ({batteryMV}mV)" };
            if (autoDeploy) batParts.Add("auto-deploy");
            if (lowVoltage) batParts.Add("LOW VOLTAGE");
            o.WriteData("Battery", string.Join(", ", batParts));
        }
        else
            o.WriteData("Battery", "disabled");

        // LEDs
        var ledParts = new List<string>();
        ledParts.Add((ledFlags & 0x01) != 0 ? "N:dep" : "N:ret");
        ledParts.Add((ledFlags & 0x02) != 0 ? "L:dep" : "L:ret");
        ledParts.Add((ledFlags & 0x04) != 0 ? "R:dep" : "R:ret");
        if ((ledFlags & 0x08) != 0) ledParts.Add("CONN");
        if ((ledFlags & 0x10) != 0) ledParts.Add("err");
        o.WriteData("LEDs", $"[{string.Join(", ", ledParts)}]");
    }

    // ═══════════════════════════════════════════════════════════════════
    // HubFX Module Status (6-19 bytes)
    // ═══════════════════════════════════════════════════════════════════

    public static void FormatHubFxModuleStatus(ReadOnlySpan<byte> data, IConsoleOutput o)
    {
        if (data.Length < 6) { o.WriteWarning($"HubFX: incomplete ({data.Length} bytes)"); return; }

        byte flags = data[0];
        byte slaveMask = data[1];
        uint loop1Count = Endian.ReadU32LE(data, 2);

        bool core1Ready = (flags & 0x01) != 0;
        bool audioInit = (flags & 0x02) != 0;
        bool flashReady = (flags & 0x04) != 0;
        bool usbHostReady = (flags & 0x08) != 0;
        bool sdCardReady = (flags & 0x10) != 0;

        o.WriteInfo("── HubFX Status ──────────────");
        o.WriteData("Core 1", core1Ready ? "Ready" : "NOT READY");
        o.WriteLine($"             {loop1Count} iterations");
        o.WriteData("Audio", audioInit ? "Initialized" : "Not initialized");
        o.WriteData("Flash", flashReady ? "Ready" : "Not available");
        o.WriteData("SD Card", sdCardReady ? "Ready" : "Not available");
        o.WriteData("USB Host", usbHostReady ? "Active" : "Not active");

        bool hasAnySlave = slaveMask != 0;
        if (hasAnySlave)
        {
            o.WriteInfo("  Slaves:");
            o.WriteLine($"    GunFX: {((slaveMask & 0x01) != 0 ? "connected" : "not connected")}");
            o.WriteLine($"    LightFX: {((slaveMask & 0x02) != 0 ? "connected" : "not connected")}");
            o.WriteLine($"    GearControl: {((slaveMask & 0x04) != 0 ? "connected" : "not connected")}");
        }
        else
            o.WriteData("Slaves", "None connected");

        // I2C device status (v2 extended, 13 bytes at offset 6)
        // i2cDeviceMask bits: 0=PCAL6416A@0x20, 1-6=INA226@0x40-0x45, 7=TAS5825M@0x4C
        if (data.Length >= 19)
        {
            byte i2cMask = data[6];
            int detected = 0;
            for (int b = 0; b < 8; b++)
                if ((i2cMask & (1 << b)) != 0) detected++;

            o.WriteInfo($"── I2C Devices ({detected}/8) ──────────");

            // PCAL6416A
            bool pcalOK = (i2cMask & 0x01) != 0;
            o.WriteData("PCAL6416A", $"{(pcalOK ? "OK" : "not found")}  (0x20 GPIO expander)");

            // INA226 monitors with voltage readings
            for (int i = 0; i < 6; i++)
            {
                bool present = (i2cMask & (1 << (i + 1))) != 0;
                ushort voltage_mV = Endian.ReadU16LE(data, 7 + i * 2);
                int addr = 0x40 + i;
                if (present)
                {
                    double voltage_V = voltage_mV / 1000.0;
                    o.WriteData($"INA226[{i}]", $"{voltage_V:F3}V ({voltage_mV} mV)  (0x{addr:X2})");
                }
                else
                    o.WriteData($"INA226[{i}]", $"not found  (0x{addr:X2})");
            }

            // TAS5825M (reserved bit 7)
            if ((i2cMask & 0x80) != 0)
                o.WriteData("TAS5825M", "OK  (0x4C audio codec)");
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // HubFX Query Responses
    // ═══════════════════════════════════════════════════════════════════

    public static void FormatSlaveList(Response resp, IConsoleOutput o)
    {
        var slaves = BoardDetector.ParseSlaveList(resp);
        if (slaves.Count == 0) { o.WriteWarning("No slaves connected"); return; }
        o.WriteInfo($"Connected slaves: {slaves.Count}");
        foreach (var s in slaves)
        {
            string status = s.Ready ? "ready" : s.Connected ? "connected" : "disconnected";
            string display = string.IsNullOrEmpty(s.Name) ? s.TypeName : $"{s.TypeName} ({s.Name})";
            o.WriteLine($"  {display}: {status}");
        }
    }

    /// <summary>
    /// Audio status with full v3/v4 extended format.
    /// Wire: [masterVol:u8][flags:u8][sampleRate:u16LE][bitDepth:u8][maxChannels:u8][codecNameLen:u8][codecName:str]
    ///   Ring stats (if flags bit 3): [ringFillPct:u8][availRead:u16LE][availWrite:u16LE]
    ///       [underruns:u32LE][consumeLoops:u32LE][consumeFrames:u32LE]
    ///   Buffer caps (if flags bit 4): [wavBufCapacity:u16LE][ringCapacity:u16LE]
    ///   Channels: [activeMask:u8] per-active: [ch:u8][vol:u8][playing:u8][looping:u8][loopCount:u16LE]
    ///       [remaining_ms:u32LE][queueLen:u8][output:u8][wavRate:u16LE][wavCh:u8][wavBits:u8]
    ///       [wavBufFillPct:u8 if bit4][fnameLen:u8][fname:str]
    /// </summary>
    public static void FormatAudioStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 7) { o.WriteWarning("Audio status: payload too short"); return; }

        int pos = 0;
        byte masterVol = p[pos++];
        byte flags = p[pos++];
        bool initialized = (flags & 0x01) != 0;
        bool i2sRunning = (flags & 0x02) != 0;
        bool hasCodec = (flags & 0x04) != 0;
        bool hasRingStats = (flags & 0x08) != 0;
        bool hasBufferCaps = (flags & 0x10) != 0;

        ushort sampleRate = Endian.ReadU16LE(p, pos); pos += 2;
        byte bitDepth = p[pos++];
        byte maxChannels = p[pos++];
        byte codecNameLen = p[pos++];
        string codecName = "";
        if (codecNameLen > 0 && pos + codecNameLen <= p.Length)
            codecName = Encoding.UTF8.GetString(p, pos, codecNameLen);
        pos += codecNameLen;

        // Display header
        o.WriteInfo("── Audio Mixer Status ──────────");
        o.WriteData("Initialized", initialized ? "yes" : "no");
        o.WriteData("I2S", $"{(i2sRunning ? "running" : "stopped")} ({sampleRate}Hz / {bitDepth}bit)");
        o.WriteData("Codec", hasCodec ? (codecName.Length > 0 ? codecName : "yes") : "none (I2S only)");
        o.WriteData("Max Channels", maxChannels.ToString());
        o.WriteData("Master Volume", $"{masterVol}%");

        // Ring buffer stats (conditional on flags bit 3)
        byte ringFillPct = 0;
        ushort ringAvailRead = 0, ringAvailWrite = 0;
        uint underruns = 0, consumeLoops = 0, consumeFrames = 0;
        if (hasRingStats && pos + 9 <= p.Length)
        {
            ringFillPct = p[pos++];
            ringAvailRead = Endian.ReadU16LE(p, pos); pos += 2;
            ringAvailWrite = Endian.ReadU16LE(p, pos); pos += 2;
            underruns = Endian.ReadU32LE(p, pos); pos += 4;
            if (pos + 8 <= p.Length)
            {
                consumeLoops = Endian.ReadU32LE(p, pos); pos += 4;
                consumeFrames = Endian.ReadU32LE(p, pos); pos += 4;
            }
        }

        // Buffer capacities (conditional on flags bit 4)
        ushort wavBufCapacity = 0, ringCapacity = 0;
        if (hasBufferCaps && pos + 4 <= p.Length)
        {
            wavBufCapacity = Endian.ReadU16LE(p, pos); pos += 2;
            ringCapacity = Endian.ReadU16LE(p, pos); pos += 2;
        }

        // Display ring buffer stats
        if (hasRingStats)
        {
            ushort ringTotal = (ushort)(ringAvailRead + ringAvailWrite);
            string ringCapStr = ringCapacity > 0 ? $"/{ringCapacity}" : $"/{ringTotal}";
            string ringMs = sampleRate > 0 && ringAvailRead > 0
                ? $" ({(uint)ringAvailRead * 1000 / sampleRate}ms)"
                : "";
            o.WriteData("Ring Buffer", $"{ringFillPct}% ({ringAvailRead}{ringCapStr} frames{ringMs})");
            o.WriteData("Underruns", underruns.ToString());
            o.WriteData("Consumer", $"{consumeLoops} loops, {consumeFrames} frames written to I2S");
        }

        // Display buffer capacities
        if (hasBufferCaps && wavBufCapacity > 0 && sampleRate > 0)
        {
            uint wavMs = (uint)wavBufCapacity * 1000 / sampleRate;
            uint wavKB = (uint)wavBufCapacity * (uint)maxChannels * 2 * 4 / 1024;
            o.WriteData("WAV Buffer", $"{wavBufCapacity} frames/ch ({wavMs}ms, {wavKB}KB total)");
        }

        // Channel data
        if (pos >= p.Length)
        {
            o.WriteWarning("No channel data");
            return;
        }

        byte activeMask = p[pos++];
        if (activeMask == 0)
        {
            o.WriteData("Active", "No active channels");
            return;
        }

        int activeCount = 0;
        for (byte b = activeMask; b != 0; b >>= 1)
            activeCount += b & 1;

        o.WriteData("Active", $"{activeCount} channel(s) (mask: 0b{Convert.ToString(activeMask, 2).PadLeft(8, '0')})");
        o.WriteInfo("── Channels ───────────────────");

        for (int i = 0; i < activeCount && pos + 16 <= p.Length; i++)
        {
            byte ch = p[pos++];
            byte vol = p[pos++];
            bool playing = p[pos++] != 0;
            bool looping = p[pos++] != 0;
            ushort loopCount = Endian.ReadU16LE(p, pos); pos += 2;
            uint remaining_ms = Endian.ReadU32LE(p, pos); pos += 4;
            byte queueLen = p[pos++];
            byte output = p[pos++];

            ushort wavRate = Endian.ReadU16LE(p, pos); pos += 2;
            byte wavCh = p[pos++];
            byte wavBits = p[pos++];

            byte wavBufFill = 0;
            if (hasBufferCaps && pos < p.Length)
                wavBufFill = p[pos++];

            byte fnameLen = pos < p.Length ? p[pos++] : (byte)0;
            string fname = "";
            if (fnameLen > 0 && pos + fnameLen <= p.Length)
                fname = Encoding.UTF8.GetString(p, pos, fnameLen);
            pos += fnameLen;

            // Format channel line
            string status = playing ? "> playing" : "- queued";
            string outName = output switch
            {
                0x01 => "ch1", 0x02 => "ch2", 0x03 => "all",
                _ => $"out{output}"
            };
            string loopStr = looping
                ? (loopCount == 0xFFFF ? " loop=inf" : $" loop=x{loopCount}")
                : "";
            string remainStr = remaining_ms > 0
                ? $" {remaining_ms / 1000}.{remaining_ms % 1000:D3}s left"
                : "";
            string queueStr = queueLen > 0 ? $" [queue: {queueLen}]" : "";
            string bufStr = hasBufferCaps && playing ? $" buf={wavBufFill}%" : "";

            o.WriteLine($"  ch{ch}: {status} vol={vol}% {outName}{loopStr}{remainStr}{queueStr}{bufStr}");

            if (fname.Length > 0)
                o.WriteLine($"        file: {fname}");
            if (wavRate > 0)
            {
                string chStr = wavCh == 2 ? "stereo" : "mono";
                o.WriteLine($"        wav:  {wavRate}Hz/{wavBits}bit/{chStr}");
            }
        }
    }

    /// <summary>
    /// Engine status: [state:u8][toggle:u8][active:u8]
    /// </summary>
    public static void FormatEngineStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("Engine: no data"); return; }

        byte state = p[0];
        string icon = state switch { 0 => "■", 1 => "▶", 2 => "▶", 3 => "⏹", _ => "?" };
        string stateName = state switch
        {
            0 => "Stopped", 1 => "Starting", 2 => "Running",
            3 => "Stopping", _ => $"Unknown({state})"
        };
        o.WriteData("Engine", $"{icon} {stateName}");

        if (p.Length >= 2) o.WriteData("Toggle", p[1] != 0 ? "on" : "off");
        if (p.Length >= 3) o.WriteData("Active", p[2] != 0 ? "yes" : "no");
    }

    public static void FormatSlaveInfo(Response resp, IConsoleOutput o)
    {
        FormatInitReady(resp, o);
    }

    /// <summary>
    /// Config status: [loaded:u8][size:u16LE][valid:u8]
    /// </summary>
    public static void FormatConfigStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("Config status: no data"); return; }

        byte loaded = p[0];
        o.WriteData("Config loaded", loaded != 0 ? "Yes" : "No");

        if (p.Length >= 3)
        {
            ushort size = Endian.ReadU16LE(p, 1);
            o.WriteData("Config size", $"{size} bytes");
        }
        if (p.Length >= 4)
        {
            byte valid = p[3];
            o.WriteData("Config valid", valid != 0 ? "Yes" : "No");
        }
    }

    /// <summary>
    /// SD status: [init:u8][cardSize:u32LE][total:u32LE][free:u32LE][fatType:u8][cardType:u8][busMode:u8][used:u32LE]
    /// </summary>
    public static void FormatSdStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("SD status: no data"); return; }

        byte init = p[0];
        o.WriteData("SD initialized", init != 0 ? "Yes" : "No");

        if (p.Length >= 14)
        {
            uint cardSize = Endian.ReadU32LE(p, 1);
            uint total = Endian.ReadU32LE(p, 5);
            uint free = Endian.ReadU32LE(p, 9);

            byte fatType = p.Length >= 15 ? p[13] : (byte)0;
            byte cardType = p.Length >= 16 ? p[14] : (byte)0;
            byte busMode = p.Length >= 17 ? p[15] : (byte)0;
            uint used = p.Length >= 21 ? Endian.ReadU32LE(p, 17) : total - free;

            string cardTypeName = cardType switch
            {
                1 => "SD", 2 => "SDHC", 3 => "SDXC", _ => "Unknown"
            };
            string busModeName = busMode switch
            {
                0 => "SPI", 1 => "1-bit", 2 => "4-bit", _ => $"?({busMode})"
            };
            string fatName = fatType switch
            {
                12 => "FAT12", 16 => "FAT16", 32 => "FAT32", 64 => "exFAT",
                _ => fatType > 0 ? $"FAT{fatType}" : "?"
            };

            o.WriteData("Card", $"{cardTypeName} ({busModeName})");
            o.WriteData("FAT", fatName);
            o.WriteData("Size", FormatSize(cardSize));
            o.WriteData("Total", FormatSize(total));
            o.WriteData("Used", FormatSize(used));
            o.WriteData("Free", FormatSize(free));
        }
        else if (p.Length >= 9)
        {
            uint total = Endian.ReadU32LE(p, 1);
            uint free = Endian.ReadU32LE(p, 5);
            o.WriteData("Total", FormatSize(total));
            o.WriteData("Free", FormatSize(free));
        }
    }

    /// <summary>
    /// Flash status: [init:u8][total:u32LE][used:u32LE][free:u32LE]
    /// </summary>
    public static void FormatFlashStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("Flash status: no data"); return; }

        byte init = p[0];
        o.WriteData("Flash", init != 0 ? "Ready" : "Not initialized");

        if (p.Length >= 13)
        {
            uint total = Endian.ReadU32LE(p, 1);
            uint used = Endian.ReadU32LE(p, 5);
            uint free = Endian.ReadU32LE(p, 9);
            o.WriteData("Total", FormatSize(total));
            o.WriteData("Used", FormatSize(used));
            o.WriteData("Free", FormatSize(free));
        }
    }

    /// <summary>
    /// File info: [exists:u8][isDir:u8][size:u32LE]
    /// </summary>
    public static void FormatFileInfo(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 6) { o.WriteWarning("File info: too short"); return; }

        byte exists = p[0];
        byte isDir = p[1];
        uint size = Endian.ReadU32LE(p, 2);

        o.WriteData("Exists", exists != 0 ? "Yes" : "No");
        o.WriteData("Type", isDir != 0 ? "Directory" : "File");
        o.WriteData("Size", $"{size:N0} bytes");
    }

    /// <summary>
    /// USB devices: [init:u8][taskRunning:u8][backendLen:u8][backend:str][count:u8]
    /// Per device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]
    /// </summary>
    public static void FormatUsbDevices(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 4) { o.WriteWarning("USB devices: too short"); return; }

        byte init = p[0];
        byte taskRunning = p[1];
        byte backendLen = p[2];

        string backend = backendLen > 0 && 3 + backendLen <= p.Length
            ? Encoding.UTF8.GetString(p, 3, backendLen)
            : "Unknown";

        int off = 3 + backendLen;
        byte count = off < p.Length ? p[off++] : (byte)0;

        o.WriteData("USB Host", init != 0 ? "Initialized" : "Not initialized");
        o.WriteData("Task", taskRunning != 0 ? "Running" : "Stopped");
        o.WriteData("Backend", backend);
        o.WriteData("Devices", count.ToString());

        string[] stateNames = ["ATTACHED", "CONFIGURED", "IDENTIFIED", "READY", "FAILED"];

        for (int i = 0; i < count && off + 7 <= p.Length; i++)
        {
            byte addr = p[off++];
            ushort vid = Endian.ReadU16LE(p, off); off += 2;
            ushort pid = Endian.ReadU16LE(p, off); off += 2;
            byte state = p[off++];
            byte slaveType = p[off++];

            string stateName = state < stateNames.Length ? stateNames[state] : $"?({state})";
            string typeName = PacketTypes.SlaveType.GetName(slaveType);
            o.WriteLine($"  [{addr}] VID={vid:X4} PID={pid:X4} {stateName} → {typeName}");
        }
    }

    /// <summary>
    /// Codec status: [codecType:u8][init:u8][i2cOK:u8][sda:u8][scl:u8][supply:u8][muted:u8]
    ///               [digitalVol:u8][deviceCtrl:u8][fault:u8][nameLen:u8][name:str]
    /// </summary>
    public static void FormatCodecStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 1) { o.WriteWarning("Codec status: no data"); return; }

        o.WriteInfo("── Codec Status ──────────────");

        if (p.Length < 10)
        {
            o.WriteData("Codec initialized", p[0] != 0 ? "Yes" : "No");
            if (p.Length >= 2) o.WriteData("Volume", $"{p[1]}%");
            return;
        }

        byte codecType = p[0];
        byte init = p[1];
        byte i2cOK = p[2];
        byte sda = p[3];
        byte scl = p[4];
        byte supply = p[5];
        byte muted = p[6];
        byte digitalVol = p[7];
        byte deviceCtrl = p[8];
        byte fault = p[9];

        string typeName = codecType switch
        {
            0 => "None", 1 => "TAS5825M", 2 => "SimpleI2S", _ => $"Unknown({codecType})"
        };

        string name = typeName;
        if (p.Length >= 11)
        {
            byte nameLen = p[10];
            if (nameLen > 0 && 11 + nameLen <= p.Length)
                name = Encoding.UTF8.GetString(p, 11, nameLen);
        }

        o.WriteData("Codec", $"{name} ({typeName})");
        o.WriteData("Initialized", init != 0 ? "Yes" : "No");
        o.WriteData("I2C", $"{(i2cOK != 0 ? "OK" : "FAIL")} (SDA={sda}, SCL={scl})");
        o.WriteData("Supply", supply != 0 ? "OK" : "OFF");
        o.WriteData("Muted", muted != 0 ? "Yes" : "No");
        o.WriteData("Volume", $"{digitalVol} (raw)");
        o.WriteData("Device Ctrl", $"0x{deviceCtrl:X2}");
        o.WriteData("Fault", $"0x{fault:X2}");
    }

    // ═══════════════════════════════════════════════════════════════════
    // LightFX Query Responses
    // ═══════════════════════════════════════════════════════════════════

    /// <summary>
    /// LED status: [ch:u8][brightness:u8][seq_playing:u8][seq_count:u8] per channel.
    /// </summary>
    public static void FormatLedStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 4) { o.WriteWarning("(empty LED status)"); return; }

        o.WriteInfo("── LED Channel Status ──");
        for (int i = 0; i + 4 <= p.Length; i += 4)
        {
            byte ch = p[i];
            byte brightness = p[i + 1];
            bool seqPlaying = p[i + 2] != 0;
            byte seqCount = p[i + 3];

            int filled = brightness * 8 / 100;
            if (brightness > 0 && filled == 0) filled = 1;
            string bar = new string('█', filled) + new string('░', 8 - filled);
            string seqIcon = seqPlaying ? "▶" : "■";
            o.WriteLine($"  CH{ch}: {bar} {brightness,3}% | Seq: {seqIcon} ({seqCount} events)");
        }
    }

    /// <summary>
    /// LED seq status: [ch:u8][playing:u8][event_count:u8][current_index:u8][loop_count:u32LE][brightness:u8?]
    /// </summary>
    public static void FormatLedSeqStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 8) { o.WriteWarning("(invalid sequence status)"); return; }

        byte ch = p[0];
        bool playing = p[1] != 0;
        byte count = p[2];
        byte index = p[3];
        uint loops = Endian.ReadU32LE(p, 4);
        byte brightness = p.Length >= 9 ? p[8] : (byte)0;

        string status = playing ? "PLAYING" : "STOPPED";
        o.WriteInfo($"── LED {ch} Sequence Status ──");
        o.WriteData("Status", status);
        o.WriteData("Events", count.ToString());
        o.WriteData("Current", index.ToString());
        o.WriteData("Loop Count", loops.ToString());
        o.WriteData("Brightness", $"{brightness}%");
    }

    /// <summary>
    /// LED seq queue: [ch:u8][count:u8][current_index:u8][playing:u8][brightness:u8]
    /// + per event: [type:u8][duration:u16LE][param1:u8]
    /// </summary>
    public static void FormatLedSeqQueue(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 5) { o.WriteWarning("(invalid sequence queue)"); return; }

        byte ch = p[0];
        byte count = p[1];
        byte curIdx = p[2];
        bool playing = p[3] != 0;
        byte brightness = p[4];

        string status = playing ? "PLAYING" : "STOPPED";
        o.WriteInfo($"── LED {ch} Sequence Queue ({status}, {count} events, brightness {brightness}%) ──");

        if (count == 0) { o.WriteLine("  (empty)"); return; }

        for (int i = 0; i < count; i++)
        {
            int off = 5 + (i * 4);
            if (off + 4 > p.Length) break;
            byte evType = p[off];
            ushort duration = Endian.ReadU16LE(p, off + 1);
            byte param1 = p[off + 3];
            string evName = PacketTypes.LedEvent.GetName(evType);
            string marker = (byte)i == curIdx ? " ← current" : "";
            o.WriteLine($"  [{i}] {evName,-8}: {duration}ms (param={param1}){marker}");
        }
    }

    /// <summary>
    /// Landing light status (async): [slot:u8][phase:u8][finished:u8]
    /// </summary>
    public static void FormatLandingLightStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 3) return;

        byte slot = p[0];
        byte phase = p[1];
        bool finished = p[2] != 0;

        string phaseName = LandingLightPhaseName(phase);

        if (finished)
            o.WriteSuccess($"  ✓ Light {slot}: {phaseName} complete");
        else
            o.WriteInfo($"  ▸ Light {slot}: {phaseName}");
    }

    // ═══════════════════════════════════════════════════════════════════
    // GearControl Async Responses
    // ═══════════════════════════════════════════════════════════════════

    private static string GearIdName(byte id) => id switch
    {
        0 => "Nose", 1 => "Left Main", 2 => "Right Main", _ => $"Gear{id}"
    };

    private static readonly string[] CalibPhaseNames =
        ["IDLE", "CLEAR_RUN", "CLEAR_SETTLE", "DEPLOY_RUN", "MID_SETTLE",
         "RETRACT_RUN", "COMPLETE", "ERROR", "CANCELLED", "OPENING_DOORS", "CLOSING_DOORS"];

    private static string CalibPhaseName(byte phase) =>
        phase < CalibPhaseNames.Length ? CalibPhaseNames[phase] : $"?({phase})";

    private static readonly string[] SeqPhaseNames =
        ["IDLE", "OPEN_DOORS", "WAIT_DOORS", "START_MOTOR", "RUNNING",
         "STOP_MOTOR", "CLOSE_DOORS", "DONE", "ERROR"];

    private static string SeqPhaseName(byte phase) =>
        phase < SeqPhaseNames.Length ? SeqPhaseNames[phase] : $"?({phase})";

    /// <summary>
    /// Gear sequence status (async): [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
    /// </summary>
    public static void FormatGearSeqStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 8) return;

        byte gearId = p[0];
        byte phase = p[1];
        bool deploying = p[2] != 0;
        bool finished = p[3] != 0;
        uint elapsed = Endian.ReadU32LE(p, 4);

        string name = GearIdName(gearId);
        string phaseStr = SeqPhaseName(phase);
        string dir = deploying ? "deploy" : "retract";
        float elapsedSec = elapsed / 1000.0f;

        if (finished)
            o.WriteSuccess($"  ▸ {name} seq: {phaseStr}, {dir}, FINISHED in {elapsedSec:F1}s");
        else if (phase == 8) // ERROR
            o.WriteError($"  ▸ {name} seq: {phaseStr}, {dir}, {elapsedSec:F1}s");
        else
            o.WriteInfo($"  ▸ {name} seq: {phaseStr}, {dir}, {elapsedSec:F1}s");
    }

    /// <summary>
    /// Gear door status (async): [gear_id:u8][state:u8][door0_pos_us:u16LE?][door1_pos_us:u16LE?]
    /// </summary>
    public static void FormatGearDoorStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 2) return;

        byte gearId = p[0];
        byte state = p[1];
        string name = GearIdName(gearId);
        string st = DoorStateName(state);

        if (p.Length >= 6)
        {
            ushort pos0 = Endian.ReadU16LE(p, 2);
            ushort pos1 = Endian.ReadU16LE(p, 4);
            o.WriteInfo($"  ◇ {name} doors: {st}, d0={pos0}µs, d1={pos1}µs");
        }
        else
            o.WriteInfo($"  ◇ {name} doors: {st}");
    }

    /// <summary>
    /// Gear calibration status (async): [gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][stall_mA:u16LE][finished:u8][errorReason:u8?]
    /// </summary>
    public static void FormatGearCalibStatus(Response resp, IConsoleOutput o)
    {
        var p = resp.Payload;
        if (p.Length < 9) return;

        byte gearId = p[0];
        byte phase = p[1];
        ushort currentMA = Endian.ReadU16LE(p, 2);
        ushort peakMA = Endian.ReadU16LE(p, 4);
        ushort stallMA = Endian.ReadU16LE(p, 6);
        bool finished = p[8] != 0;
        byte errorReason = p.Length >= 10 ? p[9] : (byte)0;

        string name = GearIdName(gearId);
        string phaseStr = CalibPhaseName(phase);

        var parts = new List<string> { $"{name} calib: {phaseStr}" };
        parts.Add($"current={currentMA}mA");
        parts.Add($"peak={peakMA}mA");
        parts.Add($"stall={stallMA}mA");
        if (finished) parts.Add("FINISHED");
        if (errorReason != 0) parts.Add($"reason={GearErrorReasonName(errorReason)}");

        if (phase == 7) // ERROR
            o.WriteError($"  ◆ {string.Join(", ", parts)}");
        else if (finished)
            o.WriteSuccess($"  ◆ {string.Join(", ", parts)}");
        else
            o.WriteInfo($"  ◆ {string.Join(", ", parts)}");
    }

    // ═══════════════════════════════════════════════════════════════════
    // Helpers
    // ═══════════════════════════════════════════════════════════════════

    public static void FormatHexDump(Response resp, IConsoleOutput o)
    {
        o.WriteInfo($"{PacketTypes.GetName(resp.PacketType)} [{resp.Payload.Length} bytes]:");
        if (resp.Payload.Length > 0)
            o.WriteLine($"  {BitConverter.ToString(resp.Payload).Replace("-", " ")}");
    }

    /// <summary>
    /// Format a byte size into human-readable string (B, KB, MB, GB).
    /// </summary>
    public static string FormatSize(uint bytes)
    {
        if (bytes < 1024) return $"{bytes} B";
        if (bytes < 1048576) return $"{bytes / 1024.0:F1} KB";
        if (bytes < 1073741824) return $"{bytes / 1048576.0:F1} MB";
        return $"{bytes / 1073741824.0:F2} GB";
    }
}
