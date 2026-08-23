#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "audio_stream.h"

#include "esp_log.h"

#include "audio_buffer.h"
#include "audio_decoder.h"
#include "audio_receiver_internal.h"

// Upper bound on how long a decoded frame waits for timeline space.  Roughly
// two AAC frames' worth of slack beyond the deepest expected backlog; past
// that the packet is dropped so the decode task stays responsive to teardown.
#define AUDIO_DECODE_PUSH_TIMEOUT_MS 2000U

static const char *TAG = "audio_stream";

extern const audio_stream_ops_t audio_stream_realtime_ops;
extern const audio_stream_ops_t audio_stream_buffered_ops;

static bool apply_aac_transient_mute(audio_receiver_state_t *state,
                                     int16_t *buffer, size_t samples,
                                     int channels) {
  if (!audio_decoder_is_aac(state->decoder)) {
    return false;
  }

  if ((state->blocks_read_in_sequence <= 2) &&
      (state->blocks_read_in_sequence != state->blocks_read)) {
    memset(buffer, 0, samples * channels * sizeof(int16_t));
    return true;
  }

  return false;
}

bool audio_stream_accept_timestamp(audio_receiver_state_t *state,
                                   uint32_t timestamp) {
  if (!state) {
    return false;
  }

  // Blanket gate: reject everything between seek_flush and the next anchor.
  // Deliberately checked before decrypt/decode in the buffered TCP task so
  // old-track backlog is drained from the socket without decoder CPU or PCM
  // ring use.
  if (state->discard_all_until_anchor) {
    return false;
  }

  // Post-seek RTP window gate: discard frames outside [discard_before_rtp,
  // discard_above_rtp].  The TCP socket buffer can hold many seconds of
  // pre-seek audio; both gates together handle both seek directions:
  //   discard_before_rtp — forward seek: stale frames have lower RTP
  //   discard_above_rtp  — backward seek: stale frames have much higher RTP
  // Each self-disarms on the first frame that passes it.
  if (state->discard_before_rtp_valid) {
    if ((int32_t)(timestamp - state->discard_before_rtp) < 0) {
      return false; // below lower bound — forward-seek stale frame
    }
    state->discard_before_rtp_valid = false;
  }
  if (state->discard_above_rtp_valid) {
    if ((int32_t)(timestamp - state->discard_above_rtp) > 0) {
      return false; // above upper bound — backward-seek stale frame
    }
    state->discard_above_rtp_valid = false;
  }

  return true;
}

// Read-only variant of the RTP gate used for the post-decode re-check.  Unlike
// audio_stream_accept_timestamp() it does NOT disarm the window gates on a
// passing frame — disarming is the job of the ordered pre-decode pass.  If a
// concurrent seek armed a window gate while this frame was mid-decode and the
// frame happens to fall inside the new window, disarming here would clear the
// gate and let subsequent stale TCP backlog through.  This check only reports
// whether the frame must be dropped.
static bool timestamp_is_gated(const audio_receiver_state_t *state,
                               uint32_t timestamp) {
  if (state->discard_all_until_anchor) {
    return true;
  }
  if (state->discard_before_rtp_valid &&
      (int32_t)(timestamp - state->discard_before_rtp) < 0) {
    return true;
  }
  if (state->discard_above_rtp_valid &&
      (int32_t)(timestamp - state->discard_above_rtp) > 0) {
    return true;
  }
  return false;
}

bool audio_stream_process_accepted_frame(audio_receiver_state_t *state,
                                         uint32_t timestamp,
                                         const uint8_t *audio_data,
                                         size_t audio_len) {
  if (!state || !state->decoder) {
    return false;
  }

  size_t capacity_samples = 0;
  int16_t *decode_buffer =
      audio_buffer_get_decode_buffer(&state->buffer, &capacity_samples);
  if (!decode_buffer || capacity_samples == 0) {
    return false;
  }

  audio_decode_info_t info = {0};
  int decoded_samples =
      audio_decoder_decode(state->decoder, audio_data, audio_len, decode_buffer,
                           capacity_samples, &info);
  if (decoded_samples <= 0) {
    return false;
  }

  int channels =
      info.channels > 0 ? info.channels : state->stream->format.channels;
  if (channels <= 0) {
    channels = 2;
  }

  apply_aac_transient_mute(state, decode_buffer, (size_t)decoded_samples,
                           channels);

  // Re-check the gates after decode.  A concurrent seek/anchor flush (RTSP
  // task) can set discard_all_until_anchor OR arm the RTP window gates
  // (discard_before_rtp / discard_above_rtp, Path B) and flush the ring while
  // this frame was being decrypted/decoded.  Use the read-only predicate so a
  // stale mid-flight frame is dropped without disarming a gate a concurrent
  // seek just armed (which would let later backlog through).
  if (timestamp_is_gated(state, timestamp)) {
    return false;
  }

  return audio_buffer_queue_decoded(&state->buffer, &state->stats, timestamp,
                                    decode_buffer, (size_t)decoded_samples,
                                    channels);
}

bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len) {
  if (!audio_stream_accept_timestamp(state, timestamp)) {
    return false;
  }
  return audio_stream_process_accepted_frame(state, timestamp, audio_data,
                                             audio_len);
}

bool audio_stream_decode_encoded_packet(audio_receiver_state_t *state,
                                        const audio_encoded_packet_t *packet) {
  if (!state || !packet || !packet->payload || packet->payload_len == 0) {
    return false;
  }

  // The decoder is destroyed and recreated by audio_receiver_set_format() on
  // the RTSP task, so the worker must hold the mutex across the whole decode.
  if (!state->decoder_mutex ||
      xSemaphoreTake(state->decoder_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  if (!state->decoder || !state->engine_v2_ready ||
      !audio_epoch_matches(&state->engine_v2.epoch, packet->epoch)) {
    xSemaphoreGive(state->decoder_mutex);
    return false;
  }

  // Shared scratch with the realtime path.  Only one of the two stream types
  // is ever active, so there is no contention for it.
  size_t capacity_samples = 0;
  int16_t *decode_buffer =
      audio_buffer_get_decode_buffer(&state->buffer, &capacity_samples);
  if (!decode_buffer || capacity_samples == 0) {
    xSemaphoreGive(state->decoder_mutex);
    return false;
  }

  audio_decode_info_t info = {0};
  const int decoded_samples =
      audio_decoder_decode(state->decoder, packet->payload, packet->payload_len,
                           decode_buffer, capacity_samples, &info);
  if (decoded_samples <= 0) {
    (void)__atomic_add_fetch(&state->engine_v2.diag_decode_fail, 1U,
                             __ATOMIC_RELAXED);
    xSemaphoreGive(state->decoder_mutex);
    return false;
  }

  int channels =
      info.channels > 0 ? info.channels : state->stream->format.channels;
  if (channels <= 0) {
    channels = 2;
  }

  apply_aac_transient_mute(state, decode_buffer, (size_t)decoded_samples,
                           channels);
  xSemaphoreGive(state->decoder_mutex);

  (void)__atomic_add_fetch(&state->engine_v2.diag_decode_ok, 1U,
                           __ATOMIC_RELAXED);

  // AAC frames must advance by exactly one timeline frame.  A break means a
  // packet was lost or reordered, which the timeline will show as a hole.
  if (state->aac_diag_rtp_valid && state->aac_diag_epoch == packet->epoch) {
    const int32_t delta =
        (int32_t)(packet->rtp_timestamp - state->aac_diag_last_rtp);
    if (delta != (int32_t)AUDIO_TIMELINE_FRAME_SAMPLES) {
      ESP_LOGW(TAG, "AAC RTP step %" PRId32 " at rtp=%" PRIu32 " (expected %u)",
               delta, packet->rtp_timestamp,
               (unsigned)AUDIO_TIMELINE_FRAME_SAMPLES);
    }
  }
  state->aac_diag_epoch = packet->epoch;
  state->aac_diag_last_rtp = packet->rtp_timestamp;
  state->aac_diag_rtp_valid = true;

  // Deferred FLUSHBUFFERED boundary: cut the timeline here so this packet and
  // its successors replace the tail of the outgoing track.  Applied before the
  // push so the freed slots are immediately reusable.
  uint32_t flush_until_ts = 0;
  if (audio_timing_take_deferred_flush(&state->timing, packet->rtp_timestamp,
                                       &flush_until_ts)) {
    (void)audio_engine_v2_deferred_flush(&state->engine_v2, packet->epoch,
                                         flush_until_ts);
    state->blocks_read_in_sequence = 0;
  }

  // Re-check the gates: a concurrent seek can have armed them while this
  // frame was in the decoder.
  if (timestamp_is_gated(state, packet->rtp_timestamp)) {
    return false;
  }

  if (!audio_engine_v2_push_pcm_wait(&state->engine_v2, packet->epoch,
                                     packet->rtp_timestamp, decode_buffer,
                                     (size_t)decoded_samples, (uint8_t)channels,
                                     AUDIO_DECODE_PUSH_TIMEOUT_MS)) {
    return false;
  }

  (void)__atomic_add_fetch(&state->engine_v2.diag_pcm_inserted, 1U,
                           __ATOMIC_RELAXED);
  state->stats.packets_received++;
  return true;
}

audio_stream_t *audio_stream_create_realtime(void) {
  audio_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream) {
    return NULL;
  }

  stream->ops = &audio_stream_realtime_ops;
  stream->type = AUDIO_STREAM_REALTIME;
  return stream;
}

audio_stream_t *audio_stream_create_buffered(void) {
  audio_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream) {
    return NULL;
  }

  stream->ops = &audio_stream_buffered_ops;
  stream->type = AUDIO_STREAM_BUFFERED;
  return stream;
}

void audio_stream_destroy(audio_stream_t *stream) {
  if (!stream) {
    return;
  }

  if (stream->ops && stream->ops->destroy) {
    stream->ops->destroy(stream);
    return;
  }

  free(stream);
}

bool audio_stream_uses_buffer(audio_stream_type_t type) {
  return type == AUDIO_STREAM_BUFFERED;
}
