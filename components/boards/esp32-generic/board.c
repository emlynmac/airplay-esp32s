/**
 * @file board.c
 * @brief ESP32 Generic board implementation
 *
 * Minimal implementation for generic ESP32 dev boards with external I2S DAC.
 * No board-specific initialization required.
 */

#include "iot_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#ifdef CONFIG_DAC_WM8711BL
#include "dac_wm8711bl.h"
#include "driver/i2c_master.h"
#include "rtsp_events.h"
#include "settings.h"
#endif

static const char TAG[] = "ESP32-Generic";

static bool s_board_initialized = false;

#ifdef CONFIG_DAC_WM8711BL
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
#endif

#ifdef CONFIG_DAC_WM8711BL
static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
    dac_set_power_mode(DAC_POWER_STANDBY);
    break;
  case RTSP_EVENT_PLAYING:
    dac_set_power_mode(DAC_POWER_ON);
    break;
  case RTSP_EVENT_DISCONNECTED:
    dac_set_power_mode(DAC_POWER_OFF);
    break;
  case RTSP_EVENT_METADATA:
    break;
  }
}
#endif

#ifdef CONFIG_MUTE_GPIO
static esp_err_t init_mute_gpio(void) {
  if (CONFIG_MUTE_GPIO < 0) {
    return ESP_OK;
  }

  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << CONFIG_MUTE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);
  ESP_RETURN_ON_ERROR(err, TAG, "Failed to configure mute GPIO");

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
#ifdef CONFIG_DAC_WM8711BL
  if (id == BOARD_I2C_DAC_ID)
    return (board_res_handle_t)s_i2c_bus_handle;
#endif
  (void)id;
  return NULL;
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }
  esp_err_t err = ESP_OK;

#ifdef CONFIG_DAC_WM8711BL
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  err = i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(err));
    return err;
  }

  dac_register(&dac_wm8711bl_ops);

  err = dac_init(s_i2c_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC: %s", esp_err_to_name(err));
    return err;
  }

  float vol_db;
  if (ESP_OK == settings_get_volume(&vol_db)) {
    dac_set_volume(vol_db);
  }

  rtsp_events_register(on_rtsp_event, NULL);
#endif

#ifdef CONFIG_MUTE_GPIO
  err = init_mute_gpio();
  if (err != ESP_OK) {
    return err;
  }
#endif

  s_board_initialized = true;
  ESP_LOGI(TAG, "Generic board initialized (no board-specific init needed)");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  s_board_initialized = false;
  return ESP_OK;
}
