namespace ScaleFX.Serial;

/// <summary>
/// Packet type constants for the ScaleFX binary protocol.
/// Mirrors C++ sfx_serial headers and Python packets.py.
/// MUST stay in sync with those files.
/// </summary>
public static class PacketTypes
{
    // ─── Core (0xF0-0xFF) ───

    public static class Core
    {
        public const byte INIT         = 0xF0;
        public const byte SHUTDOWN     = 0xF1;
        public const byte KEEPALIVE    = 0xF2;
        public const byte INIT_READY   = 0xF3;
        public const byte STATUS       = 0xF4;
        public const byte ERROR        = 0xF5;
        public const byte ACK          = 0xF6;
        public const byte NACK         = 0xF7;
        public const byte REBOOT       = 0xF8;
        public const byte BOOTSEL      = 0xF9;
        public const byte STATUS_REQ   = 0xFA;
        public const byte I2C_SCAN     = 0xFB;
        public const byte I2C_SCAN_RES = 0xFC;
        public const byte LOG_MESSAGE  = 0xFD;
        public const byte IDENTIFY     = 0xFE;
        public const byte DIAG_HISTORY = 0xFF;
    }

    // ─── GunFX (0x01-0x2F) ───

    public static class GunFx
    {
        public const byte TRIGGER_ON          = 0x01;
        public const byte TRIGGER_OFF         = 0x02;
        public const byte SERVO_SET           = 0x10;
        public const byte SERVO_SETTINGS      = 0x11;
        public const byte SERVO_RECOIL        = 0x12;
        public const byte SMOKE_HEAT          = 0x20;
        public const byte SMOKE_SETTINGS      = 0x21;
        public const byte SMOKE_RESET         = 0x22;
        public const byte SMOKE_CURRENT_LIMIT = 0x23;
    }

    // ─── LightFX (0x40-0x5F) ───

    public static class LightFx
    {
        public const byte LED_SET              = 0x40;
        public const byte LED_OFF              = 0x41;
        public const byte LED_SEQ_CLEAR        = 0x42;
        public const byte LED_SEQ_ADD          = 0x43;
        public const byte LED_SEQ_START        = 0x44;
        public const byte LED_SEQ_STOP         = 0x45;
        public const byte LED_SEQ_RESTART      = 0x46;
        public const byte LED_SEQ_STATUS       = 0x47;
        public const byte LED_STATUS           = 0x48;
        public const byte LED_SEQ_QUEUE        = 0x49;
        public const byte LED_MASTER_BRIGHTNESS = 0x4A;
        public const byte LED_RESET            = 0x4B;
        public const byte LED_ENABLE           = 0x4C;
        public const byte SERVO_SET            = 0x50;
        public const byte SERVO_SETTINGS       = 0x51;
        public const byte LANDING_LIGHT_BIND    = 0x52;
        public const byte LANDING_LIGHT_UNBIND  = 0x53;
        public const byte LANDING_LIGHT_DEPLOY  = 0x54;
        public const byte LANDING_LIGHT_RETRACT = 0x55;
        public const byte LANDING_LIGHT_STATUS  = 0x56;
        public const byte LED_SEQ_STATUS_RESP   = 0x5A;
        public const byte LED_STATUS_RESP       = 0x5B;
        public const byte LED_SEQ_QUEUE_RESP    = 0x5D;
    }

    // ─── LightFX LED Event Types ───

    public static class LedEvent
    {
        public const byte ON       = 0x00;
        public const byte OFF      = 0x01;
        public const byte FLASH    = 0x02;
        public const byte FADE_IN  = 0x03;
        public const byte FADE_OUT = 0x04;
        public const byte FADING   = 0x05;
        public const byte BEACON   = 0x06;

        public static string GetName(byte eventType) => eventType switch
        {
            ON       => "ON",
            OFF      => "OFF",
            FLASH    => "FLASH",
            FADE_IN  => "FADE_IN",
            FADE_OUT => "FADE_OUT",
            FADING   => "FADING",
            BEACON   => "BEACON",
            _        => $"0x{eventType:X2}"
        };
    }

    // ─── GearControl (0x60-0x7F) ───

    public static class GearControl
    {
        public const byte GEAR_DEPLOY      = 0x60;
        public const byte GEAR_RETRACT     = 0x61;
        public const byte GEAR_STOP        = 0x62;
        public const byte GEAR_ALL         = 0x63;
        public const byte SERVO_SET        = 0x64;
        public const byte SERVO_SETTINGS   = 0x65;
        public const byte GEAR_CONFIG      = 0x66;
        public const byte DOOR_CONFIG      = 0x67;
        public const byte YAW_CONFIG       = 0x68;
        public const byte YAW_INPUT        = 0x69;
        public const byte GEAR_CALIBRATE   = 0x6A;
        public const byte GEAR_CALIB_STATUS = 0x6B;
        public const byte GEAR_CALIB_CANCEL = 0x6C;
        public const byte BATTERY_CONFIG   = 0x6D;
        public const byte DOOR_MODE        = 0x6E;
        public const byte GEAR_RESET       = 0x6F;
        public const byte GEAR_SEQ_STATUS  = 0x70;
        public const byte GEAR_ENABLE      = 0x71;
        public const byte GEAR_DOOR_STATUS = 0x72;
    }

    // ─── HubFX (0x80-0xAF) ───

    public static class HubFx
    {
        public const byte SLAVE_LIST          = 0x80;
        public const byte SLAVE_LIST_RESP     = 0x81;
        public const byte SLAVE_INIT          = 0x82;
        public const byte SLAVE_STATUS        = 0x83;
        public const byte AUDIO_PLAY          = 0x84;
        public const byte AUDIO_STOP          = 0x85;
        public const byte AUDIO_VOLUME        = 0x86;
        public const byte AUDIO_FADE          = 0x87;
        public const byte AUDIO_QUEUE         = 0x88;
        public const byte AUDIO_QUEUE_CLEAR   = 0x89;
        public const byte AUDIO_STATUS_REQ    = 0x8A;
        public const byte AUDIO_STATUS_RESP   = 0x8B;
        public const byte ENGINE_START        = 0x8C;
        public const byte ENGINE_STOP         = 0x8D;
        public const byte ENGINE_STATUS_REQ   = 0x8E;
        public const byte ENGINE_STATUS_RESP  = 0x8F;
        public const byte CONFIG_RELOAD       = 0x90;
        public const byte CONFIG_STATUS       = 0x91;
        public const byte CONFIG_STATUS_RESP  = 0x92;
        public const byte SD_INIT             = 0x93;
        public const byte SD_STATUS_REQ       = 0x94;
        public const byte SD_STATUS_RESP      = 0x95;
        public const byte SLAVE_ROUTE_GUNFX       = 0x96;
        public const byte SLAVE_ROUTE_LIGHTFX     = 0x97;
        public const byte SLAVE_ROUTE_GEARCONTROL = 0x98;
        public const byte FLASH_STATUS_REQ    = 0x99;
        public const byte FILE_LIST           = 0x9A;
        public const byte FILE_DELETE         = 0x9B;
        public const byte FILE_MKDIR          = 0x9C;
        public const byte FILE_INFO           = 0x9D;
        public const byte FILE_INFO_RESP      = 0x9E;
        public const byte FILE_DOWNLOAD       = 0x9F;
        public const byte FILE_UPLOAD_BEGIN   = 0xA0;
        public const byte FILE_UPLOAD_DATA    = 0xA1;
        public const byte FILE_UPLOAD_END     = 0xA2;
        public const byte FILE_UPLOAD_CANCEL  = 0xA3;
        public const byte FILE_TREE           = 0xA9;
        public const byte USB_DEVICES_REQ     = 0xA7;
        public const byte USB_DEVICES_RESP    = 0xA8;
        public const byte CONFIG_SAVE         = 0xAC;
        public const byte USB_RESET_BUS       = 0xAD;
        public const byte CODEC_STATUS_REQ    = 0xAA;
        public const byte CODEC_STATUS_RESP   = 0xAB;
        public const byte SLAVE_INFO          = 0xAE;
        public const byte SLAVE_INFO_RESP     = 0xAF;
        /// <summary>Stream segment ACK: [segment_idx:u16LE][bytes_received:u32LE][ring_fill_pct:u8]</summary>
        public const byte FILE_UPLOAD_PROGRESS = 0xB0;
    }

    // ─── Stream Protocol ───

    public static class Stream
    {
        public const byte BEGIN = 0xA4;
        public const byte DATA  = 0xA5;
        public const byte END   = 0xA6;
    }

    // ─── Audio Constants ───

    public static class Audio
    {
        public const byte OUTPUT_CH1 = 0x01;
        public const byte OUTPUT_CH2 = 0x02;
        public const byte OUTPUT_ALL = 0x03;

        public const byte LOOP_NONE     = 0;
        public const byte LOOP_FINITE   = 1;
        public const byte LOOP_INFINITE = 2;

        public const byte QUEUE_FINISH_LOOP = 0;
        public const byte QUEUE_STOP_NOW    = 1;

        public const byte CH_ALL    = 0xFF;
        public const int  MAX_CHANS = 8;
    }

    // ─── Storage Constants ───

    public static class Storage
    {
        public const byte TARGET_SD    = 0;
        public const byte TARGET_FLASH = 1;

        // Upload modes
        public const byte MODE_SYNC    = 0; // Per-chunk ACK with CRC retry
        public const byte MODE_STREAM  = 3; // Raw binary streaming with segment-based ACKs
    }

    // ─── Slave Types ───

    public static class SlaveType
    {
        public const byte UNKNOWN      = 0;
        public const byte GUNFX        = 1;
        public const byte LIGHTFX      = 2;
        public const byte GEARCONTROL  = 3;

        public static string GetName(byte type) => type switch
        {
            GUNFX       => "GunFX",
            LIGHTFX     => "LightFX",
            GEARCONTROL => "GearControl",
            _           => $"Unknown({type})"
        };
    }

    // ─── Name Lookup ───

    /// <summary>
    /// Returns a human-readable name for a packet type byte.
    /// </summary>
    public static string GetName(byte packetType) => packetType switch
    {
        Core.INIT         => "INIT",
        Core.SHUTDOWN     => "SHUTDOWN",
        Core.KEEPALIVE    => "KEEPALIVE",
        Core.INIT_READY   => "INIT_READY",
        Core.STATUS       => "STATUS",
        Core.ERROR        => "ERROR",
        Core.ACK          => "ACK",
        Core.NACK         => "NACK",
        Core.REBOOT       => "REBOOT",
        Core.BOOTSEL      => "BOOTSEL",
        Core.STATUS_REQ   => "STATUS_REQ",
        Core.I2C_SCAN     => "I2C_SCAN",
        Core.I2C_SCAN_RES => "I2C_SCAN_RES",
        Core.LOG_MESSAGE  => "LOG_MESSAGE",
        Core.IDENTIFY     => "IDENTIFY",
        Core.DIAG_HISTORY => "DIAG_HISTORY",

        GunFx.TRIGGER_ON  => "TRIGGER_ON",
        GunFx.TRIGGER_OFF => "TRIGGER_OFF",
        GunFx.SERVO_SET   => "GFX_SERVO_SET",
        GunFx.SERVO_SETTINGS => "GFX_SERVO_SETTINGS",
        GunFx.SMOKE_HEAT  => "SMOKE_HEAT",

        LightFx.LED_SET   => "LED_SET",
        LightFx.LED_OFF   => "LED_OFF",
        LightFx.LED_SEQ_ADD => "LED_SEQ_ADD",
        LightFx.LED_STATUS_RESP => "LED_STATUS_RESP",

        HubFx.SLAVE_LIST       => "SLAVE_LIST",
        HubFx.SLAVE_LIST_RESP  => "SLAVE_LIST_RESP",
        HubFx.AUDIO_PLAY       => "AUDIO_PLAY",
        HubFx.AUDIO_STOP       => "AUDIO_STOP",
        HubFx.AUDIO_STATUS_RESP => "AUDIO_STATUS_RESP",
        HubFx.ENGINE_START     => "ENGINE_START",
        HubFx.ENGINE_STOP      => "ENGINE_STOP",

        _ => $"0x{packetType:X2}"
    };
}
