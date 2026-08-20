/* Compares our encoders against the stock DMA80 flow, using the settings the
 * tuning screenshot shows for it. Any word listed is a genuine disagreement. */
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

static void diff(const uint8_t *a, const uint8_t *b, size_t size,
                 const char *label) {
  int changed[TAS57XX_CRAM_WORD_COUNT];
  memset(changed, 0, sizeof(changed));
  size_t pos = 0;
  int page = -1, total = 0, worst = 0;
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
        if (w < 0 || w >= TAS57XX_CRAM_WORD_COUNT) {
          continue;
        }
        const uint8_t *pa = &a[pos + 2 + off], *pb = &b[pos + 2 + off];
        int va = (pa[0] << 16) | (pa[1] << 8) | pa[2];
        int vb = (pb[0] << 16) | (pb[1] << 8) | pb[2];
        if (va & 0x800000) {
          va -= 0x1000000;
        }
        if (vb & 0x800000) {
          vb -= 0x1000000;
        }
        int d = va > vb ? va - vb : vb - va;
        if (d && !changed[w]) {
          changed[w] = d;
          if (d > worst) {
            worst = d;
          }
        }
      }
    }
    pos += 2 + (size_t)len;
  }
  printf("%-26s", label);
  for (int w = 0; w < TAS57XX_CRAM_WORD_COUNT; w++) {
    if (changed[w]) {
      printf(" %d(%+d)", w, changed[w]);
      total++;
    }
  }
  printf("%s   [worst %d LSB of 8388607]\n", total ? "" : " (none)", worst);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <flow.bin>\n", argv[0]);
    return 1;
  }
  size_t size;
  uint8_t *ref = slurp(argv[1], &size);
  uint8_t *img = malloc(size);
  const uint32_t fs = 44100;

  /* Values read off the tuning screenshot for this exact flow. */
  const tas57xx_hf1_pbe_t pbe = {.hpf_hz = 190.0f, .harmonic = 10, .effect = 2};

  memcpy(img, ref, size);
  tas57xx_cram_sink_t sink = {.image = img, .image_size = size};

  tas57xx_hf1_set_pbe(&sink, &pbe, fs);
  diff(ref, img, size, "PBE (190 Hz, 10, 2)");

  memcpy(img, ref, size);
  tas57xx_hf1_set_sensing_band(&sink, 50.0f, 200.0f, fs);
  diff(ref, img, size, "sensing band (50/200)");

  memcpy(img, ref, size);
  tas57xx_hf1_set_energy_window(&sink, 100.0f, fs);
  diff(ref, img, size, "energy window (100 ms)");

  memcpy(img, ref, size);
  tas57xx_hf1_set_dbe_mix(&sink, -31.0f, -10.0f);
  diff(ref, img, size, "DBE mix (-31/-10)");

  free(ref);
  free(img);
  return 0;
}
