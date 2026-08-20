/* Compares the HF3 encoders against a capture from the tuning tool.
 *
 * Reads a PurePath Console I2C log, replays it to recover the coefficient RAM
 * the tool left behind, then encodes the same settings ourselves and reports
 * where the two disagree. Any word listed under a block we claim to drive is a
 * genuine bug; words outside the map are just the parts of the flow we do not
 * tune.
 *
 * usage: hf3test <capture.cfg> [--pbe hpf effect harmonic]
 *                 [--sense lower upper] [--window ms] */
#include "tas57xx_hf3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS TAS57XX_CRAM_WORD_COUNT

/* ------------------------------------------------------------------ */
/* Replay a "w <addr> <reg> <bytes...>" log into bank A word values.   */

static int want[WORDS];
static int seen[WORDS];

static void replay(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(1);
  }
  char line[4096];
  int page = -1;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] != 'w') {
      continue;
    }
    unsigned byte[512];
    int n = 0;
    for (char *p = line + 1; *p && n < (int)(sizeof(byte) / sizeof(byte[0]));) {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
      }
      if (!*p) {
        break;
      }
      char *end;
      unsigned long v = strtoul(p, &end, 16);
      if (end == p) {
        break;
      }
      byte[n++] = (unsigned)v;
      p = end;
    }
    /* byte[0] is the device address; only the first DAC is decoded. */
    if (n < 3 || byte[0] != 0x98) {
      continue;
    }
    const int reg = (int)(byte[1] & 0x7F);
    const int len = n - 2;
    if (reg == 0x00 && len == 1) {
      page = (int)byte[2];
      continue;
    }
    if (page < 0x2C || reg < 0x08) {
      continue;
    }
    /* Bank B mirrors bank A; both land on the same word. */
    const int base = page >= 0x3E ? page - 0x12 : page;
    if (base > 0x34) {
      continue;
    }
    int word = (base - 0x2C) * TAS57XX_CRAM_WORDS_PER_PAGE + (reg - 0x08) / 4;
    for (int off = 0; off + 4 <= len; off += 4, word++) {
      if (word < 0 || word >= WORDS) {
        break;
      }
      const unsigned *p = &byte[2 + off];
      int v = (int)((p[0] << 16) | (p[1] << 8) | p[2]);
      if (v & 0x800000) {
        v -= 0x1000000;
      }
      want[word] = v;
      seen[word] = 1;
    }
  }
  fclose(f);
}

/* ------------------------------------------------------------------ */
/* A flow image covering the whole of bank A, so the encoders have     */
/* somewhere to write and the result can be read straight back out.    */

#define PAGE_BYTES (2 + 1 + 2 + TAS57XX_CRAM_WORDS_PER_PAGE * 4)
static uint8_t img[TAS57XX_CRAM_PAGES * PAGE_BYTES + 2];

static void image_init(void) {
  size_t pos = 0;
  for (int p = 0; p < TAS57XX_CRAM_PAGES; p++) {
    img[pos++] = 0x00;
    img[pos++] = 0x01;
    img[pos++] = (uint8_t)(0x2C + p);
    img[pos++] = 0x08;
    img[pos++] = TAS57XX_CRAM_WORDS_PER_PAGE * 4;
    memset(&img[pos], 0, TAS57XX_CRAM_WORDS_PER_PAGE * 4);
    pos += TAS57XX_CRAM_WORDS_PER_PAGE * 4;
  }
  img[pos++] = 0xFF;
  img[pos++] = 0xFF;
}

static int image_word(int word) {
  const size_t pos = (size_t)(word / TAS57XX_CRAM_WORDS_PER_PAGE) * PAGE_BYTES +
                     5 + (size_t)(word % TAS57XX_CRAM_WORDS_PER_PAGE) * 4;
  int v = (int)(((unsigned)img[pos] << 16) | ((unsigned)img[pos + 1] << 8) |
                img[pos + 2]);
  if (v & 0x800000) {
    v -= 0x1000000;
  }
  return v;
}

/* ------------------------------------------------------------------ */

static const struct {
  const char *name;
  int first;
  int count;
  int layout; /**< -1 if the block is not a biquad */
} block[] = {
    {"PBE harmonic", 2, 1, -1},
    {"PBE extract HP", 3, 5, TAS57XX_BQ_DEN_FIRST},
    {"PBE effect shelf", 8, 5, TAS57XX_BQ_DEN_FIRST},
    {"PBE extract LP1", 13, 5, TAS57XX_BQ_DEN_FIRST},
    {"PBE extract LP2", 18, 5, TAS57XX_BQ_DEN_FIRST},
    {"PBE extract LP3", 23, 5, TAS57XX_BQ_DEN_FIRST},
    {"DRC split high", 28, 5, TAS57XX_BQ_NUM_FIRST},
    {"DRC split mid", 33, 5, TAS57XX_BQ_NUM_FIRST},
    {"DRC gain mid", 38, 1, -1},
    {"DRC gain high", 39, 1, -1},
    {"low crossover", 43, 5, TAS57XX_BQ_NUM_FIRST},
    {"low EQ 1", 48, 5, TAS57XX_BQ_NUM_FIRST},
    {"low EQ 2", 53, 5, TAS57XX_BQ_NUM_FIRST},
    {"low EQ 3", 58, 5, TAS57XX_BQ_NUM_FIRST},
    {"low EQ 4", 63, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE mix", 69, 2, -1},
    {"DBE energy window", 71, 1, -1},
    {"high crossover", 72, 5, TAS57XX_BQ_NUM_FIRST},
    {"high EQ 1", 77, 5, TAS57XX_BQ_NUM_FIRST},
    {"high EQ 2", 82, 5, TAS57XX_BQ_NUM_FIRST},
    {"high EQ 3", 87, 5, TAS57XX_BQ_NUM_FIRST},
    {"high EQ 4", 92, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE sensing", 98, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE high-level EQ 1", 108, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE high-level EQ 2", 113, 5, TAS57XX_BQ_NUM_FIRST},
    {"DRC timing low", 118, 6, -1},
    {"DRC timing mid", 124, 6, -1},
    {"DRC timing high", 130, 6, -1},
    {"DRC curve", 136, 5, -1},
    {"delay taps", 196, 5, -1},
    {"high input mix", 201, 2, -1},
    {"low input mix", 206, 2, -1},
    {"DBE low-level EQ 1", 212, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE low-level EQ 2", 217, 5, TAS57XX_BQ_NUM_FIRST},
    {"DBE low-level EQ 3", 222, 5, TAS57XX_BQ_NUM_FIRST},
    {"PBE bypass", 227, 2, -1},
    {"smooth clip", 251, 2, -1},
};

/** Undo a slot's packing into b0, b1, b2, a1, a2. */
static void unpack(const int *w, int layout, double c[5]) {
  const double s = 8388607.0;
  if (layout == TAS57XX_BQ_DEN_FIRST) {
    c[0] = 2 * w[2] / s;
    c[1] = 4 * w[3] / s;
    c[2] = 2 * w[4] / s;
    c[3] = -2 * w[0] / s;
    c[4] = -w[1] / s;
  } else {
    c[0] = w[0] / s;
    c[1] = 2 * w[1] / s;
    c[2] = w[2] / s;
    c[3] = -2 * w[3] / s;
    c[4] = -w[4] / s;
  }
}

static double response_db(const double c[5], double f, double fs) {
  const double t = -2.0 * M_PI * f / fs;
  const double cr = cos(t), ci = sin(t);
  const double c2r = cos(2 * t), c2i = sin(2 * t);
  const double nr = c[0] + c[1] * cr + c[2] * c2r;
  const double ni = c[1] * ci + c[2] * c2i;
  const double dr = 1.0 + c[3] * cr + c[4] * c2r;
  const double di = c[3] * ci + c[4] * c2i;
  const double n = sqrt(nr * nr + ni * ni), d = sqrt(dr * dr + di * di);
  return 20.0 * log10((n + 1e-30) / (d + 1e-30));
}

/** Worst disagreement in dB across the audio band, or -1 if not comparable. */
static double worst_db(int first, int layout, double fs) {
  int a[5], b[5];
  for (int i = 0; i < 5; i++) {
    a[i] = want[first + i];
    b[i] = image_word(first + i);
  }
  double ca[5], cb[5];
  unpack(a, layout, ca);
  unpack(b, layout, cb);
  double worst = 0.0;
  for (int i = 0; i <= 200; i++) {
    const double f = 20.0 * pow(1000.0, i / 200.0);
    if (f >= fs / 2) {
      break;
    }
    const double d = response_db(ca, f, fs) - response_db(cb, f, fs);
    /* Ignore the depths of a notch, where a hair of detuning is huge in dB. */
    if (response_db(ca, f, fs) < -60.0) {
      continue;
    }
    if (fabs(d) > worst) {
      worst = fabs(d);
    }
  }
  return worst;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <capture.cfg> [--pbe hpf effect harmonic]"
            " [--sense lower upper] [--window ms]\n",
            argv[0]);
    return 1;
  }
  replay(argv[1]);
  image_init();

  tas57xx_hf3_config_t cfg;
  tas57xx_hf3_defaults(&cfg);
  /* A capture that moved one control needs that control's settings named. */
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--pbe") == 0 && i + 3 < argc) {
      cfg.pbe.hpf_hz = (float)atof(argv[i + 1]);
      cfg.pbe.effect = atoi(argv[i + 2]);
      cfg.pbe.harmonic = atoi(argv[i + 3]);
      i += 3;
    } else if (strcmp(argv[i], "--sense") == 0 && i + 2 < argc) {
      cfg.sense_lower_hz = (float)atof(argv[i + 1]);
      cfg.sense_upper_hz = (float)atof(argv[i + 2]);
      i += 2;
    } else if (strcmp(argv[i], "--dbe") == 0 && i + 2 < argc) {
      cfg.dbe_lower_db = (float)atof(argv[i + 1]);
      cfg.dbe_upper_db = (float)atof(argv[i + 2]);
      i += 2;
    } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
      cfg.sense_window_ms = (float)atof(argv[i + 1]);
      i += 1;
    } else {
      fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
      return 1;
    }
  }

  tas57xx_cram_sink_t sink = {.image = img, .image_size = sizeof(img)};
  esp_err_t err = tas57xx_hf3_apply(&sink, &cfg);
  if (err != ESP_OK) {
    printf("apply failed: %d\n", (int)err);
    return 1;
  }

  int bad = 0;
  for (size_t b = 0; b < sizeof(block) / sizeof(block[0]); b++) {
    int worst = 0, at = -1, missing = 0;
    for (int i = 0; i < block[b].count; i++) {
      const int w = block[b].first + i;
      if (!seen[w]) {
        missing++;
        continue;
      }
      const int d = image_word(w) - want[w];
      if (d > worst || -d > worst) {
        worst = d < 0 ? -d : d;
        at = w;
      }
    }
    if (missing == block[b].count) {
      continue;
    }
    if (worst == 0) {
      printf("  ok   %-22s words %3d..%-3d  exact\n", block[b].name,
             block[b].first, block[b].first + block[b].count - 1);
      continue;
    }
    if (block[b].layout >= 0) {
      const double db =
          worst_db(block[b].first, block[b].layout, (double)cfg.sample_rate_hz);
      /* A section whose poles sit near DC swings wildly on the last bit, so a
       * handful of LSB is the floor rather than a disagreement. */
      if (db < 0.001 || worst <= 8) {
        printf("  ok   %-22s words %3d..%-3d  %d LSB, %.6f dB\n", block[b].name,
               block[b].first, block[b].first + block[b].count - 1, worst, db);
        continue;
      }
      bad++;
      printf("  DIFF %-22s %d LSB at word %d, %.4f dB (want %9d, got %9d)\n",
             block[b].name, worst, at, db, want[at], image_word(at));
      continue;
    }
    /* Scalars have no pole to amplify the last bit, so only rounding. */
    if (worst <= 1) {
      printf("  ok   %-22s words %3d..%-3d  %d LSB\n", block[b].name,
             block[b].first, block[b].first + block[b].count - 1, worst);
      continue;
    }
    bad++;
    printf("  DIFF %-22s %+9d at word %d (want %9d, got %9d)\n", block[b].name,
           image_word(at) - want[at], at, want[at], image_word(at));
    (void)missing;
  }

  /* Anything the capture wrote that we never touch is still unexplained. */
  printf("\nwords written by the tool but outside our map:");
  int loose = 0;
  for (int w = 0; w < WORDS; w++) {
    if (!seen[w] || want[w] == 0) {
      continue;
    }
    int inside = 0;
    for (size_t b = 0; b < sizeof(block) / sizeof(block[0]); b++) {
      if (w >= block[b].first && w < block[b].first + block[b].count) {
        inside = 1;
        break;
      }
    }
    if (!inside) {
      printf(" %d", w);
      loose++;
    }
  }
  printf("%s\n", loose ? "" : " (none)");
  printf("\n%d blocks match, %d differ\n",
         (int)(sizeof(block) / sizeof(block[0])) - bad, bad);
  return bad ? 1 : 0;
}
