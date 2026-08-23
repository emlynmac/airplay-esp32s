#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#define AUDIO_V2_MAX_CHANNELS 2
/* I2S render quantum. This is not a storage-block size. */
#define AUDIO_V2_BLOCK_SAMPLES 352
/* AirPlay 2 buffered AAC: one decoded AAC frame is exactly 1024 samples. */
#define AUDIO_PCM_BLOCK_MAX_SAMPLES  1024
#define AUDIO_TIMELINE_FRAME_SAMPLES 1024U
#define AUDIO_TIMELINE_FRAME_MASK    (AUDIO_TIMELINE_FRAME_SAMPLES - 1U)
/* ~4.46 s at 44.1 kHz for 192 AAC frames. */
#define AUDIO_V2_TIMELINE_BLOCKS 192

/* Slot metadata, kept separate from the PCM payload.
 *
 * Every lookup, contiguity scan and diagnostic pass walks descriptors only.
 * Interleaving them with the 4 KB PCM payloads turns a 192-entry scan into 192
 * PSRAM cache misses while a spinlock holds interrupts off on both cores.
 * Descriptors live in internal RAM and are 12 bytes each, so a full scan stays
 * within a few microseconds. */
typedef struct {
  /* READY/public slot visible to timeline readers. */
  bool used;
  /* Slot reserved by a producer while PCM is copied outside timeline.lock. */
  bool writing;
  /* Slot borrowed by the consumer while PCM is copied outside timeline.lock. */
  bool reading;
  uint32_t epoch;
  uint32_t rtp_start;
} audio_timeline_desc_t;

typedef struct {
  size_t count;
  bool has_oldest;
  bool has_newest;
  bool target_covered;
  bool has_next;
  uint32_t oldest_rtp;
  uint32_t newest_start_rtp;
  uint32_t newest_end_rtp;
  uint32_t covering_start_rtp;
  uint32_t covering_end_rtp;
  uint32_t next_rtp;
  uint32_t contiguous_samples;
} audio_timeline_diag_t;

typedef struct {
  /* Descriptors in internal RAM, PCM payloads in PSRAM. */
  audio_timeline_desc_t *desc;
  int16_t *pcm_pool;
  uint16_t count;
  uint16_t writing_count;
  uint16_t capacity;

  /* RTP origin for direct circular addressing.  Frame starts are 1024 samples
   * apart but may have any low-10-bit phase.  Addressing is relative to this
   * origin so the ring also remains sequential across 32-bit RTP wrap. */
  bool base_valid;
  uint32_t base_rtp;
  uint32_t base_epoch;

  /* O(1) reclamation watermark set when scheduler chooses a new read head.
   * READY blocks ending at/before this RTP may be recycled on ring collision.
   */
  bool playback_floor_valid;
  uint32_t playback_floor_rtp;
  uint32_t playback_floor_epoch;

  /* Signalled by the consumer whenever a slot is released, so producers can
   * block instead of polling for space. */
  SemaphoreHandle_t space_available;

  portMUX_TYPE lock;
} audio_timeline_t;

/* Opaque producer reservation used to keep a slot private while PCM is copied
 * outside both the timeline metadata lock and the engine publish lock. */
typedef struct {
  uint16_t slot_index;
  bool valid;
  /* Existing READY block with the same epoch/RTP; no copy required. */
  bool duplicate;
} audio_timeline_reservation_t;

esp_err_t audio_timeline_init(audio_timeline_t *timeline, uint16_t capacity);
void audio_timeline_deinit(audio_timeline_t *timeline);
void audio_timeline_clear(audio_timeline_t *timeline);
size_t audio_timeline_count(audio_timeline_t *timeline);

/* Drop every READY block of `epoch` that extends past `rtp`, keeping earlier
 * blocks and the RTP origin intact.  This is the AirPlay 2 deferred
 * FLUSHBUFFERED primitive: the outgoing track plays out to the cut point while
 * the replacement content reuses the freed slots.  Returns blocks dropped. */
size_t audio_timeline_discard_from(audio_timeline_t *timeline, uint32_t epoch,
                                   uint32_t rtp);

/* Inverse of the above: drop every READY block of `epoch` that ends at or
 * before `rtp`, i.e. audio the playout position has already passed.  Returns
 * blocks dropped. */
size_t audio_timeline_trim_before(audio_timeline_t *timeline, uint32_t epoch,
                                  uint32_t rtp);

size_t audio_timeline_free_slots(audio_timeline_t *timeline);
bool audio_timeline_is_nearly_full(audio_timeline_t *timeline);
void audio_timeline_set_playback_floor(audio_timeline_t *timeline,
                                       uint32_t epoch, uint32_t floor_rtp);

/* Block until the consumer releases a slot or the timeout expires.  Returns
 * true if space may now be available. */
bool audio_timeline_wait_for_space(audio_timeline_t *timeline,
                                   uint32_t timeout_ms);

bool audio_timeline_reserve(audio_timeline_t *timeline, uint32_t epoch,
                            uint32_t rtp_start,
                            audio_timeline_reservation_t *reservation,
                            int16_t **pcm_out);
bool audio_timeline_commit(audio_timeline_t *timeline,
                           audio_timeline_reservation_t *reservation);
void audio_timeline_cancel(audio_timeline_t *timeline,
                           audio_timeline_reservation_t *reservation);

bool audio_timeline_find_contiguous_from(audio_timeline_t *timeline,
                                         uint32_t epoch, uint32_t target_rtp,
                                         uint32_t required_samples,
                                         uint32_t max_future_samples,
                                         uint32_t *start_rtp);
bool audio_timeline_has_playable_from(audio_timeline_t *timeline,
                                      uint32_t epoch, uint32_t target_rtp);
size_t audio_timeline_read(audio_timeline_t *timeline, uint32_t epoch,
                           uint32_t start_rtp, int16_t *out,
                           size_t requested_samples, uint8_t channels,
                           bool conceal_missing, size_t *concealed_samples);

bool audio_timeline_get_diag(audio_timeline_t *timeline, uint32_t epoch,
                             uint32_t target_rtp,
                             audio_timeline_diag_t *diag_out);
