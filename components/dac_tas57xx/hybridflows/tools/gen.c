/* Host-side HF1 flow generator: patches a captured HybridFlow 1 binary with a
 * tuning built from the same firmware sources the device runs, so the two can
 * never drift. Build: see build.sh in this directory. */
#include "flat.h"
#include "tas57xx_hf1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "%s: short read\n", path);
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

/* Walk the [reg,len,data...] stream and report every coefficient word whose
 * bytes differ, as a cross-check that only the intended slots moved. */
static void report_diff(const uint8_t *a, const uint8_t *b, size_t size) {
  int changed[TAS57XX_CRAM_WORD_COUNT];
  memset(changed, 0, sizeof(changed));
  size_t pos = 0;
  int page = -1;
  while (pos + 1 < size && !(a[pos] == 0xFF && a[pos + 1] == 0xFF)) {
    int reg = a[pos], len = a[pos + 1];
    if (pos + 2 + (size_t)len > size) {
      break;
    }
    if (reg == 0x00 && len == 1) {
      page = a[pos + 2];
    } else if (page >= 0x2C) {
      int base = page >= 0x3E ? page - 0x12 : page;
      for (int off = 0; off + 4 <= len; off += 4) {
        int r = reg + off;
        if (r < 0x08 || (r - 0x08) % 4) {
          continue;
        }
        int word = (base - 0x2C) * 30 + (r - 0x08) / 4;
        if (word < 0 || word >= TAS57XX_CRAM_WORD_COUNT) {
          continue;
        }
        if (memcmp(&a[pos + 2 + off], &b[pos + 2 + off], 3)) {
          changed[word] = 1;
        }
      }
    }
    pos += 2 + (size_t)len;
  }
  printf("changed words:");
  int run_start = -1;
  for (int w = 0; w <= TAS57XX_CRAM_WORD_COUNT; w++) {
    int on = w < TAS57XX_CRAM_WORD_COUNT && changed[w];
    if (on && run_start < 0) {
      run_start = w;
    } else if (!on && run_start >= 0) {
      if (w - 1 == run_start) {
        printf(" %d", run_start);
      } else {
        printf(" %d-%d", run_start, w - 1);
      }
      run_start = -1;
    }
  }
  printf("\n");
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <base-hf1.bin> <out.bin>\n", argv[0]);
    return 2;
  }

  size_t size = 0;
  uint8_t *base = slurp(argv[1], &size);
  if (!base) {
    return 1;
  }
  uint8_t *img = malloc(size);
  memcpy(img, base, size);

  tas57xx_hf1_config_t cfg;
  flat_config(&cfg);

  const tas57xx_cram_sink_t sink = {.image = img, .image_size = size};
  esp_err_t err = tas57xx_hf1_apply(&sink, &cfg);
  if (err != ESP_OK) {
    fprintf(stderr, "apply failed: %s\n", esp_err_to_name(err));
    return 1;
  }

  report_diff(base, img, size);

  FILE *out = fopen(argv[2], "wb");
  if (!out || fwrite(img, 1, size, out) != size) {
    perror(argv[2]);
    return 1;
  }
  fclose(out);
  printf("wrote %s (%zu bytes)\n", argv[2], size);
  return 0;
}
