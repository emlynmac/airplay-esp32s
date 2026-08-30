#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @file sendspin.h
 * @brief Sendspin player-role client (experimental, first milestone).
 *
 * Sendspin is a multi-room audio protocol in which the *server* discovers and
 * connects to speakers.  A speaker advertises _sendspin._tcp with the
 * WebSocket path to use, the server opens that socket, and the two exchange
 * JSON control messages plus binary audio chunks timestamped in the server's
 * monotonic clock.
 *
 * This build implements enough of it to prove the timing path:
 *
 *   - the WebSocket endpoint and its framing, including reassembly;
 *   - the Noise KKpsk2 handshake keyed with the published Sentinel PSK, and
 *     the encrypted transport that follows it;
 *   - the init/hello/activate handshake, the client/time clock exchange and
 *     client/state reporting;
 *   - PCM stream playout through audio_engine_v2.
 *
 * It does NOT implement pairing or the FLAC and Opus codecs.  Keying with the
 * Sentinel leaves the session *unpaired*: encrypted and replay-protected, but
 * with neither peer's identity proven, so a server may only use it for
 * playback once its operator has approved the device.
 *
 * Only one audio source can drive the output at a time.  Sendspin announces
 * itself unavailable while AirPlay, Bluetooth or USB owns it, which is the
 * protocol's own answer to an external source taking the speaker.
 */

/** Called when Sendspin takes or releases the audio output. */
typedef void (*sendspin_activity_cb_t)(bool active);

/**
 * Allocate the receive buffer, playout timeline and persistent identity.
 * Call once at boot, before sendspin_register().
 */
esp_err_t sendspin_init(sendspin_activity_cb_t callback);

/**
 * Attach the WebSocket endpoint to the running web server and start the
 * housekeeping task (clock sync, state reporting, mDNS advertisement).
 */
esp_err_t sendspin_register(httpd_handle_t server);

/** True while a Sendspin stream is driving the output. */
bool sendspin_is_streaming(void);

/**
 * Report whether the audio output is ours to use.
 *
 * Passing false ends any running stream and makes the client advertise
 * `available: false`, which the protocol defines as "my output is in use by
 * an external system": the server moves us to a solo group and stops sending
 * audio.  It will not resume on its own when we become available again.
 */
void sendspin_set_output_available(bool available);
