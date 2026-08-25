#pragma once

#include "dac.h"
#include "tas58xx_biquad.h"

/**
 * TAS58xx (TAS5825M) DAC driver ops — register with dac_register() before
 * calling dac_init().
 */
extern const dac_ops_t dac_tas58xx_ops;

/** Per-amplifier level trim limits (dB), relative to the master volume. */
#define TAS58XX_TRIM_MIN_DB (-15.0f)
#define TAS58XX_TRIM_MAX_DB (15.0f)

/**
 * Set one amplifier's level trim in dB, relative to the master volume, so the
 * chips can be balanced against each other — a bridged sub against the
 * satellites, say. Clamped to [TAS58XX_TRIM_MIN_DB, TAS58XX_TRIM_MAX_DB].
 * Safe to call before dac_init(); the value is applied on the next volume
 * update.
 */
void dac_tas58xx_set_trim_db(int dev, float trim_db);

/** Get one amplifier's level trim in dB. Returns 0 for an unknown index. */
float dac_tas58xx_get_trim_db(int dev);

/**
 * Number of TAS58xx chips found on the I2C bus. Returns 0 before dac_init();
 * >1 means the board is a dual-DAC variant.
 */
int dac_tas58xx_get_device_count(void);

/**
 * Whether the second amplifier on a dual-DAC board is bridged (PBTL) mono
 * rather than a stereo pair. This describes how the board is wired and
 * nothing more: it selects the bridged output stage and sums L+R into that
 * chip. Any crossover between the two amplifiers is expressed as ordinary
 * biquad sections, not as a mode.
 */
bool dac_tas58xx_get_second_pbtl(void);

/** The wiring the chips were actually brought up in. */
bool dac_tas58xx_get_active_second_pbtl(void);

/**
 * Set whether the second amplifier is bridged. PBTL is a control-port setting
 * that can only be changed while the output stage is idle, so the new value is
 * stored and applied by the next dac_init() — the caller must restart.
 */
void dac_tas58xx_set_second_pbtl(bool pbtl);

/** Whether an amplifier is driving a bridged (PBTL) mono output right now. */
bool dac_tas58xx_is_pbtl(int dev);

/**
 * Which of the incoming stereo channels an amplifier plays. A bridged (PBTL)
 * amplifier drives one output from one channel of the pair, so it has to be
 * fed a summed or single-channel routing rather than TAS58XX_MIX_STEREO.
 */
typedef enum {
  TAS58XX_MIX_STEREO = 0, /* L -> left output, R -> right output */
  TAS58XX_MIX_MONO,       /* (L+R)/2 -> both outputs */
  TAS58XX_MIX_LEFT,       /* L -> both outputs */
  TAS58XX_MIX_RIGHT,      /* R -> both outputs */
  TAS58XX_MIX_COUNT,
} tas58xx_mix_t;

/**
 * Set one amplifier's input routing. Applied immediately if the chip is
 * playing, and re-applied on the next PLAY transition either way. Safe to
 * call before dac_init(), which is how a stored setting is restored.
 */
esp_err_t dac_tas58xx_set_mix(int dev, tas58xx_mix_t mix);

/** Get one amplifier's input routing. */
tas58xx_mix_t dac_tas58xx_get_mix(int dev);

/* ---------- Fully parametric biquad chain ----------
 *
 * Each amplifier runs a 15-section biquad chain per channel, and this chain is
 * the only writer of the chip's coefficient RAM. Crossovers, shelves and room
 * correction are all just sections in it.
 */

/** Channels per amplifier: 0 = left/CH1, 1 = right/CH2. */
#define TAS58XX_BQ_CHANNELS 2

/** Read one channel's chain. Returns false for an out-of-range index. */
bool dac_tas58xx_bq_get(int dev, int ch, tas58xx_bq_t out[TAS58XX_BQ_SLOTS]);

/** Replace one channel's chain and push it to the hardware. */
esp_err_t dac_tas58xx_bq_set(int dev, int ch,
                             const tas58xx_bq_t in[TAS58XX_BQ_SLOTS]);

/**
 * Gang the two channels of an amplifier: the left chain drives both, and the
 * right chain is left untouched so un-ganging restores it.
 */
void dac_tas58xx_bq_set_ganged(int dev, bool ganged);
bool dac_tas58xx_bq_get_ganged(int dev);

/** Sample rate the chain is currently designed against. */
uint32_t dac_tas58xx_bq_sample_rate(void);

/** Write the current chains to SPIFFS so they survive a reboot. */
esp_err_t dac_tas58xx_bq_commit(void);

/** Reload the chains from SPIFFS, discarding uncommitted edits. */
esp_err_t dac_tas58xx_bq_revert(void);

/** Reset every chain to bypass, in memory and on the hardware. */
esp_err_t dac_tas58xx_bq_reset(void);
