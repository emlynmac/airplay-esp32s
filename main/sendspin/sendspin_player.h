#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sendspin_time.h"

/**
 * @file sendspin_player.h
 * @brief Sendspin "player@v1" playout, built on the AirPlay engine.
 *
 * Sendspin and AirPlay pose the same problem — PCM addressed by a timestamp
 * in a remote clock's domain, to be emitted exactly on time by a local
 * crystal that does not agree — so this reuses audio_engine_v2 wholesale
 * rather than growing a second scheduler.  What it adds is the translation:
 *
 *   - chunk timestamps (server microseconds) become RTP sample numbers
 *     relative to the first chunk of the stream, which is what the clock map
 *     and scheduler consume;
 *   - variable-length chunks (the protocol allows 15-150 ms) are re-cut into
 *     the timeline's fixed blocks, which must land on the block phase;
 *   - the local playout instant is converted into the server's domain with
 *     sendspin_time before the scheduler is asked for samples.
 *
 * The engine here is a second, smaller instance rather than the receiver's:
 * the receiver's is never torn down (the playback task renders from it), and
 * hijacking it would mean reasoning about a handover while both stacks are
 * live.  Ownership of the I2S output is arbitrated one level up.
 */

typedef struct {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bit_depth;
  bool pcm; /* false for a codec this milestone cannot decode */
} sendspin_player_format_t;

/**
 * Allocate the playout timeline (~384 KB of PSRAM at the default depth).
 *
 * @param clock estimator owned by the session; must outlive the player.
 */
esp_err_t sendspin_player_init(const sendspin_time_t *clock);

/** Release the timeline. Must not be called while the player owns output. */
void sendspin_player_deinit(void);

/** Bytes of queued audio the client advertises it can hold. */
uint32_t sendspin_player_buffer_capacity(void);

/** Milliseconds of audio the client wants buffered before it plays. */
uint32_t sendspin_player_min_buffer_ms(void);

/**
 * Begin a stream. Installs the output source, so the caller must already
 * have stopped whatever else was driving I2S.
 *
 * @return ESP_ERR_NOT_SUPPORTED for a format this milestone cannot play.
 */
esp_err_t sendspin_player_stream_start(const sendspin_player_format_t *format);

/** Drop buffered audio but keep the stream open (seek / track jump). */
void sendspin_player_stream_clear(void);

/** Stop output, drop buffered audio and release the output source. */
void sendspin_player_stream_end(void);

/**
 * Feed one audio chunk.
 *
 * @param timestamp_us server-clock instant for the chunk's first sample.
 * @param data         encoded payload (raw interleaved PCM in this milestone).
 * @param len          payload length in bytes.
 */
void sendspin_player_chunk(int64_t timestamp_us, const uint8_t *data,
                           size_t len);

/** True between stream/start and stream/end. */
bool sendspin_player_is_streaming(void);

/** Log a one-line summary of buffer occupancy and sync error. */
void sendspin_player_log_status(void);
