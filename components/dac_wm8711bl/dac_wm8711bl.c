/**
 * Implementation of control interface to Wolfson WM8711BL DAC
 *
 * The WM8711BL is a low-power stereo DAC with integrated headphone driver,
 * controlled via a 2-wire (I2C) serial interface with 7-bit register address
 * and 9-bit data words.
 *
 * WM8711BL datasheet:
 * https://www.cirrus.com/products/wm8711/
 */

#include "dac_wm8711bl.h"

#include <math.h>
#include <string.h>
#include <sys/param.h>

#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* I2C address depends on CSB pin level */
#define WM8711_ADDR_CSB_LOW  0x1A
#define WM8711_ADDR_CSB_HIGH 0x1B

#define I2C_TIMEOUT    100
#define I2C_LINE_SPEED 100000

static const char TAG[] = "WM8711BL DAC";

/* ---- Register addresses (7-bit) ---- */
#define WM8711_REG_LHPOUT   0x02 /* Left Headphone Out  */
#define WM8711_REG_RHPOUT   0x03 /* Right Headphone Out */
#define WM8711_REG_APATH    0x04 /* Analogue Audio Path Control */
#define WM8711_REG_DPATH    0x05 /* Digital Audio Path Control  */
#define WM8711_REG_POWER    0x06 /* Power Down Control          */
#define WM8711_REG_IFACE    0x07 /* Digital Audio Interface Fmt  */
#define WM8711_REG_SAMPLING 0x08 /* Sampling Control            */
#define WM8711_REG_ACTIVE   0x09 /* Active Control              */
#define WM8711_REG_RESET    0x0F /* Reset (write any value)     */

/* ---- R2 / R3  Headphone Out ---- */
#define WM8711_HPOUT_BOTH (1 << 8) /* Update both L+R simultaneously */
#define WM8711_HPOUT_ZCEN (1 << 7) /* Zero-cross detect enable      */

/* ---- R4  Analogue Audio Path ---- */
#define WM8711_APATH_DACSEL (1 << 4) /* Route DAC to output mixer */

/* ---- R5  Digital Audio Path ---- */
#define WM8711_DPATH_DACMU (1 << 3) /* DAC soft mute */

/* ---- R6  Power Down Control ---- */
#define WM8711_PWR_POWEROFF (1 << 7)
#define WM8711_PWR_CLKOUTPD (1 << 6)
#define WM8711_PWR_OSCPD    (1 << 5)
#define WM8711_PWR_OUTPD    (1 << 4)
#define WM8711_PWR_DACPD    (1 << 3)
#define WM8711_PWR_LINEINPD (1 << 0)

/* ---- R7  Digital Audio Interface Format ---- */
#define WM8711_IFACE_FMT_I2S (0x02)      /* I2S format        */
#define WM8711_IFACE_IWL_16  (0x00 << 2) /* 16-bit word len   */
#define WM8711_IFACE_IWL_20  (0x01 << 2) /* 20-bit word len   */
#define WM8711_IFACE_IWL_24  (0x02 << 2) /* 24-bit word len   */
#define WM8711_IFACE_IWL_32  (0x03 << 2) /* 32-bit word len   */

/* ---- R8  Sampling Control ---- */
#define WM8711_SAMP_USB     (1 << 0)
#define WM8711_SAMP_BOSR    (1 << 1)
#define WM8711_SAMP_SR_48K  (0x00 << 2) /* 48 kHz, normal, 256fs */
#define WM8711_SAMP_SR_441K (0x08 << 2) /* 44.1 kHz, normal, 256fs */

/* ---- R9  Active Control ---- */
#define WM8711_ACTIVE (1 << 0)

/* ---- Headphone volume register constants ---- */
#define WM8711_HP_VOL_MUTE 0x00 /* 0x00–0x2F all mute  */
#define WM8711_HP_VOL_MIN  0x30 /* -73 dB              */
#define WM8711_HP_VOL_0DB  0x79 /*   0 dB              */
#define WM8711_HP_VOL_MAX  0x7F /*  +6 dB              */

/* Power-down value: unused blocks off, DAC + output on.
 * CLKOUTPD | OSCPD | LINEINPD – ESP32 supplies MCLK externally. */
#define WM8711_PWR_RUNNING \
  (WM8711_PWR_CLKOUTPD | WM8711_PWR_OSCPD | WM8711_PWR_LINEINPD)

/* ------------------------------------------------------------------ */
/*  Private state                                                      */
/* ------------------------------------------------------------------ */

static uint8_t wm8711_addr;
static i2c_master_bus_handle_t s_bus_handle;
static i2c_master_dev_handle_t wm8711_dev;

/* Forward declarations */
static esp_err_t wm8711_write_reg(uint8_t reg, uint16_t data);
static esp_err_t i2c_bus_add_device(uint8_t addr,
                                    i2c_master_dev_handle_t *handle);

/* ------------------------------------------------------------------ */
/*  DAC ops implementation                                             */
/* ------------------------------------------------------------------ */

static esp_err_t wm8711_init(void *i2c_bus) {
  esp_err_t err;

  s_bus_handle = (i2c_master_bus_handle_t)i2c_bus;
  if (s_bus_handle == NULL) {
    ESP_LOGE(TAG, "No I2C bus handle provided");
    return ESP_ERR_INVALID_ARG;
  }

  /* Detect the WM8711BL by attempting a reset write at each candidate addr.
   * Using i2c_master_probe() can confuse the chip because the WM8711's I2C
   * state machine expects 2 data bytes after every address; the probe's
   * address-only transaction leaves it in an undefined state. */
  const uint8_t addrs[] = {WM8711_ADDR_CSB_LOW, WM8711_ADDR_CSB_HIGH};
  wm8711_addr = 0;

  for (int i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
    i2c_master_dev_handle_t try_dev = NULL;
    err = i2c_bus_add_device(addrs[i], &try_dev);
    if (err != ESP_OK)
      continue;

    /* Attempt the reset register write – this is the detection */
    uint8_t buf[2];
    buf[0] = (WM8711_REG_RESET << 1) | 0;
    buf[1] = 0x00;
    err = i2c_master_transmit(try_dev, buf, 2, I2C_TIMEOUT);
    if (err == ESP_OK) {
      wm8711_addr = addrs[i];
      ESP_LOGI(TAG, "Detected WM8711BL at @0x%02X (reset OK)", wm8711_addr);

      /* The software reset can glitch the bus.  Drop the device handle,
       * reset the I2C master to recover, wait for the chip to settle,
       * then re-add the device so the driver starts from a clean state.
       * Note: bus_reset may warn about strapping pins — this is harmless. */
      i2c_master_bus_rm_device(try_dev);
      i2c_master_bus_reset(s_bus_handle);
      vTaskDelay(pdMS_TO_TICKS(20));

      err = i2c_bus_add_device(wm8711_addr, &wm8711_dev);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Re-add device failed: %s", esp_err_to_name(err));
        return err;
      }
      break;
    }
    /* Not at this address – remove device and try next */
    i2c_master_bus_rm_device(try_dev);
  }

  if (!wm8711_addr) {
    ESP_LOGW(TAG, "No WM8711BL detected on I2C bus");
    return ESP_ERR_NOT_FOUND;
  }

  /* 2. Power-down control – power up DAC + headphone output,
   *    keep internal oscillator and line-in powered down */
  err = wm8711_write_reg(WM8711_REG_POWER, WM8711_PWR_RUNNING);
  if (err != ESP_OK) return err;

  /* 3. Analogue audio path – route DAC to output mixer */
  err = wm8711_write_reg(WM8711_REG_APATH, WM8711_APATH_DACSEL);
  if (err != ESP_OK) return err;

  /* 4. Digital audio path – unmute DAC, no de-emphasis */
  err = wm8711_write_reg(WM8711_REG_DPATH, 0x00);
  if (err != ESP_OK) return err;

  /* 5. Digital audio interface – I2S, 16-bit, slave mode */
  err = wm8711_write_reg(WM8711_REG_IFACE,
                          WM8711_IFACE_FMT_I2S | WM8711_IFACE_IWL_16);
  if (err != ESP_OK) return err;

  /* 6. Sampling control – normal mode, 256 fs, 44.1 kHz */
  err = wm8711_write_reg(WM8711_REG_SAMPLING, WM8711_SAMP_SR_441K);
  if (err != ESP_OK) return err;

  /* 7. Headphone volume – 0 dB, zero-cross enabled, both channels */
  err = wm8711_write_reg(WM8711_REG_LHPOUT,
                          WM8711_HP_VOL_0DB | WM8711_HPOUT_ZCEN |
                              WM8711_HPOUT_BOTH);
  if (err != ESP_OK) return err;

  /* 8. Activate the digital audio interface */
  err = wm8711_write_reg(WM8711_REG_ACTIVE, WM8711_ACTIVE);
  if (err != ESP_OK) return err;

  ESP_LOGI(TAG, "WM8711BL initialised at I2C 0x%02X", wm8711_addr);
  return ESP_OK;
}

static esp_err_t wm8711_deinit(void) {
  esp_err_t err = ESP_OK;

  /* Deactivate interface, then power off */
  wm8711_write_reg(WM8711_REG_ACTIVE, 0x00);
  wm8711_write_reg(WM8711_REG_POWER, WM8711_PWR_POWEROFF);

  if (wm8711_dev) {
    err = i2c_master_bus_rm_device(wm8711_dev);
    wm8711_dev = NULL;
  }

  s_bus_handle = NULL;
  return err;
}

static void wm8711_set_power_mode(dac_power_mode_t mode) {
  switch (mode) {
  case DAC_POWER_ON:
    wm8711_write_reg(WM8711_REG_POWER, WM8711_PWR_RUNNING);
    wm8711_write_reg(WM8711_REG_ACTIVE, WM8711_ACTIVE);
    break;
  case DAC_POWER_STANDBY:
    /* Keep DAC clocked but power down the output stage */
    wm8711_write_reg(WM8711_REG_ACTIVE, 0x00);
    wm8711_write_reg(WM8711_REG_POWER,
                     WM8711_PWR_RUNNING | WM8711_PWR_OUTPD);
    break;
  case DAC_POWER_OFF:
    wm8711_write_reg(WM8711_REG_ACTIVE, 0x00);
    wm8711_write_reg(WM8711_REG_POWER, WM8711_PWR_POWEROFF);
    break;
  default:
    ESP_LOGW(TAG, "Unhandled power mode %d", mode);
    break;
  }
}

static void wm8711_enable_speaker(bool enable) {
  /* The WM8711BL only has a headphone/line output – use DAC soft mute */
  if (enable) {
    wm8711_write_reg(WM8711_REG_DPATH, 0x00);
  } else {
    wm8711_write_reg(WM8711_REG_DPATH, WM8711_DPATH_DACMU);
  }
}

static void wm8711_enable_line_out(bool enable) {
  /* Headphone output doubles as line out on WM8711BL */
  wm8711_enable_speaker(enable);
}

/**
 * Map AirPlay volume (-30 … 0 dB) to the WM8711BL headphone register.
 *
 * Headphone register encoding (1 dB steps):
 *   0x00–0x2F  mute
 *   0x30       -73 dB
 *   …
 *   0x79         0 dB
 *   0x7A–0x7F   +1 … +6 dB
 *
 * CONFIG_DAC_WM8711BL_MAX_VOLUME (dB) sets the DAC level that corresponds
 * to AirPlay 0 dB.  Default 0 means the DAC runs at its own 0 dB reference
 * when AirPlay is at full volume.
 */
static void wm8711_set_volume(float volume_airplay_db) {
  /* Clamp AirPlay input range */
  if (volume_airplay_db > 0.0f)
    volume_airplay_db = 0.0f;
  if (volume_airplay_db < -30.0f)
    volume_airplay_db = -30.0f;

  float max_db = (float)CONFIG_DAC_WM8711BL_MAX_VOLUME;

  /* 1:1 dB mapping – AirPlay 0 dB → max_db on the DAC */
  float dac_db = max_db + volume_airplay_db;

  uint8_t reg_val;

  if (dac_db < -73.0f) {
    reg_val = WM8711_HP_VOL_MUTE;
  } else {
    int r = WM8711_HP_VOL_0DB + (int)dac_db; /* 0x79 + dB offset */
    if (r < WM8711_HP_VOL_MIN)
      reg_val = WM8711_HP_VOL_MUTE;
    else if (r > WM8711_HP_VOL_MAX)
      reg_val = WM8711_HP_VOL_MAX;
    else
      reg_val = (uint8_t)r;
  }

  ESP_LOGD(TAG, "Volume: AirPlay %.1f dB -> DAC %.1f dB -> reg 0x%02X",
           volume_airplay_db, dac_db, reg_val);

  /* Write both channels, zero-cross enabled */
  uint16_t val = reg_val | WM8711_HPOUT_ZCEN | WM8711_HPOUT_BOTH;
  wm8711_write_reg(WM8711_REG_LHPOUT, val);
}

/* ------------------------------------------------------------------ */
/*  Public ops table                                                   */
/* ------------------------------------------------------------------ */

const dac_ops_t dac_wm8711bl_ops = {
    .init = wm8711_init,
    .deinit = wm8711_deinit,
    .set_volume = wm8711_set_volume,
    .set_power_mode = wm8711_set_power_mode,
    .enable_speaker = wm8711_enable_speaker,
    .enable_line_out = wm8711_enable_line_out,
};

/* ------------------------------------------------------------------ */
/*  I2C register write                                                 */
/* ------------------------------------------------------------------ */

/**
 * Write a 9-bit value to a WM8711BL register.
 *
 * The chip uses a non-standard I2C framing:
 *   [slave addr + W]  [reg_addr(7) | data_bit8(1)]  [data_bits7:0]
 */
static esp_err_t wm8711_write_reg(uint8_t reg, uint16_t data) {
  if (!wm8711_dev) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t buf[2];
  buf[0] = (reg << 1) | ((data >> 8) & 0x01);
  buf[1] = data & 0xFF;

  esp_err_t err = i2c_master_transmit(wm8711_dev, buf, 2, I2C_TIMEOUT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C write reg 0x%02X failed: %s", reg,
             esp_err_to_name(err));
  }
  return err;
}

/* ------------------------------------------------------------------ */
/*  I2C bus helpers                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t i2c_bus_add_device(uint8_t addr,
                                    i2c_master_dev_handle_t *handle) {
  if (!s_bus_handle) return ESP_ERR_INVALID_STATE;

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = addr,
      .scl_speed_hz = I2C_LINE_SPEED,
  };
  return i2c_master_bus_add_device(s_bus_handle, &dev_cfg, handle);
}
