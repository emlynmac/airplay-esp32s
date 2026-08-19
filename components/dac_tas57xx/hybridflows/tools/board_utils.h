#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Never reached: the generator only ever uses the image sink. */
static inline esp_err_t board_i2c_write(i2c_master_dev_handle_t dev,
                                        uint8_t reg, const uint8_t *data,
                                        size_t len) {
  (void)dev;
  (void)reg;
  (void)data;
  (void)len;
  return ESP_FAIL;
}
