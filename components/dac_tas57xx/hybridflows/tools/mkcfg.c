/* Writes the hf1.cfg that matches hf1-flat-441.bin, so the tuning page opens
 * showing what is actually resident instead of the firmware defaults. */
#include "flat.h"
#include "tas57xx_hf1.h"

#include <stdio.h>

int main(int argc, char **argv) {
  tas57xx_hf1_config_t cfg;
  flat_config(&cfg);

  FILE *f = fopen(argv[1], "wb");
  fwrite(&cfg, 1, sizeof(cfg), f);
  fclose(f);
  printf("%s: %zu bytes\n", argv[1], sizeof(cfg));
  return 0;
}
