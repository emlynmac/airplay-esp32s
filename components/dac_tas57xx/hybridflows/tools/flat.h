/* The tuning that reproduces DMA80/hf1-flat-441.bin.
 *
 * gen.c writes that flow, mkcfg.c writes the .cfg naming it and test.c asserts
 * the two agree, so all three have to build the same config or the idempotency
 * check fails for no real reason. It is NOT tas57xx_hf1_defaults(): the
 * defaults carry TI's recommended starting tune, this is the flat canvas it is
 * applied to. */
#pragma once

#include "tas57xx_hf1.h"

static void flat_config(tas57xx_hf1_config_t *cfg) {
  tas57xx_hf1_defaults(cfg);
  cfg->sample_rate_hz = 44100;
  for (int i = 0; i < TAS57XX_HF1_EQ_BANDS; i++) {
    cfg->eq[i].type = TAS57XX_BQ_BYPASS;
  }
  for (int i = 0; i < TAS57XX_HF1_DBE_EQ_BANDS; i++) {
    cfg->dbe_high[i].type = TAS57XX_BQ_BYPASS;
    cfg->dbe_low[i].type = TAS57XX_BQ_BYPASS;
  }
  cfg->pbe.harmonic = 0;
  cfg->pbe.effect = 1;
  cfg->dbe_lower_db = -31.0f;
  cfg->dbe_upper_db = -10.0f;
  /* Pinned rather than left to the defaults so the diff against the vendor
   * flow stays minimal even if the recommended split moves. */
  cfg->drc_cross[TAS57XX_HF1_DRC_CROSS_LOW].freq_hz = 300.0f;
  cfg->drc_cross[TAS57XX_HF1_DRC_CROSS_MID_A].freq_hz = 5000.0f;
  cfg->drc_cross[TAS57XX_HF1_DRC_CROSS_MID_B].freq_hz = 300.0f;
  cfg->drc_cross[TAS57XX_HF1_DRC_CROSS_HIGH].freq_hz = 5000.0f;
  for (int i = 0; i < TAS57XX_HF1_DRC_REGIONS; i++) {
    cfg->drc_region[i].mode = TAS57XX_HF1_DRC_COMPRESS;
    cfg->drc_region[i].ratio = 1.0f;
  }
}
