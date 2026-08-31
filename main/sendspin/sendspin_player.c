#include "sendspin_player.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_engine_v2.h"
#include "audio_output.h"
#include "audio_receiver.h"

static const char *TAG = "sendspin_pl";

/* Timeline block length. Shorter than the AAC block the AirPlay path uses:
 * Sendspin chunks are timestamped individually and can be as short as 15 ms,
 * so a finer block keeps the re-cut below cheap and bounds how much audio a
 * partial block holds back at the end of a stream. */
#define SENDSPIN_FRAME_SAMPLES 512U

/* Reported to the server as min_buffer_ms. It has to clear the engine's
 * 180 ms preroll with room for the chunk period on top, or the server aims
 * its lead time at a buffer depth the scheduler will not start from. */
#define SENDSPIN_MIN_BUFFER_MS 300U

#define SENDSPIN_STATUS_PERIOD_US 10000000LL

static audio_engine_v2_t s_engine;
static bool s_engine_ready = false;
static const sendspin_time_t *s_clock = NULL;

static bool s_streaming = false;
static uint32_t s_epoch = 0;
static uint32_t s_sample_rate = 44100;
static uint8_t s_src_channels = 2;
static uint8_t s_bit_depth = 16;
static uint8_t s_bytes_per_frame = 4;

/* Chunk timestamps are absolute server microseconds; the clock map wants RTP
 * sample numbers. Anchor on the first chunk of the stream and derive every
 * later chunk from its own timestamp, so a gap in the stream lands at the
 * right place instead of being packed against the previous chunk. */
static bool s_anchor_valid = false;
static int64_t s_anchor_server_us = 0;
static uint32_t s_anchor_rtp = 0;

/* Partial block waiting to be pushed. Blocks must start on a multiple of
 * frame_samples from the timeline's base, so chunk boundaries cannot be
 * block boundaries and the tail of every chunk is carried here. */
static int16_t *s_fill = NULL;
static uint32_t s_fill_frames = 0;
static uint32_t s_block_rtp = 0;
static uint32_t s_next_rtp = 0;

static uint64_t s_chunks_received = 0;
static uint64_t s_chunks_dropped = 0;
static uint64_t s_frames_pushed = 0;
static int64_t s_last_status_us = 0;

static size_t sendspin_player_read(int16_t *buffer, size_t samples);

uint32_t sendspin_player_buffer_capacity(void) {
  /* Bytes of *compressed* audio the server may keep queued on us, so quote
   * the timeline in bytes at the widest format we accept -- but only a third
   * of it. The server fills right up to whatever it is told, so the rest has
   * to absorb the preroll and the jitter; quote the whole ring and every
   * transient overflows, and the overflow is dropped and comes back as a
   * hole to conceal. The margin has to cover compression too: bytes stop
   * bounding *duration* once FLAC is on the wire, and the server's own
   * duration cap is 30 s, which is no help at all. */
  const uint32_t frames =
      ((uint32_t)CONFIG_SENDSPIN_TIMELINE_BLOCKS / 3U) * SENDSPIN_FRAME_SAMPLES;
  return frames * 2U * (uint32_t)sizeof(int16_t);
}

uint32_t sendspin_player_min_buffer_ms(void) {
  return SENDSPIN_MIN_BUFFER_MS;
}

esp_err_t sendspin_player_init(const sendspin_time_t *clock) {
  if (s_engine_ready) {
    return ESP_OK;
  }
  if (!clock) {
    return ESP_ERR_INVALID_ARG;
  }

  s_fill = heap_caps_calloc(SENDSPIN_FRAME_SAMPLES * 2U, sizeof(int16_t),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_fill) {
    return ESP_ERR_NO_MEM;
  }

  const audio_format_t format = {
      .codec = "pcm",
      .sample_rate = 44100,
      .channels = 2,
      .bits_per_sample = 16,
      .frame_size = (int)SENDSPIN_FRAME_SAMPLES,
  };
  esp_err_t err =
      audio_engine_v2_init(&s_engine, &format, SENDSPIN_FRAME_SAMPLES,
                           (uint16_t)CONFIG_SENDSPIN_TIMELINE_BLOCKS);
  if (err != ESP_OK) {
    free(s_fill);
    s_fill = NULL;
    return err;
  }

  s_clock = clock;
  s_engine_ready = true;
  ESP_LOGI(TAG, "timeline ready: %u blocks x %u frames (%u KB, %u ms)",
           (unsigned)CONFIG_SENDSPIN_TIMELINE_BLOCKS,
           (unsigned)SENDSPIN_FRAME_SAMPLES,
           (unsigned)(sendspin_player_buffer_capacity() / 1024U),
           (unsigned)((uint64_t)CONFIG_SENDSPIN_TIMELINE_BLOCKS *
                      SENDSPIN_FRAME_SAMPLES * 1000U / 44100U));
  return ESP_OK;
}

void sendspin_player_deinit(void) {
  if (!s_engine_ready) {
    return;
  }
  sendspin_player_stream_end();
  audio_engine_v2_deinit(&s_engine);
  free(s_fill);
  s_fill = NULL;
  s_engine_ready = false;
  s_clock = NULL;
}

bool sendspin_player_is_streaming(void) {
  return s_streaming;
}

/* ------------------------------------------------------------------ */
/*  Stream lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void sendspin_player_reset_alignment(void) {
  s_anchor_valid = false;
  s_anchor_server_us = 0;
  s_anchor_rtp = 0;
  s_fill_frames = 0;
  s_block_rtp = 0;
  s_next_rtp = 0;
}

esp_err_t sendspin_player_stream_start(const sendspin_player_format_t *format) {
  if (!s_engine_ready || !format) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!format->pcm) {
    ESP_LOGW(TAG, "only pcm is implemented in this build");
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (format->channels < 1 || format->channels > 2 ||
      (format->bit_depth != 16 && format->bit_depth != 24) ||
      format->sample_rate < 8000 || format->sample_rate > 192000) {
    ESP_LOGW(TAG, "unsupported pcm format: %" PRIu32 " Hz, %u ch, %u bit",
             format->sample_rate, (unsigned)format->channels,
             (unsigned)format->bit_depth);
    return ESP_ERR_NOT_SUPPORTED;
  }

  s_sample_rate = format->sample_rate;
  s_src_channels = format->channels;
  s_bit_depth = format->bit_depth;
  s_bytes_per_frame =
      (uint8_t)((format->bit_depth / 8U) * (uint32_t)format->channels);

  const audio_format_t engine_format = {
      .codec = "pcm",
      .sample_rate = (int)format->sample_rate,
      /* Mono is widened on the way in: the timeline slot is sized for two
       * channels regardless, and the output stage is stereo. */
      .channels = 2,
      .bits_per_sample = 16,
      .frame_size = (int)SENDSPIN_FRAME_SAMPLES,
  };
  audio_engine_v2_set_format(&s_engine, &engine_format);
  audio_output_set_source_rate((int)format->sample_rate);

  sendspin_player_reset_alignment();
  s_epoch = audio_engine_v2_begin_epoch(&s_engine, esp_timer_get_time());
  audio_engine_v2_wait_for_anchor(&s_engine, esp_timer_get_time());
  audio_engine_v2_set_playing(&s_engine, true);

  s_chunks_received = 0;
  s_chunks_dropped = 0;
  s_frames_pushed = 0;
  s_last_status_us = esp_timer_get_time();

  s_streaming = true;
  audio_output_set_source(sendspin_player_read);
  audio_output_start();

  ESP_LOGI(TAG, "stream start: %" PRIu32 " Hz, %u ch, %u bit",
           format->sample_rate, (unsigned)format->channels,
           (unsigned)format->bit_depth);
  return ESP_OK;
}

void sendspin_player_stream_clear(void) {
  if (!s_engine_ready) {
    return;
  }
  /* A seek invalidates the timestamp-to-RTP anchor as well as the content:
   * the next chunk restarts both. */
  sendspin_player_reset_alignment();
  s_epoch = audio_engine_v2_begin_epoch(&s_engine, esp_timer_get_time());
  audio_engine_v2_wait_for_anchor(&s_engine, esp_timer_get_time());
  audio_engine_v2_set_playing(&s_engine, s_streaming);
  ESP_LOGI(TAG, "stream clear");
}

void sendspin_player_stream_end(void) {
  if (!s_streaming) {
    return;
  }
  s_streaming = false;
  /* Drop the render hook before the engine is quiesced, so the playback task
   * cannot observe a half-reset scheduler. */
  audio_output_set_source(NULL);
  audio_engine_v2_set_playing(&s_engine, false);
  sendspin_player_reset_alignment();
  s_epoch = audio_engine_v2_begin_epoch(&s_engine, esp_timer_get_time());
  audio_output_stop();
  ESP_LOGI(TAG,
           "stream end: %" PRIu64 " chunks, %" PRIu64 " frames, %" PRIu64
           " dropped",
           s_chunks_received, s_frames_pushed, s_chunks_dropped);
}

/* ------------------------------------------------------------------ */
/*  Chunk ingest                                                       */
/* ------------------------------------------------------------------ */

static inline int16_t sendspin_sample_at(const uint8_t *p, uint8_t bit_depth) {
  if (bit_depth == 16) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }
  /* 24-bit little-endian packed, truncated to the engine's 16-bit slots. */
  return (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
}

/* Push the held block. Returns false if the timeline had no room, which is
 * the normal back-pressure signal and costs one block of audio. */
static bool sendspin_player_flush_block(void) {
  if (s_fill_frames == 0) {
    return true;
  }
  const bool ok = audio_engine_v2_push_pcm(&s_engine, s_epoch, s_block_rtp,
                                           s_fill, s_fill_frames, 2);
  if (ok) {
    s_frames_pushed += s_fill_frames;
  } else {
    s_chunks_dropped++;
  }
  s_block_rtp += SENDSPIN_FRAME_SAMPLES;
  s_fill_frames = 0;
  return ok;
}

void sendspin_player_chunk(int64_t timestamp_us, const uint8_t *data,
                           size_t len) {
  if (!s_engine_ready || !s_streaming || !data || len < s_bytes_per_frame) {
    return;
  }

  s_chunks_received++;
  const uint32_t frames = (uint32_t)(len / s_bytes_per_frame);

  if (!s_anchor_valid) {
    s_anchor_server_us = timestamp_us;
    s_anchor_rtp = 0;
    s_next_rtp = 0;
    s_block_rtp = 0;
    s_fill_frames = 0;
    s_anchor_valid = true;
    if (!audio_engine_v2_set_anchor(&s_engine, s_anchor_rtp,
                                    (uint64_t)(timestamp_us * 1000LL), 0)) {
      ESP_LOGW(TAG, "anchor rejected at ts=%" PRId64, timestamp_us);
      s_anchor_valid = false;
      return;
    }
    ESP_LOGI(TAG, "anchored at server ts=%" PRId64 " us", timestamp_us);
  } else {
    /* Where this chunk claims to sit, from its own timestamp. */
    const int64_t delta_us = timestamp_us - s_anchor_server_us;
    const int64_t chunk_rtp64 =
        (int64_t)s_anchor_rtp +
        ((delta_us * (int64_t)s_sample_rate) / 1000000LL);
    const int32_t gap = (int32_t)((uint32_t)chunk_rtp64 - s_next_rtp);

    /* Continuous audio lands within a sample of the running position; the
     * mismatch is only the microsecond quantisation of the timestamp. A real
     * discontinuity means the server jumped, and the clock map has to be
     * re-anchored because RTP is no longer a continuous function of it. */
    const int32_t tolerance = (int32_t)(s_sample_rate / 1000U);
    if (gap > tolerance || gap < -tolerance) {
      ESP_LOGI(TAG, "discontinuity of %" PRId32 " samples — re-anchoring", gap);
      sendspin_player_reset_alignment();
      s_epoch = audio_engine_v2_begin_epoch(&s_engine, esp_timer_get_time());
      audio_engine_v2_wait_for_anchor(&s_engine, esp_timer_get_time());
      audio_engine_v2_set_playing(&s_engine, true);
      sendspin_player_chunk(timestamp_us, data, len);
      return;
    }
  }

  const uint8_t sample_bytes = (uint8_t)(s_bit_depth / 8U);
  const uint8_t *p = data;
  for (uint32_t i = 0; i < frames; i++) {
    int16_t l = sendspin_sample_at(p, s_bit_depth);
    int16_t r = l; /* mono is widened by duplicating the sample */
    if (s_src_channels == 2) {
      r = sendspin_sample_at(p + sample_bytes, s_bit_depth);
    }
    p += s_bytes_per_frame;

    s_fill[s_fill_frames * 2U] = l;
    s_fill[(s_fill_frames * 2U) + 1U] = r;
    s_fill_frames++;
    if (s_fill_frames == SENDSPIN_FRAME_SAMPLES) {
      (void)sendspin_player_flush_block();
    }
  }

  s_next_rtp += frames;
  sendspin_player_log_status();
}

/* ------------------------------------------------------------------ */
/*  Render                                                             */
/* ------------------------------------------------------------------ */

static size_t sendspin_player_read(int16_t *buffer, size_t samples) {
  if (!s_engine_ready || !s_streaming || !buffer || samples == 0) {
    return 0;
  }

  const int64_t now_us = esp_timer_get_time();
  const int64_t playout_local_ns =
      audio_output_get_next_playout_time_ns(now_us);

  int64_t offset_ns = 0;
  if (!sendspin_time_offset_ns(s_clock, playout_local_ns / 1000LL,
                               &offset_ns)) {
    /* No usable clock estimate yet. Silence is the honest answer: the
     * scheduler cannot place this block on the server's timeline. */
    return 0;
  }

  return audio_engine_v2_render(&s_engine, playout_local_ns + offset_ns, buffer,
                                samples);
}

/* ------------------------------------------------------------------ */
/*  Diagnostics                                                        */
/* ------------------------------------------------------------------ */

void sendspin_player_log_status(void) {
  if (!s_engine_ready) {
    return;
  }
  const int64_t now_us = esp_timer_get_time();
  if (now_us - s_last_status_us < SENDSPIN_STATUS_PERIOD_US) {
    return;
  }
  s_last_status_us = now_us;

  const size_t blocks = audio_timeline_count(&s_engine.timeline);
  ESP_LOGI(TAG,
           "%s buf=%u blk (%u ms) err=%" PRId32 " smp drift=%" PRId32
           " ppm rtt=%" PRId64 " us skew=%" PRId32 " ppm drops=%" PRIu64,
           audio_scheduler_state_name(s_engine.scheduler.state),
           (unsigned)blocks,
           (unsigned)((uint64_t)blocks * SENDSPIN_FRAME_SAMPLES * 1000U /
                      (s_sample_rate ? s_sample_rate : 44100U)),
           s_engine.scheduler.playout_error_samples,
           s_engine.scheduler.estimated_drift_ppm,
           sendspin_time_best_rtt_us(s_clock), sendspin_time_skew_ppm(s_clock),
           s_chunks_dropped);
}
