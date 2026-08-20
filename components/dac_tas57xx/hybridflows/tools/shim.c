#include "tas57xx_cram.h"
#define static
#include "tas57xx_cram.c"
#undef static
void tas57xx_bq_design_for_test(const tas57xx_bq_t *bq, double fs, double *b, double *a) {
  design(bq, fs, b, a);
}
