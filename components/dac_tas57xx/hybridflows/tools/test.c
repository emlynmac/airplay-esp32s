/* Checks that tas57xx_hf1_apply() is idempotent against its own output, and
 * that changing one EQ band moves only that band's words. If both hold, the
 * computed values are self-consistent and any fault is in the I2C transport. */
#include "flat.h"
#include "tas57xx_hf1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    exit(1);
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

static int diff_words(const uint8_t *a, const uint8_t *b, size_t size,
                      const char *label) {
  int changed[TAS57XX_CRAM_WORD_COUNT];
  memset(changed, 0, sizeof(changed));
  size_t pos = 0;
  int page = -1, total = 0;
  while (pos + 1 < size && !(a[pos] == 0xFF && a[pos + 1] == 0xFF)) {
    int reg = a[pos], len = a[pos + 1];
    if (reg == 0x00 && len == 1) {
      page = a[pos + 2];
    } else if (page >= 0x2C) {
      int base = page >= 0x3E ? page - 0x12 : page;
      for (int off = 0; off + 4 <= len; off += 4) {
        int r = reg + off;
        if (r < 0x08 || (r - 0x08) % 4) {
          continue;
        }
        int w = (base - 0x2C) * 30 + (r - 0x08) / 4;
        if (w >= 0 && w < TAS57XX_CRAM_WORD_COUNT &&
            memcmp(&a[pos + 2 + off], &b[pos + 2 + off], 3)) {
          changed[w] = 1;
        }
      }
    }
    pos += 2 + (size_t)len;
  }
  printf("%-42s changed words:", label);
  for (int w = 0; w < TAS57XX_CRAM_WORD_COUNT; w++) {
    if (changed[w]) {
      printf(" %d", w);
      total++;
    }
  }
  printf("%s\n", total ? "" : " (none)");
  return total;
}

static void base_config(tas57xx_hf1_config_t *cfg) {
  flat_config(cfg);
}

int main(int argc, char **argv) {
  size_t size;
  uint8_t *flat = slurp(argv[1], &size);
  uint8_t *work = malloc(size);
  int fails = 0;

  tas57xx_hf1_config_t cfg;
  base_config(&cfg);

  memcpy(work, flat, size);
  tas57xx_cram_sink_t sink = {.image = work, .image_size = size};
  if (tas57xx_hf1_apply(&sink, &cfg) != ESP_OK) {
    printf("apply failed\n");
    return 1;
  }
  if (diff_words(flat, work, size, "re-apply identical config")) {
    printf("  ^ NOT IDEMPOTENT: values disagree with the flow it produced\n");
    fails++;
  }

  memcpy(work, flat, size);
  cfg.eq[0].type = TAS57XX_BQ_PEAKING;
  cfg.eq[0].freq_hz = 100.0f;
  cfg.eq[0].q = 1.0f;
  cfg.eq[0].gain_db = 3.0f;
  sink.image = work;
  tas57xx_hf1_apply(&sink, &cfg);
  diff_words(flat, work, size, "EQ band 1 -> peaking 100Hz Q1 +3dB");

  memcpy(work, flat, size);
  base_config(&cfg);
  cfg.eq[4].type = TAS57XX_BQ_HIGHPASS;
  cfg.eq[4].freq_hz = 2000.0f;
  cfg.eq[4].q = 0.707f;
  sink.image = work;
  tas57xx_hf1_apply(&sink, &cfg);
  diff_words(flat, work, size, "EQ band 5 -> HP2 2kHz");

  printf("\n%s\n", fails ? "FAIL" : "values self-consistent");
  return fails;
}
