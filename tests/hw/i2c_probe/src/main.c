/*
 * HubFX I2C probe — minimal "noop" bring-up firmware.
 *
 * Inits the I2C bus (SDA=GPIO8, SCL=GPIO9 — same on every HubFX rev), scans
 * 0x08..0x77, and positively identifies each device:
 *   - INA226  : reads Manufacturer-ID (0xFE == 0x5449 'TI') + Die-ID
 *               (0xFF == 0x2260) — the SAME canonical check the firmware's
 *               INA226::begin() uses, so counterfeits are flagged, not trusted.
 *   - PCA9685 : 0x70 (8-ch PWM driver / all-call)
 *   - TAS5825P: 0x4C (audio codec)
 *
 * Prints the table over UART0 (GPIO43/44 -> CH343 -> USB0) at 115200, every 3 s.
 * Does NOT bring up PCA/codec/USB-host/audio — safe on unproven hardware.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

#define SDA_GPIO   8
#define SCL_GPIO   9
#define I2C_PORT   I2C_NUM_0
#define I2C_HZ     400000

static esp_err_t rd16(uint8_t addr, uint8_t reg, uint16_t *out)
{
    uint8_t d[2] = {0, 0};
    esp_err_t e = i2c_master_write_read_device(
        I2C_PORT, addr, &reg, 1, d, 2, pdMS_TO_TICKS(50));
    if (e == ESP_OK && out) *out = ((uint16_t)d[0] << 8) | d[1];
    return e;
}

/* Read a register N times; returns true if EVERY read succeeded AND all
 * returned the SAME value (out = that value).  Two chips answering at one
 * address drive SDA simultaneously -> the wire-AND is often UNSTABLE across
 * reads, which is our only real signal of an address CLASH on I2C (you can't
 * address the two chips separately, but you can catch them fighting). */
static bool rd16_stable(uint8_t addr, uint8_t reg, uint16_t *out)
{
    uint16_t first = 0;
    if (rd16(addr, reg, &first) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) {
        uint16_t v = 0;
        if (rd16(addr, reg, &v) != ESP_OK) return false;
        if (v != first) return false;   /* contention -> unstable */
    }
    if (out) *out = first;
    return true;
}

static bool is_canonical_ina(uint8_t a)
{
    uint16_t mfg = 0, die = 0;
    return rd16_stable(a, 0xFE, &mfg) && mfg == 0x5449 &&
           rd16_stable(a, 0xFF, &die) && (die & 0xFFF0) == 0x2260;
}

/* Fills `out` with a human label; sets *clash if the address shows the
 * unstable-read signature of two chips fighting. */
static void identify(uint8_t a, char *out, size_t outLen, bool *clash)
{
    *clash = false;
    uint16_t mfg = 0, die = 0;
    bool mfgStable = rd16_stable(a, 0xFE, &mfg);
    bool dieStable = rd16_stable(a, 0xFF, &die);

    if (mfgStable && mfg == 0x5449 && dieStable && (die & 0xFFF0) == 0x2260) {
        snprintf(out, outLen, "INA226   (canonical mfg=0x5449 die=0x2260)");
        return;
    }
    if (a == 0x70) { snprintf(out, outLen, "PCA9685  (8-ch PWM / all-call)"); return; }
    if (a == 0x4C) { snprintf(out, outLen, "TAS5825P (audio codec)"); return; }

    if (a >= 0x40 && a <= 0x4F) {
        /* In the INA/codec range but not a clean INA.  If the ID regs read
         * UNSTABLE, two devices are fighting here (e.g. the PCA9685's HW
         * address colliding with an INA226 that hasn't been re-strapped). */
        if (!mfgStable || !dieStable) {
            *clash = true;
            snprintf(out, outLen,
                     "** CLASH? unstable ID reads (mfg last=0x%04X die last=0x%04X) "
                     "— two chips answering at 0x%02X", mfg, die, a);
        } else {
            snprintf(out, outLen,
                     "single non-INA chip (stable mfg=0x%04X) — e.g. the PCA9685 "
                     "hardware address", mfg);
        }
        return;
    }
    snprintf(out, outLen, "?? (mfg reg=0x%04X)", mfg);
}

static bool present(uint8_t a)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e == ESP_OK;
}

void app_main(void)
{
    const i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = SDA_GPIO,
        .scl_io_num       = SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_HZ,
    };
    i2c_param_config(I2C_PORT, &cfg);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);

    for (;;) {
        printf("\n=== ScaleFX HubFX  I2C probe  ·  SDA=GPIO%d  SCL=GPIO%d  @ %d Hz ===\n",
               SDA_GPIO, SCL_GPIO, I2C_HZ);
        int n = 0, inas = 0, clashes = 0;
        char label[96];
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (!present(a)) continue;
            bool clash = false;
            identify(a, label, sizeof(label), &clash);
            printf("  0x%02X   %s\n", a, label);
            n++;
            if (clash) clashes++;
            if (is_canonical_ina(a)) inas++;
        }
        printf("=== %d device(s), %d canonical INA226, %d suspected clash%s ===\n",
               n, inas, clashes, clashes == 1 ? "" : "es");
        if (clashes == 0)
            printf("=== NO address clashes detected — every occupied address answers cleanly ===\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
