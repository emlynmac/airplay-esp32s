/**
 * @file dac_pcm512x.c
 * @brief TI PCM5121 / PCM5122 line-out DAC driver
 *
 * Datasheet SLAS763C — https://www.ti.com/lit/ds/symlink/pcm5122.pdf
 *
 * The TAS575x driver in this tree talks to what is essentially the same part
 * with a class-D output stage attached, so page 0 — reset, power, mute,
 * clocking, volume and status — is register-for-register identical and the
 * bring-up here is deliberately a trimmed copy of it.
 *
 * Two things are different and shape this driver:
 *
 *  - There is no user miniDSP. P0-R43 only accepts the ROM process flows
 *    (1, 2, 3, 5, 7); the "program in RAM" setting the TAS575x HybridFlow code
 *    relies on is marked reserved. Nothing therefore has to be re-downloaded
 *    after a powerdown, which makes DAC_POWER_OFF cheap and removes the whole
 *    flow-resident state machine.
 *
 *  - Page 1 drives a 2.1 Vrms line output rather than an amplifier. There is
 *    no speaker to gate, no fault line and no thermal shutdown, so
 *    enable_speaker() maps onto the digital mute.
 *
 * A PCM512x answers on 0x4C-0x4F, exactly like a TAS575x, and neither part has
 * a device ID register. Which driver runs is therefore a build-time choice
 * (CONFIG_DAC_PCM512X vs CONFIG_DAC_TAS57XX); all this driver can do is read a
 * register whose reset value differs and warn when it looks wrong.
 */

#include "dac_pcm512x.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "board_utils.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcm512x_regs.h"
#include "sdkconfig.h"

static const char TAG[] = "PCM512X";

#define PCM512X_I2C_SPEED_HZ 100000

/** Candidate 7-bit addresses, set by the ADR1/ADR2 straps. */
static const uint8_t s_addr_candidates[] = {0x4C, 0x4D, 0x4E, 0x4F};

/** Charge pump ramp plus PLL lock after leaving standby. */
#define PCM512X_POWER_UP_SETTLE_MS 50

/** The part needs a moment in standby before the PLL reference may change. */
#define PCM512X_STANDBY_SETTLE_MS 10

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_addr = 0;
static SemaphoreHandle_t s_mutex = NULL;

/** Requested power mode. ON by default so a board that never drives the power
 *  hooks still plays once the I2S clocks come up. */
static dac_power_mode_t s_power_mode = DAC_POWER_ON;
static bool s_output_enabled = true;
static bool s_i2s_running = false;
static uint32_t s_i2s_rate_hz = 0;
static bool s_pll_ref_bck = false;
static float s_volume_db = PCM512X_VOL_MIN_DB;

/* ========================= register helpers ========================= */

static esp_err_t pcm512x_write(uint8_t reg, uint8_t value) {
  return board_i2c_write(s_dev, reg, &value, 1);
}

static esp_err_t pcm512x_read(uint8_t reg, uint8_t *value) {
  return board_i2c_read(s_dev, reg, value, 1);
}

static esp_err_t pcm512x_page(uint8_t page) {
  return pcm512x_write(PCM512X_REG_PAGE, page);
}

static const char *pcm512x_power_state_str(uint8_t state) {
  switch (state) {
  case 0x0:
    return "powerdown";
  case 0x1:
    return "wait for CP voltage";
  case 0x2:
  case 0x3:
    return "calibration";
  case 0x4:
    return "volume ramp up";
  case 0x5:
    return "run";
  case 0x6:
    return "line output short";
  case 0x7:
    return "volume ramp down";
  case 0x8:
    return "standby";
  default:
    return "unknown";
  }
}

/* ============================== volume ============================== */

/**
 * Map the AirPlay -30..0 dB range onto the part's attenuator.
 *
 * Same 2:1 taper as the TAS57xx driver: the top 25 dB of the control gives
 * 50 dB of real attenuation, and the last 5 dB roll off steeply to silence so
 * the bottom of the slider is actually quiet. The ceiling is a Kconfig value
 * because it depends on what the line output is feeding.
 */
static float pcm512x_map_volume_db(float airplay_db) {
  const float max_db = (float)CONFIG_PCM512X_MAX_VOLUME;

  if (airplay_db > 0.0f) {
    airplay_db = 0.0f;
  } else if (airplay_db < -30.0f) {
    airplay_db = -30.0f;
  }

  if (airplay_db >= -25.0f) {
    return max_db + airplay_db * 2.0f;
  }

  // -25 dB maps to max_db - 50; roll the remaining 5 dB down to -103.
  const float knee_db = max_db - 50.0f;
  const float frac = (-25.0f - airplay_db) / 5.0f;
  return knee_db + frac * (PCM512X_VOL_MIN_DB - knee_db);
}

/** 0x00 = +24 dB, 0.5 dB per step. */
static uint8_t pcm512x_db_to_reg(float db) {
  if (db <= PCM512X_VOL_MIN_DB) {
    return PCM512X_VOL_REG_MUTE;
  }
  if (db > PCM512X_VOL_REG_MAX_DB) {
    db = PCM512X_VOL_REG_MAX_DB;
  }
  return (uint8_t)((PCM512X_VOL_REG_MAX_DB - db) * 2.0f + 0.5f);
}

// Caller holds the mutex.
static void pcm512x_apply_volume_locked(void) {
  const uint8_t reg = pcm512x_db_to_reg(s_volume_db);

  if (pcm512x_page(PCM512X_PAGE_CONTROL) != ESP_OK) {
    return;
  }
  if (pcm512x_write(PCM512X_REG_VOL_LEFT, reg) != ESP_OK ||
      pcm512x_write(PCM512X_REG_VOL_RIGHT, reg) != ESP_OK) {
    ESP_LOGW(TAG, "Volume write failed");
  }
}

/* ============================== clocking ============================== */

/**
 * Point the PLL at whichever clock is actually there.
 *
 * P0-R94 bit 6 reports the SCK pin directly, independently of what R13
 * currently selects, so a board that never wired MCLK is detected rather than
 * configured. Without MCLK the PLL runs off BCK, and then the SCK-halt and
 * LRCK/BCK-missing detectors have to be masked or every gap between tracks
 * drops the part into powerdown.
 *
 * Caller holds the mutex and must have already put the part in standby: a live
 * PLL re-lock is audible.
 */
static void pcm512x_apply_clocking_locked(void) {
  uint8_t clk = 0;
  bool mclk = pcm512x_read(PCM512X_REG_CLOCK_STATUS, &clk) == ESP_OK &&
              !(clk & PCM512X_CLK_SCK_MISSING);

#if CONFIG_PCM512X_FORCE_BCK_PLL
  mclk = false;
#endif

  const uint8_t pll_ref = mclk ? PCM512X_PLL_REF_SCK : PCM512X_PLL_REF_BCK;
  const uint8_t ignore_err =
      mclk ? 0x00 : (PCM512X_IGNORE_CLOCK_HALT | PCM512X_IGNORE_CLOCK_MISSING);

  if (pcm512x_write(PCM512X_REG_PLL_REF, pll_ref) != ESP_OK ||
      pcm512x_write(PCM512X_REG_IGNORE_ERR, ignore_err) != ESP_OK) {
    ESP_LOGW(TAG, "Clock configuration write failed");
    return;
  }

  s_pll_ref_bck = !mclk;
  ESP_LOGI(TAG, "PLL reference = %s%s", mclk ? "MCLK/SCK" : "BCK",
           s_i2s_running ? "" : " (clocks not running yet)");
}

// Caller holds the mutex.
static void pcm512x_log_clocks_locked(void) {
  uint8_t clk = 0;
  uint8_t pll = 0;
  uint8_t fs = 0;
  uint8_t ratio_msb = 0;
  uint8_t ratio_lsb = 0;

  if (pcm512x_page(PCM512X_PAGE_CONTROL) != ESP_OK ||
      pcm512x_read(PCM512X_REG_CLOCK_STATUS, &clk) != ESP_OK ||
      pcm512x_read(PCM512X_REG_PLL_ENABLE, &pll) != ESP_OK ||
      pcm512x_read(PCM512X_REG_FS_SPEED, &fs) != ESP_OK ||
      pcm512x_read(PCM512X_REG_BCK_RATIO_MSB, &ratio_msb) != ESP_OK ||
      pcm512x_read(PCM512X_REG_BCK_RATIO_LSB, &ratio_lsb) != ESP_OK) {
    ESP_LOGW(TAG, "Clock status read failed");
    return;
  }

  // R4 bit 4 and R94 bit 5 are both inverted: 0 means locked.
  ESP_LOGI(TAG,
           "clocks: sck=%s bck=%s fs=%s pll=%s ref=%s bck_ratio=%u fs_class=%u",
           (clk & PCM512X_CLK_SCK_MISSING) ? "missing" : "present",
           (clk & PCM512X_CLK_BCK_INVALID) ? "invalid" : "ok",
           (clk & PCM512X_CLK_FS_INVALID) ? "invalid" : "ok",
           (pll & PCM512X_PLL_UNLOCKED) ? "unlocked" : "locked",
           s_pll_ref_bck ? "BCK" : "SCK",
           (unsigned)(((uint16_t)ratio_msb << 8) | ratio_lsb), fs & 0x07);
}

/* ============================ power control ============================ */

// Caller holds the mutex.
static void pcm512x_apply_power_locked(void) {
  if (pcm512x_page(PCM512X_PAGE_CONTROL) != ESP_OK) {
    return;
  }

  switch (s_power_mode) {
  case DAC_POWER_ON:
    if (!s_i2s_running) {
      // The PLL has no reference yet. on_i2s_started() finishes the job.
      return;
    }
    pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_ACTIVE);
    vTaskDelay(pdMS_TO_TICKS(PCM512X_POWER_UP_SETTLE_MS));
    pcm512x_write(PCM512X_REG_MUTE,
                  s_output_enabled ? PCM512X_MUTE_NONE : PCM512X_MUTE_BOTH);
    break;

  case DAC_POWER_STANDBY:
    pcm512x_write(PCM512X_REG_MUTE, PCM512X_MUTE_BOTH);
    pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_STANDBY);
    break;

  case DAC_POWER_OFF:
    // Nothing lives in RAM on this part, so a full powerdown costs only the
    // charge-pump ramp on the way back.
    pcm512x_write(PCM512X_REG_MUTE, PCM512X_MUTE_BOTH);
    pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_DOWN);
    break;
  }
}

/* ============================== bring-up ============================== */

/**
 * Warn when the part on the bus looks like a TAS575x.
 *
 * P1-R5 (undervoltage protection) resets to 0x00 on a PCM512x and 0x11 on a
 * TAS575x, and neither driver ever writes it. Read before anything else is
 * touched this is a reliable hint — but only a hint, since a warm reboot can
 * leave any register wherever the previous firmware put it.
 */
static void pcm512x_check_identity_locked(void) {
  uint8_t uvp = 0;

  if (pcm512x_page(PCM512X_PAGE_ANALOG) != ESP_OK ||
      pcm512x_read(PCM512X_P1_REG_UVP, &uvp) != ESP_OK) {
    return;
  }
  (void)pcm512x_page(PCM512X_PAGE_CONTROL);

  if (uvp == PCM512X_P1_UVP_TAS575X_RESET) {
    ESP_LOGW(TAG,
             "@0x%02X P1-R5 reads 0x%02X — this looks like a TAS575x, not a "
             "PCM512x. Check CONFIG_DAC_PCM512X.",
             s_addr, uvp);
  }
}

// Caller holds the mutex.
static void pcm512x_configure_analog_locked(void) {
  if (pcm512x_page(PCM512X_PAGE_ANALOG) != ESP_OK) {
    ESP_LOGW(TAG, "Analogue page select failed");
    return;
  }

#if CONFIG_PCM512X_ANALOG_GAIN_MINUS_6DB
  const uint8_t gain = PCM512X_P1_ANALOG_GAIN_MINUS_6DB;
#else
  const uint8_t gain = 0x00;
#endif
  pcm512x_write(PCM512X_P1_REG_ANALOG_GAIN, gain);

  (void)pcm512x_page(PCM512X_PAGE_CONTROL);
  ESP_LOGI(TAG, "Line output: %s (%s)",
           gain ? "-6 dB" : "0 dB (2.1 Vrms full scale)",
           gain ? "attenuated" : "default");
}

/**
 * Put the part in a known state: muted, in standby, PLL pointed at a clock
 * that exists. Everything else is left at its reset value — the ROM process
 * flow (P0-R43 = 1) and the reset I2S format are what this pipeline wants.
 */
static void pcm512x_program_locked(void) {
  if (pcm512x_page(PCM512X_PAGE_CONTROL) != ESP_OK) {
    ESP_LOGW(TAG, "Page select failed");
    return;
  }

  pcm512x_write(PCM512X_REG_MUTE, PCM512X_MUTE_BOTH);
  pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_STANDBY);
  vTaskDelay(pdMS_TO_TICKS(PCM512X_STANDBY_SETTLE_MS));

  pcm512x_apply_clocking_locked();
}

/* ================================ ops ================================ */

static esp_err_t pcm512x_init(void *i2c_bus) {
  if (i2c_bus == NULL) {
    ESP_LOGE(TAG, "No I2C bus supplied");
    return ESP_ERR_INVALID_ARG;
  }
  if (s_dev != NULL) {
    ESP_LOGW(TAG, "Already initialised");
    return ESP_OK;
  }

  s_bus = (i2c_master_bus_handle_t)i2c_bus;

  if (s_mutex == NULL) {
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  uint8_t found = 0;
  for (size_t i = 0; i < sizeof(s_addr_candidates); i++) {
    const uint8_t addr = s_addr_candidates[i];
    if (i2c_master_probe(s_bus, addr, BOARD_I2C_TIMEOUT_MS) != ESP_OK) {
      continue;
    }
    found++;
    if (s_addr == 0) {
      s_addr = addr;
    } else {
      ESP_LOGW(TAG, "Extra device at 0x%02X ignored — using 0x%02X only", addr,
               s_addr);
    }
  }

  if (found == 0) {
    ESP_LOGE(TAG, "No PCM512x found at 0x4C-0x4F");
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t err =
      board_i2c_add_device(s_bus, s_addr, PCM512X_I2C_SPEED_HZ, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add device 0x%02X: %s", s_addr,
             esp_err_to_name(err));
    return err;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  pcm512x_check_identity_locked();
  pcm512x_program_locked();
  pcm512x_configure_analog_locked();
  pcm512x_apply_volume_locked();
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "PCM512x initialised at 0x%02X (standby, muted)", s_addr);
  return ESP_OK;
}

static esp_err_t pcm512x_deinit(void) {
  if (s_dev == NULL) {
    return ESP_OK;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  (void)pcm512x_page(PCM512X_PAGE_CONTROL);
  pcm512x_write(PCM512X_REG_MUTE, PCM512X_MUTE_BOTH);
  pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_DOWN);
  board_i2c_remove_device(s_dev);
  s_dev = NULL;
  s_addr = 0;
  s_bus = NULL;
  s_i2s_running = false;
  xSemaphoreGive(s_mutex);

  return ESP_OK;
}

static void pcm512x_set_volume(float volume_db) {
  if (s_dev == NULL) {
    return;
  }

  const float mapped = pcm512x_map_volume_db(volume_db);

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_volume_db = mapped;
  pcm512x_apply_volume_locked();
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "Volume %.1f dB -> %.1f dB (reg 0x%02X)", (double)volume_db,
           (double)mapped, pcm512x_db_to_reg(mapped));
}

static void pcm512x_set_power_mode(dac_power_mode_t mode) {
  if (s_dev == NULL) {
    return;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_power_mode = mode;
  pcm512x_apply_power_locked();
  xSemaphoreGive(s_mutex);
}

/**
 * The clocks are live for the first time, or the sample rate just changed.
 *
 * This is the only point at which the SCK detector means anything — dac_init()
 * runs before the I2S channel exists — so the PLL reference is re-evaluated
 * here and the requested power mode applied on top.
 */
static void pcm512x_on_i2s_started(uint32_t sample_rate_hz) {
  if (s_dev == NULL) {
    return;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_i2s_running = true;
  s_i2s_rate_hz = sample_rate_hz;

  if (pcm512x_page(PCM512X_PAGE_CONTROL) == ESP_OK) {
    pcm512x_write(PCM512X_REG_MUTE, PCM512X_MUTE_BOTH);
    pcm512x_write(PCM512X_REG_POWER, PCM512X_POWER_STANDBY);
    vTaskDelay(pdMS_TO_TICKS(PCM512X_STANDBY_SETTLE_MS));

    pcm512x_apply_clocking_locked();
    pcm512x_apply_power_locked();
    pcm512x_log_clocks_locked();
  }
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "I2S started at %" PRIu32 " Hz", sample_rate_hz);
}

/**
 * There is no amplifier on a PCM512x, so this is the digital mute — which is
 * what the callers actually want when they gate the output on a jack insert or
 * a board fault.
 */
static void pcm512x_enable_speaker(bool enable) {
  if (s_dev == NULL) {
    return;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_output_enabled = enable;
  if (s_power_mode == DAC_POWER_ON && s_i2s_running &&
      pcm512x_page(PCM512X_PAGE_CONTROL) == ESP_OK) {
    pcm512x_write(PCM512X_REG_MUTE,
                  enable ? PCM512X_MUTE_NONE : PCM512X_MUTE_BOTH);
  }
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "Output %s", enable ? "unmuted" : "muted");
}

/* ============================== public API ============================== */

int dac_pcm512x_get_device_count(void) {
  return s_dev != NULL ? 1 : 0;
}

void dac_pcm512x_log_status(void) {
  if (s_dev == NULL) {
    ESP_LOGW(TAG, "Status unavailable, DAC not initialised");
    return;
  }

  uint8_t power = 0;
  uint8_t analog_mute = 0;
  uint8_t auto_mute = 0;
  uint8_t short_detect = 0;
  uint8_t overflow = 0;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const bool ok =
      pcm512x_page(PCM512X_PAGE_CONTROL) == ESP_OK &&
      pcm512x_read(PCM512X_REG_POWER_STATE, &power) == ESP_OK &&
      pcm512x_read(PCM512X_REG_ANALOG_MUTE_MON, &analog_mute) == ESP_OK &&
      pcm512x_read(PCM512X_REG_AUTO_MUTE_FLAGS, &auto_mute) == ESP_OK &&
      pcm512x_read(PCM512X_REG_SHORT_DETECT, &short_detect) == ESP_OK &&
      pcm512x_read(PCM512X_REG_DSP_OVERFLOW, &overflow) == ESP_OK;
  if (ok) {
    pcm512x_log_clocks_locked();
  }
  xSemaphoreGive(s_mutex);

  if (!ok) {
    ESP_LOGW(TAG, "@0x%02X status read failed", s_addr);
    return;
  }

  // The analogue mute monitor is active low: 0 means that channel is muted.
  ESP_LOGI(TAG,
           "@0x%02X state=%s%s analog-mute=[%c%c] auto-mute=[%c%c] "
           "short=0x%02X dsp-overflow=0x%02X rate=%" PRIu32 " Hz",
           s_addr, pcm512x_power_state_str(power & PCM512X_POWER_STATE_MASK),
           (power & PCM512X_POWER_BOOT_DONE) ? "" : " dsp-booting",
           (analog_mute & 0x01) ? '-' : 'L', (analog_mute & 0x02) ? '-' : 'R',
           (auto_mute & 0x01) ? 'L' : '-', (auto_mute & 0x10) ? 'R' : '-',
           short_detect, overflow, s_i2s_rate_hz);
}

const dac_ops_t dac_pcm512x_ops = {
    .init = pcm512x_init,
    .deinit = pcm512x_deinit,
    .set_volume = pcm512x_set_volume,
    .set_power_mode = pcm512x_set_power_mode,
    .on_i2s_started = pcm512x_on_i2s_started,
    .enable_speaker = pcm512x_enable_speaker,
    .enable_line_out = NULL,
};
