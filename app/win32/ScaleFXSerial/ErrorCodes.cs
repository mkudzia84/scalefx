namespace ScaleFX.Serial;

/// <summary>
/// Error codes for the ScaleFX protocol. Mirrors C++ SerialError/XxxError namespaces
/// and Python packets.py error classes. MUST stay in sync.
/// </summary>
public static class ErrorCodes
{
    // ─── Generic / Core (0x00-0x0F, 0x10-0x1F, 0xF0-0xFF) ───

    public const byte OK              = 0x00;
    public const byte UNKNOWN         = 0x01;
    public const byte NOT_INITIALIZED = 0x02;
    public const byte INVALID_COMMAND = 0x03;
    public const byte MISSING_PARAM   = 0x04;
    public const byte BUSY            = 0x05;
    public const byte NOT_SUPPORTED   = 0x06;
    public const byte PERMISSION_DENIED = 0x07;
    public const byte INVALID_PARAM   = 0x10;
    public const byte PARAM_RANGE     = 0x11;
    public const byte INVALID_ID      = 0x12;
    public const byte INVALID_VALUE   = 0x13;
    public const byte PARAM_TOO_LONG  = 0x14;
    public const byte INTERNAL        = 0xF0;
    public const byte TIMEOUT         = 0xF1;
    public const byte COMM_ERROR      = 0xF2;
    public const byte BUFFER_OVERFLOW = 0xF3;
    public const byte CRC_ERROR       = 0xF4;
    public const byte FRAMING_ERROR   = 0xF5;

    // ─── GunFX (0x20-0x4F) ───

    public static class GunFx
    {
        public const byte SERVO_INVALID_ID     = 0x20;
        public const byte SERVO_PULSE_RANGE    = 0x21;
        public const byte SERVO_MIN_MAX        = 0x22;
        public const byte SERVO_NOT_CONFIGURED = 0x23;
        public const byte INVALID_FAN_SPEED   = 0x30;
        public const byte HEATER_DISCONNECTED = 0x31;
        public const byte FAN_DISCONNECTED    = 0x32;
        public const byte HEATER_OVERCURRENT  = 0x33;
        public const byte FAN_OVERCURRENT     = 0x34;
        public const byte INVALID_RPM         = 0x40;
        public const byte ALREADY_FIRING      = 0x41;
        public const byte NOT_FIRING          = 0x42;
    }

    // ─── LightFX (0x50-0x5F) ───

    public static class LightFx
    {
        public const byte INVALID_CHANNEL  = 0x50;
        public const byte SEQ_FULL         = 0x51;
        public const byte INVALID_EVENT    = 0x52;
        public const byte INVALID_PARAM    = 0x53;
        public const byte INVALID_SERVO    = 0x54;
        public const byte INVALID_SLOT     = 0x55;
        public const byte CHANNEL_DISABLED = 0x56;
    }

    // ─── GearControl (0x60-0x6F) ───

    public static class GearControl
    {
        public const byte INVALID_GEAR_ID    = 0x60;
        public const byte INVALID_SERVO_ID   = 0x61;
        public const byte GEAR_BUSY          = 0x62;
        public const byte MOTOR_STALL        = 0x63;
        public const byte MOTOR_TIMEOUT      = 0x64;
        public const byte SERVO_OUT_OF_RANGE = 0x65;
        public const byte INA226_ERROR       = 0x66;
        public const byte YAW_NOT_AVAILABLE  = 0x67;
        public const byte INVALID_ACTION     = 0x68;
        public const byte NO_CURRENT_MONITOR = 0x69;
        public const byte NOT_CALIBRATING    = 0x6A;
        public const byte GEAR_DISABLED      = 0x6B;
    }

    // ─── HubFX (0x80-0x8F) ───

    public static class HubFx
    {
        public const byte SLAVE_NOT_FOUND     = 0x80;
        public const byte SLAVE_NOT_CONNECTED = 0x81;
        public const byte SLAVE_INIT_FAILED   = 0x82;
        public const byte NO_SLAVES           = 0x83;
        public const byte SLAVE_COMM_ERROR    = 0x84;
        public const byte AUDIO_ERROR         = 0x85;
        public const byte SD_NOT_INITIALIZED  = 0x86;
        public const byte ENGINE_NOT_AVAILABLE = 0x87;
        public const byte CONFIG_ERROR        = 0x88;
        public const byte INVALID_CHANNEL     = 0x89;
        public const byte FILE_NOT_FOUND      = 0x8A;
        public const byte FILE_ALREADY_EXISTS = 0x8B;
        public const byte FILE_IO_ERROR       = 0x8C;
        public const byte FILE_TOO_LARGE      = 0x8D;
        public const byte UPLOAD_IN_PROGRESS  = 0x8E;
        public const byte NO_UPLOAD_ACTIVE    = 0x8F;
    }

    /// <summary>
    /// Returns a human-readable name for an error code.
    /// </summary>
    public static string GetName(byte code) => code switch
    {
        OK              => "OK",
        UNKNOWN         => "UNKNOWN",
        NOT_INITIALIZED => "NOT_INITIALIZED",
        INVALID_COMMAND => "INVALID_COMMAND",
        MISSING_PARAM   => "MISSING_PARAMETER",
        BUSY            => "BUSY",
        NOT_SUPPORTED   => "NOT_SUPPORTED",
        PERMISSION_DENIED => "PERMISSION_DENIED",
        INVALID_PARAM   => "INVALID_PARAMETER",
        PARAM_RANGE     => "PARAMETER_OUT_OF_RANGE",
        INVALID_ID      => "INVALID_ID",
        INVALID_VALUE   => "INVALID_VALUE",
        PARAM_TOO_LONG  => "PARAMETER_TOO_LONG",
        INTERNAL        => "INTERNAL_ERROR",
        TIMEOUT         => "TIMEOUT",
        COMM_ERROR      => "COMMUNICATION_ERROR",
        BUFFER_OVERFLOW => "BUFFER_OVERFLOW",
        CRC_ERROR       => "CRC_ERROR",
        FRAMING_ERROR   => "FRAMING_ERROR",

        // GunFX
        GunFx.SERVO_INVALID_ID     => "GFX_SERVO_INVALID_ID",
        GunFx.SERVO_PULSE_RANGE    => "GFX_SERVO_PULSE_RANGE",
        GunFx.SERVO_MIN_MAX        => "GFX_SERVO_MIN_MAX",
        GunFx.SERVO_NOT_CONFIGURED => "GFX_SERVO_NOT_CONFIGURED",
        GunFx.INVALID_FAN_SPEED    => "GFX_INVALID_FAN_SPEED",
        GunFx.HEATER_DISCONNECTED  => "GFX_HEATER_DISCONNECTED",
        GunFx.FAN_DISCONNECTED     => "GFX_FAN_DISCONNECTED",
        GunFx.HEATER_OVERCURRENT   => "GFX_HEATER_OVERCURRENT",
        GunFx.FAN_OVERCURRENT      => "GFX_FAN_OVERCURRENT",
        GunFx.INVALID_RPM          => "GFX_INVALID_RPM",
        GunFx.ALREADY_FIRING       => "GFX_ALREADY_FIRING",
        GunFx.NOT_FIRING           => "GFX_NOT_FIRING",

        // LightFX
        LightFx.INVALID_CHANNEL    => "LFX_INVALID_CHANNEL",
        LightFx.SEQ_FULL           => "LFX_SEQUENCE_FULL",
        LightFx.INVALID_EVENT      => "LFX_INVALID_EVENT",
        LightFx.INVALID_PARAM      => "LFX_INVALID_PARAM",
        LightFx.INVALID_SERVO      => "LFX_INVALID_SERVO",
        LightFx.INVALID_SLOT       => "LFX_INVALID_SLOT",
        LightFx.CHANNEL_DISABLED   => "LFX_CHANNEL_DISABLED",

        // GearControl
        GearControl.INVALID_GEAR_ID    => "GC_INVALID_GEAR_ID",
        GearControl.INVALID_SERVO_ID   => "GC_INVALID_SERVO_ID",
        GearControl.GEAR_BUSY          => "GC_GEAR_BUSY",
        GearControl.MOTOR_STALL        => "GC_MOTOR_STALL",
        GearControl.MOTOR_TIMEOUT      => "GC_MOTOR_TIMEOUT",
        GearControl.SERVO_OUT_OF_RANGE => "GC_SERVO_OUT_OF_RANGE",
        GearControl.INA226_ERROR       => "GC_INA226_ERROR",
        GearControl.YAW_NOT_AVAILABLE  => "GC_YAW_NOT_AVAILABLE",
        GearControl.INVALID_ACTION     => "GC_INVALID_ACTION",
        GearControl.NO_CURRENT_MONITOR => "GC_NO_CURRENT_MONITOR",
        GearControl.NOT_CALIBRATING    => "GC_NOT_CALIBRATING",
        GearControl.GEAR_DISABLED      => "GC_GEAR_DISABLED",

        // HubFX
        HubFx.SLAVE_NOT_FOUND      => "HUB_SLAVE_NOT_FOUND",
        HubFx.SLAVE_NOT_CONNECTED  => "HUB_SLAVE_NOT_CONNECTED",
        HubFx.SLAVE_INIT_FAILED    => "HUB_SLAVE_INIT_FAILED",
        HubFx.NO_SLAVES            => "HUB_NO_SLAVES",
        HubFx.SLAVE_COMM_ERROR     => "HUB_SLAVE_COMM_ERROR",
        HubFx.AUDIO_ERROR          => "HUB_AUDIO_ERROR",
        HubFx.SD_NOT_INITIALIZED   => "HUB_SD_NOT_INITIALIZED",
        HubFx.ENGINE_NOT_AVAILABLE => "HUB_ENGINE_NOT_AVAILABLE",
        HubFx.CONFIG_ERROR         => "HUB_CONFIG_ERROR",
        HubFx.INVALID_CHANNEL      => "HUB_INVALID_CHANNEL",
        HubFx.FILE_NOT_FOUND       => "HUB_FILE_NOT_FOUND",
        HubFx.FILE_ALREADY_EXISTS  => "HUB_FILE_ALREADY_EXISTS",
        HubFx.FILE_IO_ERROR        => "HUB_FILE_IO_ERROR",
        HubFx.FILE_TOO_LARGE       => "HUB_FILE_TOO_LARGE",
        HubFx.UPLOAD_IN_PROGRESS   => "HUB_UPLOAD_IN_PROGRESS",
        HubFx.NO_UPLOAD_ACTIVE     => "HUB_NO_UPLOAD_ACTIVE",

        _ => $"ERROR_0x{code:X2}"
    };
}
