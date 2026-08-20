/*
 * Round-trip check for tas57xx_hf1_read().
 *
 * usage: hf1read <flow.bin>
 *
 * Reads the tuning out of a flow image, writes it straight back into a copy of
 * that image, and reports any word that changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tas57xx_cram.h"
#include "tas57xx_hf1.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <flow.bin>\n", argv[0]);
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *img = malloc((size_t)size);
  if (fread(img, 1, (size_t)size, f) != (size_t)size) {
    return 1;
  }
  fclose(f);

  tas57xx_hf1_config_t cfg;
  if (tas57xx_hf1_read(img, (size_t)size, 44100, &cfg) != ESP_OK) {
    fprintf(stderr, "read failed\n");
    return 1;
  }

  printf("recovered tuning from %s\n", argv[1]);
  for (int i = 0; i < TAS57XX_HF1_EQ_BANDS; i++) {
    printf("  eq%-2d         f0 %8.2f Hz  Q %8.4f\n", i + 1, cfg.eq[i].freq_hz,
           cfg.eq[i].q);
  }
  printf("  pbe          %.2f Hz  effect %d  harmonic %d  %s\n", cfg.pbe.hpf_hz,
         cfg.pbe.effect, cfg.pbe.harmonic,
         cfg.pbe_enabled ? "enabled" : "bypassed");
  for (int i = 0; i < TAS57XX_HF1_DBE_EQ_BANDS; i++) {
    printf("  dbe hi%d      f0 %8.2f Hz  Q %8.4f\n", i + 1,
           cfg.dbe_high[i].freq_hz, cfg.dbe_high[i].q);
    printf("  dbe lo%d      f0 %8.2f Hz  Q %8.4f\n", i + 1,
           cfg.dbe_low[i].freq_hz, cfg.dbe_low[i].q);
  }
  printf("  dbe mix      %.2f .. %.2f dB\n", cfg.dbe_lower_db,
         cfg.dbe_upper_db);
  printf("  sensing      %.2f .. %.2f Hz  window %.2f ms\n", cfg.sense_lower_hz,
         cfg.sense_upper_hz, cfg.sense_window_ms);
  for (int i = 0; i < TAS57XX_HF1_DRC_CROSS_SECTIONS; i++) {
    printf("  drc xover%d   f0 %8.2f Hz  Q %8.4f\n", i,
           cfg.drc_cross[i].freq_hz, cfg.drc_cross[i].q);
  }
  printf("  drc mix      %.4f %.4f %.4f\n", cfg.drc_mix[0], cfg.drc_mix[1],
         cfg.drc_mix[2]);
  for (int b = 0; b < TAS57XX_HF1_DRC_BANDS; b++) {
    printf("  drc band%d    energy %8.2f  attack %8.2f  decay %8.2f ms\n", b,
           cfg.drc_timing[b].energy_ms, cfg.drc_timing[b].attack_ms,
           cfg.drc_timing[b].decay_ms);
  }
  for (int r = 0; r < TAS57XX_HF1_DRC_REGIONS; r++) {
    printf("  drc region%d  %s ratio %.3f\n", r,
           cfg.drc_region[r].mode == TAS57XX_HF1_DRC_EXPAND ? "expand"
                                                            : "compress",
           cfg.drc_region[r].ratio);
  }
  printf("  drc thresh   %.2f / %.2f dB\n", cfg.drc_thresh1_db,
         cfg.drc_thresh2_db);
  printf("  smooth clip  %.2f dB   fine volume %.2f dB\n", cfg.smooth_clip_db,
         cfg.fine_volume_db);

  uint8_t *copy = malloc((size_t)size);
  memcpy(copy, img, (size_t)size);
  tas57xx_cram_sink_t sink = {.image = copy, .image_size = (size_t)size};
  esp_err_t err = tas57xx_hf1_apply(&sink, &cfg);
  if (err != ESP_OK) {
    printf("\napply failed: %d\n", (int)err);
    return 1;
  }

  int worst = 0, moved = 0;
  for (int word = 0; word < 270; word++) {
    int32_t a, b;
    if (tas57xx_cram_read_image(img, (size_t)size, word, &a, 1) != ESP_OK ||
        tas57xx_cram_read_image(copy, (size_t)size, word, &b, 1) != ESP_OK) {
      continue;
    }
    int d = a > b ? a - b : b - a;
    if (d == 0) {
      continue;
    }
    moved++;
    if (d > worst) {
      worst = d;
    }
    if (d > 8) {
      printf("  word %3d  %9d -> %9d  (%d LSB)\n", word, a, b, d);
    }
  }
  printf("\nround trip: %d words moved, worst %d LSB\n", moved, worst);
  free(img);
  free(copy);
  return worst > 8;
}
