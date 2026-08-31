/**
 * @file dac_pcm512x.h
 * @brief TI PCM5121 / PCM5122 line-out DAC driver
 */
#pragma once

#include "dac.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver ops for the PCM512x family
 *
 * Pass to dac_register() from the board init, then call dac_init() with the
 * I2C master bus handle the board owns.
 */
extern const dac_ops_t dac_pcm512x_ops;

/**
 * @brief Number of PCM512x devices found on the bus (0 or 1)
 */
int dac_pcm512x_get_device_count(void);

/**
 * @brief Log the read-only power, clock and mute status registers
 *
 * A no-op with a warning when the driver has not been initialised.
 */
void dac_pcm512x_log_status(void);

#ifdef __cplusplus
}
#endif
