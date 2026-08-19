/**
 * HybridFlow 1 parameter map for the TAS5754M.
 *
 * Slot addresses are specific to this flow — nothing in the .bin identifies
 * which flow is loaded, so these are only valid once HF1 has been downloaded.
 */
#pragma once

#include "tas57xx_cram.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAS57XX_HF1_EQ_BANDS 10

#define TAS57XX_HF1_PBE_HPF_MIN_HZ   50.0f
#define TAS57XX_HF1_PBE_HPF_MAX_HZ   300.0f
#define TAS57XX_HF1_PBE_HARMONIC_MAX 100
#define TAS57XX_HF1_PBE_EFFECT_MIN   1
#define TAS57XX_HF1_PBE_EFFECT_MAX   5

/** Psychoacoustic bass enhancer settings, matching the PurePath Console pane.
 */
typedef struct {
  float hpf_hz; /**< extraction corner, 40-200 Hz */
  int harmonic; /**< harmonic intensity, 0-100 (0 mutes the generator) */
  int effect;   /**< effect intensity, 1-5 */
} tas57xx_hf1_pbe_t;

/** Program one band of the 10-band parametric EQ chain. */
esp_err_t tas57xx_hf1_set_eq_band(const tas57xx_cram_sink_t *sink, int band,
                                  const tas57xx_bq_t *bq,
                                  uint32_t sample_rate_hz);

/**
 * Retune the whole bass enhancer. The effect shelf is derived from the HPF
 * corner as well as the intensity, so all of it is written together to stop
 * the two from drifting out of step.
 */
esp_err_t tas57xx_hf1_set_pbe(const tas57xx_cram_sink_t *sink,
                              const tas57xx_hf1_pbe_t *pbe,
                              uint32_t sample_rate_hz);

#define TAS57XX_HF1_DBE_EQ_BANDS 2

/** Program one band of the DBE's high-level (loud path) EQ. */
esp_err_t tas57xx_hf1_set_dbe_hl_eq_band(const tas57xx_cram_sink_t *sink,
                                         int band, const tas57xx_bq_t *bq,
                                         uint32_t sample_rate_hz);

/** Program one band of the DBE's low-level (quiet path) EQ. */
esp_err_t tas57xx_hf1_set_dbe_ll_eq_band(const tas57xx_cram_sink_t *sink,
                                         int band, const tas57xx_bq_t *bq,
                                         uint32_t sample_rate_hz);

/**
 * Set the levels the DBE crossfades between, in dBFS. The pair is stored as a
 * threshold and a reciprocal span, so very narrow ranges are not representable
 * and are rejected; roughly 4 dB is the minimum near -20 dBFS, more further
 * down.
 */
esp_err_t tas57xx_hf1_set_dbe_mix(const tas57xx_cram_sink_t *sink,
                                  float lower_db, float upper_db);

/**
 * Set the band the DBE energy estimator listens to. Realised as one bandpass
 * centred on the geometric mean of the two boundaries.
 */
esp_err_t tas57xx_hf1_set_sensing_band(const tas57xx_cram_sink_t *sink,
                                       float lower_hz, float upper_hz,
                                       uint32_t sample_rate_hz);

/** Set the energy estimator's averaging window in milliseconds. */
esp_err_t tas57xx_hf1_set_energy_window(const tas57xx_cram_sink_t *sink,
                                        float window_ms,
                                        uint32_t sample_rate_hz);

#define TAS57XX_HF1_DRC_BANDS 3

#define TAS57XX_HF1_DRC_TIME_MIN_MS 0.1f
#define TAS57XX_HF1_DRC_TIME_MAX_MS 10000.0f

enum {
  TAS57XX_HF1_DRC_LOW = 0,
  TAS57XX_HF1_DRC_MID,
  TAS57XX_HF1_DRC_HIGH,
};

/** One compander band's detector timing, in milliseconds. */
typedef struct {
  float energy_ms; /**< level-estimator averaging window */
  float attack_ms;
  float decay_ms;
} tas57xx_hf1_drc_timing_t;

/**
 * Set the two corners of the compander's 3-band crossover. All four sections
 * are written together because the mid band shares a corner with each of its
 * neighbours.
 */
esp_err_t tas57xx_hf1_set_drc_crossover(const tas57xx_cram_sink_t *sink,
                                        float low_hz, float high_hz,
                                        uint32_t sample_rate_hz);

/** Set one compander band's detector timing. */
esp_err_t tas57xx_hf1_set_drc_timing(const tas57xx_cram_sink_t *sink, int band,
                                     const tas57xx_hf1_drc_timing_t *timing,
                                     uint32_t sample_rate_hz);

#define TAS57XX_HF1_DRC_REGIONS   3
#define TAS57XX_HF1_DRC_RATIO_MIN 0.2f
#define TAS57XX_HF1_DRC_RATIO_MAX 5.0f

enum {
  TAS57XX_HF1_DRC_COMPRESS = 0,
  TAS57XX_HF1_DRC_EXPAND,
};

/** One segment of the compander's piecewise-linear transfer curve. */
typedef struct {
  int mode;    /**< TAS57XX_HF1_DRC_COMPRESS or _EXPAND */
  float ratio; /**< 0.2 to 5.0; 1.0 leaves the segment flat either way */
} tas57xx_hf1_drc_region_t;

/**
 * Set the compander transfer curve: three regions bottom-up, split by two
 * level thresholds. The per-region offset in PurePath Console has no effect
 * here - changing it emits no I2C traffic at all.
 */
esp_err_t tas57xx_hf1_set_drc_curve(
    const tas57xx_cram_sink_t *sink,
    const tas57xx_hf1_drc_region_t regions[TAS57XX_HF1_DRC_REGIONS],
    float thresh1_db, float thresh2_db);

/** Set the output smooth-clip threshold, -78..0 dBFS. */
esp_err_t tas57xx_hf1_set_smooth_clip(const tas57xx_cram_sink_t *sink,
                                      float threshold_db);

#define TAS57XX_HF1_CONFIG_MAGIC   0x48463145u /* "HF1E" */
#define TAS57XX_HF1_CONFIG_VERSION 2u

/**
 * Every tunable parameter of the flow, in one blob.
 *
 * Coefficient RAM cannot be read back while the DSP is running, so this struct
 * — not the device — is the source of truth for what the current tuning is.
 */
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate_hz; /**< rate the flow was designed for */

  tas57xx_bq_t eq[TAS57XX_HF1_EQ_BANDS];

  tas57xx_hf1_pbe_t pbe;

  tas57xx_bq_t dbe_high[TAS57XX_HF1_DBE_EQ_BANDS];
  tas57xx_bq_t dbe_low[TAS57XX_HF1_DBE_EQ_BANDS];
  float dbe_lower_db;
  float dbe_upper_db;
  float sense_lower_hz;
  float sense_upper_hz;
  float sense_window_ms;

  float drc_low_hz;
  float drc_high_hz;
  tas57xx_hf1_drc_timing_t drc_timing[TAS57XX_HF1_DRC_BANDS];
  tas57xx_hf1_drc_region_t drc_region[TAS57XX_HF1_DRC_REGIONS];
  float drc_thresh1_db;
  float drc_thresh2_db;

  float smooth_clip_db;
} tas57xx_hf1_config_t;

/** Fill in a neutral tuning: flat EQ, bass enhancer muted, compander flat. */
void tas57xx_hf1_defaults(tas57xx_hf1_config_t *cfg);

/**
 * Write a whole tuning to a live device or into a flow image.
 *
 * Carries on past a rejected section so one bad parameter cannot leave the
 * chain half-programmed; the first error seen is returned at the end.
 */
esp_err_t tas57xx_hf1_apply(tas57xx_cram_sink_t *sink,
                            const tas57xx_hf1_config_t *cfg);

#ifdef __cplusplus
}
#endif
