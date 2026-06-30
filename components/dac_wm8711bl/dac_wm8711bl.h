#pragma once

#include "dac.h"

/**
 * Wolfson WM8711BL DAC driver ops — register with dac_register() before calling
 * dac_init().
 */
extern const dac_ops_t dac_wm8711bl_ops;