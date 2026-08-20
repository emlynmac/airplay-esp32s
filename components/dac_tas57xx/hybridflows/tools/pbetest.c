/*
 * Proves the PBE crossfade writers against the PPC2 bypass captures.
 *
 * The old HF3 writer was inverted in both directions at once, so a round-trip
 * test could not see it. This one checks the words against known-good values
 * from the part instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tas57xx_cram.h"
#include "tas57xx_hf1.h"
#include "tas57xx_hf3.h"

static int fails = 0;

static void check(const char *what, const tas57xx_cram_sink_t *sink, int word,
                  int32_t want_wet, int32_t want_dry) {
  int32_t w[2] = {-1, -1};
  tas57xx_cram_read_image(sink->image, sink->image_size, word, w, 2);
  const int ok = (w[0] == want_wet && w[1] == want_dry);
  printf("  %-22s w%d=%06x w%d=%06x  %s\n", what, word, w[0] & 0xFFFFFF,
         word + 1, w[1] & 0xFFFFFF, ok ? "ok" : "MISMATCH");
  if (!ok) {
    fails++;
  }
}

static uint8_t *slurp(const char *path, size_t *out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *img = malloc((size_t)size);
  if (fread(img, 1, (size_t)size, f) != (size_t)size) {
    exit(1);
  }
  fclose(f);
  *out = (size_t)size;
  return img;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <hf1-flow.bin> <hf3-flow.bin>\n", argv[0]);
    return 1;
  }

  size_t n1, n3;
  uint8_t *hf1 = slurp(argv[1], &n1);
  uint8_t *hf3 = slurp(argv[2], &n3);
  tas57xx_cram_sink_t s1 = {.image = hf1, .image_size = n1};
  tas57xx_cram_sink_t s3 = {.image = hf3, .image_size = n3};

  puts("HF1 (words 209/210)");
  tas57xx_hf1_set_pbe_enabled(&s1, true);
  check("engaged", &s1, 209, 0x7FFFFF, 0);
  tas57xx_hf1_set_pbe_enabled(&s1, false);
  check("bypassed", &s1, 209, 0, 0x7FFFFF);

  puts("HF3 (words 227/228)");
  tas57xx_hf3_set_pbe_enabled(&s3, true);
  check("engaged", &s3, 227, 0x7FFFFF, 0);
  tas57xx_hf3_set_pbe_enabled(&s3, false);
  check("bypassed", &s3, 227, 0, 0x7FFFFF);

  printf("%s\n", fails ? "FAILED" : "all match the PPC2 captures");
  return fails ? 1 : 0;
}
