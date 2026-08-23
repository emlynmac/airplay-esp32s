#include "audio_scheduler.h"

#include <string.h>

#include "esp_timer.h"

const char *audio_scheduler_state_name(audio_scheduler_state_t state) {
  switch (state) {
  case AUDIO_SCHED_IDLE:
    return "IDLE";
  case AUDIO_SCHED_WAIT_ANCHOR:
    return "WAIT_ANCHOR";
  case AUDIO_SCHED_PREROLL:
    return "PREROLL";
  case AUDIO_SCHED_PLAYING:
    return "PLAYING";
  case AUDIO_SCHED_PAUSED:
    return "PAUSED";
  case AUDIO_SCHED_RECOVERING:
    return "RECOVERING";
  default:
    return "UNKNOWN";
  }
}

const char *
audio_scheduler_wait_reason_name(audio_scheduler_wait_reason_t reason) {
  switch (reason) {
  case AUDIO_SCHED_WAIT_NONE:
    return "NONE";
  case AUDIO_SCHED_WAIT_PAUSED:
    return "PAUSED";
  case AUDIO_SCHED_WAIT_CLOCK_MAP:
    return "CLOCK_MAP";
  case AUDIO_SCHED_WAIT_PTP_TO_RTP:
    return "PTP_TO_RTP";
  case AUDIO_SCHED_WAIT_PREROLL:
    return "PREROLL";
  case AUDIO_SCHED_WAIT_FALLBACK_DATA:
    return "FALLBACK_DATA";
  default:
    return "UNKNOWN";
  }
}

static void output_silence(int16_t *out, size_t samples, uint8_t channels) {
  memset(out, 0, samples * channels * sizeof(int16_t));
}

void audio_scheduler_init(audio_scheduler_t *scheduler,
                          uint32_t preroll_samples, int64_t fallback_after_us) {
  if (!scheduler) {
    return;
  }
  *scheduler = (audio_scheduler_t){
      .state = AUDIO_SCHED_IDLE,
      .preroll_samples = preroll_samples,
      .fallback_after_us = fallback_after_us,
  };
}

void audio_scheduler_begin_epoch(audio_scheduler_t *scheduler, uint32_t epoch,
                                 int64_t now_us) {
  if (!scheduler) {
    return;
  }
  scheduler->state = AUDIO_SCHED_WAIT_ANCHOR;
  scheduler->epoch = epoch;
  scheduler->cursor_rtp = 0;
  scheduler->preroll_started_us = now_us;
  scheduler->wanted_rtp = 0;
  scheduler->raw_playout_error_samples = 0;
  scheduler->playout_error_samples = 0;
  scheduler->filtered_playout_error_q16 = 0;
  scheduler->max_abs_playout_error_samples = 0;
  scheduler->estimated_drift_ppm = 0;
  scheduler->drift_reference_error_q16 = 0;
  scheduler->drift_reference_ptp_ns = 0;
  scheduler->rendered_samples = 0;
  scheduler->error_filter_valid = false;
  scheduler->wait_reason = AUDIO_SCHED_WAIT_CLOCK_MAP;
  scheduler->render_calls = 0;
  scheduler->silent_render_calls = 0;
  scheduler->start_attempts = 0;
  scheduler->fallback_attempts = 0;
}

void audio_scheduler_set_paused(audio_scheduler_t *scheduler, bool paused) {
  if (!scheduler) {
    return;
  }
  scheduler->state = paused ? AUDIO_SCHED_PAUSED : AUDIO_SCHED_PREROLL;
  scheduler->wait_reason =
      paused ? AUDIO_SCHED_WAIT_PAUSED : AUDIO_SCHED_WAIT_PREROLL;
}

size_t audio_scheduler_render(audio_scheduler_t *scheduler,
                              audio_timeline_t *timeline,
                              const audio_clock_map_t *clock_map,
                              int64_t output_ptp_ns, int16_t *out,
                              size_t samples, uint8_t channels,
                              size_t *concealed_samples) {
  if (concealed_samples) {
    *concealed_samples = 0;
  }
  if (!scheduler || !timeline || !clock_map || !out || samples == 0U) {
    return 0;
  }
  scheduler->render_calls++;

  if (scheduler->state == AUDIO_SCHED_PAUSED ||
      scheduler->state == AUDIO_SCHED_IDLE) {
    scheduler->wait_reason = AUDIO_SCHED_WAIT_PAUSED;
    scheduler->silent_render_calls++;
    output_silence(out, samples, channels);
    return samples;
  }

  if (!clock_map->valid) {
    scheduler->state = AUDIO_SCHED_WAIT_ANCHOR;
    scheduler->wait_reason = AUDIO_SCHED_WAIT_CLOCK_MAP;
    scheduler->silent_render_calls++;
    output_silence(out, samples, channels);
    return samples;
  }

  uint32_t wanted_rtp = 0;
  if (!audio_clock_map_ptp_to_rtp(clock_map, output_ptp_ns, &wanted_rtp)) {
    scheduler->wait_reason = AUDIO_SCHED_WAIT_PTP_TO_RTP;
    scheduler->silent_render_calls++;
    output_silence(out, samples, channels);
    return samples;
  }

  scheduler->wanted_rtp = wanted_rtp;
  scheduler->wait_reason = AUDIO_SCHED_WAIT_NONE;
  if (scheduler->state == AUDIO_SCHED_PLAYING) {
    /* Compare the midpoint of the block that is about to be submitted with
     * the RTP position scheduled for that same midpoint.  Measuring only the
     * block start aliases the 352-frame callback cadence into a 0..8 ms
     * sawtooth even when the underlying clock is stable. */
    uint32_t midpoint_samples = (uint32_t)(samples / 2U);
    int64_t midpoint_ptp_ns =
        output_ptp_ns +
        ((int64_t)midpoint_samples * 1000000000LL) / clock_map->sample_rate;
    uint32_t wanted_mid_rtp = wanted_rtp;
    (void)audio_clock_map_ptp_to_rtp(clock_map, midpoint_ptp_ns,
                                     &wanted_mid_rtp);
    uint32_t actual_mid_rtp = scheduler->cursor_rtp + midpoint_samples;
    int32_t raw_error = (int32_t)(actual_mid_rtp - wanted_mid_rtp);
    scheduler->raw_playout_error_samples = raw_error;

    int64_t raw_q16 = (int64_t)raw_error * 65536LL;
    if (!scheduler->error_filter_valid) {
      scheduler->filtered_playout_error_q16 = raw_q16;
      scheduler->error_filter_valid = true;
      scheduler->drift_reference_error_q16 = raw_q16;
      scheduler->drift_reference_ptp_ns = midpoint_ptp_ns;
    } else {
      /* alpha = 1/8: removes callback phase jitter while still following
       * real clock drift within a few hundred milliseconds. */
      scheduler->filtered_playout_error_q16 +=
          (raw_q16 - scheduler->filtered_playout_error_q16) / 8;
    }
    scheduler->playout_error_samples =
        scheduler->filtered_playout_error_q16 >> 16;

    int32_t abs_error = scheduler->playout_error_samples < 0
                            ? -scheduler->playout_error_samples
                            : scheduler->playout_error_samples;
    if (abs_error > scheduler->max_abs_playout_error_samples) {
      scheduler->max_abs_playout_error_samples = abs_error;
    }

    if (scheduler->drift_reference_ptp_ns != 0 &&
        midpoint_ptp_ns - scheduler->drift_reference_ptp_ns >= 1000000000LL) {
      int64_t elapsed_ns = midpoint_ptp_ns - scheduler->drift_reference_ptp_ns;
      int64_t delta_q16 = scheduler->filtered_playout_error_q16 -
                          scheduler->drift_reference_error_q16;
      int64_t delta_samples = delta_q16 / 65536LL;
      int64_t elapsed_samples =
          (elapsed_ns * (int64_t)clock_map->sample_rate) / 1000000000LL;
      if (elapsed_samples > 0) {
        int64_t ppm = (delta_samples * 1000000LL) / elapsed_samples;
        if (ppm > 20000)
          ppm = 20000;
        if (ppm < -20000)
          ppm = -20000;
        scheduler->estimated_drift_ppm = (int32_t)ppm;
      }
      scheduler->drift_reference_error_q16 =
          scheduler->filtered_playout_error_q16;
      scheduler->drift_reference_ptp_ns = midpoint_ptp_ns;
    }
  }

  if (scheduler->state != AUDIO_SCHED_PLAYING) {
    uint32_t start_rtp = 0;
    scheduler->start_attempts++;

    /* Start in sample coordinates, not block coordinates.  The requested RTP
     * may fall anywhere inside a 1024-sample AAC PCM frame.  The timeline
     * verifies that a continuous preroll exists from that exact sample and
     * returns the same RTP value, rather than rounding to the next block
     * boundary. */
    if (audio_timeline_find_contiguous_from(
            timeline, scheduler->epoch, wanted_rtp, scheduler->preroll_samples,
            0U, &start_rtp)) {
      scheduler->cursor_rtp = start_rtp;
      /* O(1): older preroll becomes lazily reclaimable on ring collision.
       * No 192-slot cleanup scan is performed at start. */
      audio_timeline_set_playback_floor(timeline, scheduler->epoch,
                                        scheduler->cursor_rtp);
      scheduler->state = AUDIO_SCHED_PLAYING;
      scheduler->wait_reason = AUDIO_SCHED_WAIT_NONE;
      scheduler->error_filter_valid = false;
      scheduler->raw_playout_error_samples = 0;
      scheduler->playout_error_samples = 0;
      scheduler->filtered_playout_error_q16 = 0;
      scheduler->drift_reference_error_q16 = 0;
      scheduler->drift_reference_ptp_ns = 0;
    } else {
      /* fallback_after_us is an elapsed-time timeout.  Both timestamps
       * must use the same monotonic local clock.  preroll_started_us is set
       * from esp_timer_get_time() when an epoch/anchor wait begins, while
       * output_ptp_ns belongs to the PTP/network clock domain and must not be
       * compared with it. */
      int64_t now_us = esp_timer_get_time();
      bool fallback_due = scheduler->fallback_after_us > 0 &&
                          scheduler->preroll_started_us > 0 &&
                          now_us >= scheduler->preroll_started_us &&
                          now_us - scheduler->preroll_started_us >=
                              scheduler->fallback_after_us;

      /* Fallback stays sample-granular, but do not start on a single tail
       * sample. Require one complete render quantum (352 samples) from the
       * chosen RTP so the very first I2S callback cannot manufacture a
       * partial conceal just because the next AAC frame has not arrived yet.
       * This touches at most two 1024-sample ring slots and remains O(1). */
      if (fallback_due) {
        scheduler->fallback_attempts++;
      }
      if (fallback_due &&
          audio_timeline_find_contiguous_from(
              timeline, scheduler->epoch, wanted_rtp, AUDIO_V2_BLOCK_SAMPLES,
              AUDIO_V2_BLOCK_SAMPLES, &start_rtp)) {
        scheduler->cursor_rtp = start_rtp;
        /* O(1) recovery jump: skipped READY slots are reclaimed lazily. */
        audio_timeline_set_playback_floor(timeline, scheduler->epoch,
                                          scheduler->cursor_rtp);
        scheduler->wait_reason = AUDIO_SCHED_WAIT_NONE;
        scheduler->state = AUDIO_SCHED_PLAYING;
        scheduler->error_filter_valid = false;
        scheduler->raw_playout_error_samples = 0;
        scheduler->playout_error_samples = 0;
        scheduler->filtered_playout_error_q16 = 0;
        scheduler->drift_reference_error_q16 = 0;
        scheduler->drift_reference_ptp_ns = 0;
      } else {
        scheduler->state = AUDIO_SCHED_PREROLL;
        scheduler->wait_reason = fallback_due ? AUDIO_SCHED_WAIT_FALLBACK_DATA
                                              : AUDIO_SCHED_WAIT_PREROLL;
        scheduler->silent_render_calls++;
        output_silence(out, samples, channels);
        return samples;
      }
    }
  }

  /* A short hole with a known next block can be concealed safely by the
   * timeline reader.  A completely empty/unusable timeline is different:
   * advancing cursor_rtp through unlimited silence makes newly arriving PCM
   * permanently stale and leaves playout hundreds of milliseconds away from
   * the PTP clock.  Stop advancing the RTP cursor and re-enter preroll so the
   * next usable island is selected from the current wanted_rtp. */
  if (!audio_timeline_has_playable_from(timeline, scheduler->epoch,
                                        scheduler->cursor_rtp)) {
    scheduler->state = AUDIO_SCHED_RECOVERING;
    scheduler->wait_reason = AUDIO_SCHED_WAIT_FALLBACK_DATA;
    scheduler->preroll_started_us = esp_timer_get_time();
    scheduler->error_filter_valid = false;
    scheduler->raw_playout_error_samples = 0;
    scheduler->playout_error_samples = 0;
    scheduler->filtered_playout_error_q16 = 0;
    scheduler->drift_reference_error_q16 = 0;
    scheduler->drift_reference_ptp_ns = 0;
    scheduler->silent_render_calls++;
    output_silence(out, samples, channels);
    return samples;
  }

  size_t produced =
      audio_timeline_read(timeline, scheduler->epoch, scheduler->cursor_rtp,
                          out, samples, channels, true, concealed_samples);
  scheduler->cursor_rtp += (uint32_t)produced;
  scheduler->rendered_samples += produced;
  if (produced < samples) {
    output_silence(&out[produced * channels], samples - produced, channels);
    produced = samples;
  }
  return produced;
}
