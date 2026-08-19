/* Prints designed coefficients for a set of sections so the page's JavaScript
 * copy of design() can be diffed against the firmware's. */
#include "tas57xx_cram.h"

#include <stdio.h>

int main(void) {
  const tas57xx_bq_t cases[] = {
      {TAS57XX_BQ_PEAKING, 0, 1000, 1.4f, 6.0f, {0}},
      {TAS57XX_BQ_PEAKING, 0, 120, 0.5f, -9.0f, {0}},
      {TAS57XX_BQ_PEAKING_BW, 0, 800, 2.0f, 4.5f, {0}},
      {TAS57XX_BQ_PEAKING_BW, 0, 3500, 0.33f, -3.0f, {0}},
      {TAS57XX_BQ_LOW_SHELF, 0, 200, 0.707f, 6.0f, {0}},
      {TAS57XX_BQ_HIGH_SHELF, 0, 6000, 0.707f, -4.0f, {0}},
      {TAS57XX_BQ_LOW_SHELF, 0, 200, 3.0f, 6.0f, {0}},    // Q capped to 1
      {TAS57XX_BQ_HIGH_SHELF, 0, 6000, 2.5f, 20.0f, {0}}, // gain capped to +12
      {TAS57XX_BQ_PEAKING, 0, 1000, 1.0f, -40.0f, {0}},   // gain capped to -15
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_VARIABLE_Q_2, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_BUTTERWORTH_1, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_BUTTERWORTH_2, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_BESSEL_2, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_CHEBYSHEV_2, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_LOWPASS, TAS57XX_BQ_SUB_LINKWITZ_RILEY_2, 2000, 1.2f, 0, {0}},
      {TAS57XX_BQ_HIGHPASS, TAS57XX_BQ_SUB_BUTTERWORTH_1, 90, 1.0f, 0, {0}},
      {TAS57XX_BQ_HIGHPASS, TAS57XX_BQ_SUB_BESSEL_2, 90, 1.0f, 0, {0}},
      {TAS57XX_BQ_BANDPASS, 0, 1000, 2.0f, 0, {0}},
      {TAS57XX_BQ_NOTCH, 0, 50, 5.0f, 0, {0}},    // 5 Hz wide
      {TAS57XX_BQ_NOTCH, 0, 1000, 200.0f, 0, {0}}, // 200 Hz wide
      {TAS57XX_BQ_PHASE_1, 0, 700, 1.0f, 0, {0}},
      {TAS57XX_BQ_PHASE_2, 0, 700, 1.5f, 0, {0}},
      {TAS57XX_BQ_BYPASS, 0, 1000, 1.0f, 0, {0}},
      {TAS57XX_BQ_CUSTOM, 0, 0, 0, 0, {0.9f, -1.2f, 0.4f, -1.1f, 0.35f}},
  };
  const double fs = 44100.0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    // design() is static, so go through the packer's own path instead.
    extern void tas57xx_bq_design_for_test(const tas57xx_bq_t *, double,
                                           double *, double *);
    double b[3], a[3];
    tas57xx_bq_design_for_test(&cases[i], fs, b, a);
    printf("%.12f %.12f %.12f %.12f %.12f\n", b[0], b[1], b[2], a[1], a[2]);
  }
  return 0;
}
