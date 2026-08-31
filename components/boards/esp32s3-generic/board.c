/**
 * @file board.c
 * @brief ESP32-S3 Generic board implementation
 *
 * Minimal implementation for generic ESP32-S3 dev boards with external I2S DAC.
 * A plain I2S DAC (PCM5102A and friends) needs no initialisation at all; an
 * I2C-controlled one is brought up here when DAC_I2C_SDA/SCL are configured.
 */

#include "iot_board.h"

#include "driver/gpio.h"
#include "esp_log.h"

#if BOARD_HAS_DAC_I2C
#include "dac.h"
#include "driver/i2c_master.h"
#include "settings.h"
#endif

#ifdef CONFIG_DAC_PCM512X
#include "dac_pcm512x.h"
#endif

static const char TAG[] = "ESP32S3-Generic";

static bool s_board_initialized = false;

#if BOARD_HAS_DAC_I2C
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
#endif

// -1 disables the pin, and the runtime test that used to guard this came too
// late: the shift below is a constant the compiler folds either way.
#if defined(CONFIG_MUTE_GPIO) && CONFIG_MUTE_GPIO >= 0
static esp_err_t init_mute_gpio(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << CONFIG_MUTE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure mute GPIO: %s", esp_err_to_name(err));
    return err;
  }

  // Initialize to unmuted state — set opposite of active level
  gpio_set_level(CONFIG_MUTE_GPIO, !CONFIG_MUTE_GPIO_LEVEL);

  ESP_LOGI(TAG, "Mute GPIO %d initialized (active %s, init %s)",
           CONFIG_MUTE_GPIO, CONFIG_MUTE_GPIO_LEVEL ? "high" : "low",
           CONFIG_MUTE_GPIO_LEVEL ? "low" : "high");
  return ESP_OK;
}
#endif

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
#if BOARD_HAS_DAC_I2C
  if (id == BOARD_I2C_DAC_ID) {
    return (board_res_handle_t)s_i2c_bus_handle;
  }
#endif
  (void)id;
  return NULL;
}

#if BOARD_HAS_DAC_I2C
static esp_err_t init_dac_i2c(void) {
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC I2C bus: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "DAC I2C bus %d initialized: sda=%d, scl=%d", BOARD_I2C_PORT,
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
  return ESP_OK;
}

static esp_err_t init_dac(void) {
#ifdef CONFIG_DAC_PCM512X
  dac_register(&dac_pcm512x_ops);
#endif

  esp_err_t err = dac_init(s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC: %s", esp_err_to_name(err));
    return err;
  }

  float vol_db;
  if (settings_get_volume(&vol_db) == ESP_OK) {
    dac_set_volume(vol_db);
  }
  return ESP_OK;
}
#endif /* BOARD_HAS_DAC_I2C */

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

#if defined(CONFIG_MUTE_GPIO) && CONFIG_MUTE_GPIO >= 0
  esp_err_t err = init_mute_gpio();
  if (err != ESP_OK) {
    return err;
  }
#endif

#if BOARD_HAS_DAC_I2C
  esp_err_t dac_err = init_dac_i2c();
  if (dac_err == ESP_OK) {
    dac_err = init_dac();
  }
  if (dac_err != ESP_OK) {
    // I2S keeps running either way, and a PCM512x plays at its reset defaults,
    // so a wiring mistake costs volume control rather than the whole receiver.
    ESP_LOGE(TAG, "DAC unavailable — continuing without hardware volume");
  }
#elif defined(CONFIG_DAC_PCM512X)
  ESP_LOGE(TAG, "CONFIG_DAC_PCM512X is set but DAC_I2C_SDA/SCL are not — "
                "no bus to reach the DAC on");
#endif

  s_board_initialized = true;
  ESP_LOGI(TAG, "Generic board initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
#if BOARD_HAS_DAC_I2C
  dac_deinit();
  if (s_i2c_bus_handle != NULL) {
    i2c_del_master_bus(s_i2c_bus_handle);
    s_i2c_bus_handle = NULL;
  }
#endif
  s_board_initialized = false;
  return ESP_OK;
}
