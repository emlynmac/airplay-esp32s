/* Checks pass-filter alignments against coefficients captured from the tuning
 * tool. Values are the DBE EQ sections of the DMA80 flow at 48 kHz. */
#include "tas57xx_cram.h"
#include "tas57xx_hf1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *label, tas57xx_bq_type_t type,
                  tas57xx_bq_subtype_t sub, float hz, uint32_t fs,
                  int32_t want_a1h, int32_t want_a2) {
  tas57xx_bq_t bq = {.type = type, .subtype = sub, .freq_hz = hz, .q = 0.0f};
  int32_t c[TAS57XX_BQ_WORDS];
  tas57xx_bq_pack(&bq, fs, TAS57XX_BQ_NUM_FIRST, c);
  printf("%-34s -a1/2 %8d (want %8d, d %+6d)   -a2 %9d (want %9d, d %+6d)\n",
         label, c[3], want_a1h, c[3] - want_a1h, c[4], want_a2, c[4] - want_a2);
}

/* A flow image holding just words 143 and 144, so the encoder has somewhere to
 * write and the result can be read straight back out. */
static void check_mix(float lower_db, float upper_db, int32_t want_lower,
                      int32_t want_scale) {
  uint8_t img[] = {0x00, 0x01, 0x30, 0x64, 0x08, 0,    0,   0,
                   0,    0,    0,    0,    0,    0xFF, 0xFF};
  tas57xx_cram_sink_t sink = {.image = img, .image_size = sizeof(img)};
  char label[40];
  snprintf(label, sizeof(label), "DBE mix %.0f / %.0f dBFS", lower_db,
           upper_db);
  if (tas57xx_hf1_set_dbe_mix(&sink, lower_db, upper_db) != ESP_OK) {
    printf("%-34s rejected\n", label);
    return;
  }
  int32_t got[2];
  for (int i = 0; i < 2; i++) {
    const uint8_t *p = img + 5 + i * 4;
    got[i] = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
    if (got[i] & 0x800000) {
      got[i] -= 1 << 24;
    }
  }
  printf("%-34s lower %8d (want %8d, d %+6d)   scale %8d (want %8d, d %+6d)\n",
         label, got[0], want_lower, got[0] - want_lower, got[1], want_scale,
         got[1] - want_scale);
}

int main(void) {
  check("Chebyshev 0.5dB HP 150 Hz", TAS57XX_BQ_HIGHPASS,
        TAS57XX_BQ_SUB_CHEBYSHEV_2, 150.0f, 48000, 8310826, -8235158);
  check("Chebyshev 0.5dB LP 1 kHz", TAS57XX_BQ_LOWPASS,
        TAS57XX_BQ_SUB_CHEBYSHEV_2, 1000.0f, 48000, 7576652, -6963394);
  check("Bessel 2 HP 30 Hz", TAS57XX_BQ_HIGHPASS, TAS57XX_BQ_SUB_BESSEL_2,
        30.0f, 48000, 8360240, -8332002);
  check("Bessel 2 HP 1 kHz", TAS57XX_BQ_HIGHPASS, TAS57XX_BQ_SUB_BESSEL_2,
        1000.0f, 48000, 7475661, -6691729);
  check("Linkwitz-Riley 2 LP 1 kHz", TAS57XX_BQ_LOWPASS,
        TAS57XX_BQ_SUB_LINKWITZ_RILEY_2, 1000.0f, 48000, 7356611, -6451575);
  check("Butterworth 2 LP 1 kHz", TAS57XX_BQ_LOWPASS,
        TAS57XX_BQ_SUB_BUTTERWORTH_2, 1000.0f, 48000, 7613994, -6970783);
  check_mix(-10.0f, -6.0f, -1673747, 2246277);
  check_mix(-10.0f, -7.0f, -1673747, 3184758);
  return 0;
}
