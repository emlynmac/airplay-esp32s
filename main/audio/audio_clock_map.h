#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool valid;
  uint32_t sample_rate;
  uint32_t anchor_rtp;
  uint64_t anchor_ptp_ns;
  int64_t playout_offset_ns;
} audio_clock_map_t;

void audio_clock_map_reset(audio_clock_map_t *map);
bool audio_clock_map_set(audio_clock_map_t *map, uint32_t sample_rate,
                         uint32_t anchor_rtp, uint64_t anchor_ptp_ns,
                         int64_t playout_offset_ns);
bool audio_clock_map_rtp_to_ptp(const audio_clock_map_t *map, uint32_t rtp,
                                int64_t *ptp_ns);
bool audio_clock_map_ptp_to_rtp(const audio_clock_map_t *map, int64_t ptp_ns,
                                uint32_t *rtp);
