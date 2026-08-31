#include "sendspin.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "mdns.h"
#include "nvs.h"
#include "sodium.h"

#include "ethernet.h"
#include "playback_control.h"
#include "rtsp_events.h"
#include "sendspin_noise.h"
#include "sendspin_player.h"
#include "sendspin_time.h"
#include "settings.h"
#include "spiram_task.h"
#include "wifi.h"

static const char *TAG = "sendspin";

#define SENDSPIN_WS_PATH       "/sendspin"
#define SENDSPIN_NVS_NAMESPACE "sendspin"
#define SENDSPIN_NVS_KEY_SK    "sk"

/* Housekeeping tick. Also the resolution of the clock-sync schedule, so it
 * has to divide the burst interval below. */
#define SENDSPIN_TICK_MS 50U

/* Clock exchange rate before the filter has converged. The player cannot
 * report itself available until it has, so this is the dominant term in how
 * long the server waits for us after connecting. */
#define SENDSPIN_TIME_BURST_MS 250U

#define SENDSPIN_TASK_STACK 5120

/* Roughly one esp_timer tick of slop on either side of the identity check
 * below; a Curve25519 public key is 32 bytes, which is 43 base64url
 * characters with the padding stripped. */
#define SENDSPIN_CLIENT_ID_LEN 43

/* The Noise prologue is the exact wire bytes of client/init followed by
 * server/init. Both are short and fixed in shape; anything approaching this
 * is a server we do not understand. */
#define SENDSPIN_PROLOGUE_MAX 1024

/* Largest JSON control message we will send. client/hello is the big one at
 * a few hundred bytes. */
#define SENDSPIN_TX_PLAIN_MAX 2048

/* First byte of a decrypted binary message. Audio chunk layout is
 * [4][timestamp:8 BE][send_ahead:4 BE][encoded audio]. */
#define SENDSPIN_BIN_JSON        0
#define SENDSPIN_BIN_FRAGMENT    1
#define SENDSPIN_BIN_AUDIO_CHUNK 4
#define SENDSPIN_AUDIO_HEADER    13

typedef enum {
  SENDSPIN_IDLE = 0,  /* no socket */
  SENDSPIN_NEED_INIT, /* socket up, client/init not sent yet */
  SENDSPIN_INIT_SENT, /* waiting for server/init */
  SENDSPIN_HANDSHAKE, /* waiting for Noise message 1 */
  SENDSPIN_ENCRYPTED, /* transport mode; waiting for server/hello */
  SENDSPIN_READY,     /* client/hello sent; waiting for server/activate */
  SENDSPIN_ACTIVATED, /* activated; clock sync and state reporting run */
} sendspin_state_t;

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_lock = NULL;
/* Serialises socket writes.  Replies are sent from the httpd task while the
 * housekeeping task sends time requests and state reports, and a WebSocket
 * frame is several write() calls, so without this two frames interleave on
 * the wire and the server sees a corrupt stream.  Always taken *inside*
 * s_lock, never the other way round. */
static SemaphoreHandle_t s_tx_lock = NULL;
static TaskHandle_t s_task = NULL;

static volatile int s_fd = -1;
static volatile sendspin_state_t s_state = SENDSPIN_IDLE;

static sendspin_time_t s_clock;
static sendspin_activity_cb_t s_activity_cb = NULL;

/* Roles the server has activated. Carried across activations that omit
 * active_roles, so they cannot be re-derived per message. */
static bool s_role_player = false;
static bool s_role_metadata = false;
static bool s_role_controller = false;

/* Commands the server says the group accepts, as a bitmask over
 * sendspin_command_t. Sending one it has not offered is defined as ignored,
 * so this is what lets a button fall back to a local action instead. */
static uint32_t s_ctrl_commands = 0;
static QueueHandle_t s_cmd_queue = NULL;

/* Availability has two independent inputs: whether another source owns the
 * output, and whether the clock estimate is good enough to place audio. The
 * protocol only has one flag, so it is the AND of the two. */
static bool s_output_available = true;
static bool s_reported_available = false;
static bool s_state_dirty = false;

/* Last volume and mute state sent to the server. The device's own buttons and
 * web UI move these too, so the tick compares them rather than relying on
 * every local path knowing to notify us. */
static int s_reported_volume = -1;
static bool s_reported_muted = false;

static uint8_t *s_rx = NULL;  /* one WebSocket frame */
static uint8_t *s_asm = NULL; /* fragment reassembly */
static size_t s_asm_len = 0;
static uint8_t s_asm_type = 0;
static bool s_asm_active = false;

/* Noise transport scratch. s_pt holds the plaintext of a received frame;
 * s_tx_plain and s_tx_cipher build one outgoing frame and are only touched
 * under s_tx_lock. */
static uint8_t *s_pt = NULL;
static uint8_t *s_tx_plain = NULL;
static uint8_t *s_tx_cipher = NULL;

static sendspin_noise_t s_noise;
static uint8_t s_client_priv[crypto_scalarmult_curve25519_BYTES];
static uint8_t s_client_pub[crypto_scalarmult_curve25519_BYTES];
static uint8_t s_prologue[SENDSPIN_PROLOGUE_MAX];
static size_t s_prologue_len = 0;

static char s_client_id[SENDSPIN_CLIENT_ID_LEN + 1];
static bool s_mdns_advertised = false;
static int64_t s_last_time_tx_us = 0;

/* ------------------------------------------------------------------ */
/*  Identity                                                           */
/* ------------------------------------------------------------------ */

/* client_id is the base64url of a Curve25519 public key, and the same keypair
 * is the client's static key in the Noise handshake -- which is why the
 * private half is kept for the life of the process rather than wiped here. */
static esp_err_t sendspin_load_identity(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  size_t len = sizeof(s_client_priv);
  err = nvs_get_blob(nvs, SENDSPIN_NVS_KEY_SK, s_client_priv, &len);
  if (err != ESP_OK || len != sizeof(s_client_priv)) {
    randombytes_buf(s_client_priv, sizeof(s_client_priv));
    err = nvs_set_blob(nvs, SENDSPIN_NVS_KEY_SK, s_client_priv,
                       sizeof(s_client_priv));
    if (err == ESP_OK) {
      err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "identity not persisted: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "generated a new client identity");
  }
  nvs_close(nvs);

  if (crypto_scalarmult_curve25519_base(s_client_pub, s_client_priv) != 0) {
    sodium_memzero(s_client_priv, sizeof(s_client_priv));
    return ESP_FAIL;
  }

  sodium_bin2base64(s_client_id, sizeof(s_client_id), s_client_pub,
                    sizeof(s_client_pub),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Sending                                                            */
/* ------------------------------------------------------------------ */

/* Is `fd` still a live WebSocket on our server?  A session that dies without
 * sending CLOSE -- a reset, a WiFi drop, a port scanner hanging up -- leaves
 * no event behind, so a remembered fd cannot be trusted on its own: the
 * number is recycled and would eventually address an unrelated request.
 * log_stream.c avoids this by never remembering one at all; Sendspin's
 * session is stateful, so it re-checks instead. */
static bool sendspin_fd_is_live(int fd) {
  if (!s_server || fd < 0) {
    return false;
  }
  return httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static bool sendspin_send_text(const char *json) {
  const int fd = s_fd;
  if (!s_server || fd < 0 || !json) {
    return false;
  }
  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)json,
      .len = strlen(json),
  };
  if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "send timed out waiting for the socket");
    return false;
  }
  esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
  xSemaphoreGive(s_tx_lock);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "send failed on fd=%d: %s", fd, esp_err_to_name(err));
    return false;
  }
  return true;
}

/* Seals `body` as a Noise transport message and sends it as a binary frame.
 * The nonce counter advances with the encryption, so encrypting and writing
 * have to stay inside one critical section or two concurrent senders would
 * put the frames on the wire out of counter order and the server's very next
 * decryption would fail. */
static bool sendspin_send_encrypted(uint8_t type, const uint8_t *body,
                                    size_t len) {
  const int fd = s_fd;
  if (!s_server || fd < 0) {
    return false;
  }
  if (len + 1 > SENDSPIN_TX_PLAIN_MAX) {
    ESP_LOGE(TAG, "message of %u bytes exceeds the send buffer", (unsigned)len);
    return false;
  }
  if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "send timed out waiting for the socket");
    return false;
  }

  s_tx_plain[0] = type;
  memcpy(&s_tx_plain[1], body, len);

  size_t cipher_len = 0;
  esp_err_t err = sendspin_noise_encrypt(
      &s_noise, s_tx_plain, len + 1, s_tx_cipher,
      SENDSPIN_TX_PLAIN_MAX + SENDSPIN_NOISE_TAG_LEN, &cipher_len);
  if (err == ESP_OK) {
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = s_tx_cipher,
        .len = cipher_len,
    };
    err = httpd_ws_send_frame_async(s_server, fd, &frame);
  }
  xSemaphoreGive(s_tx_lock);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "encrypted send failed on fd=%d: %s", fd,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

/* Serialises and sends `root` as a cleartext text frame, then frees it. Only
 * the three handshake messages may take this route. */
static bool sendspin_send_cleartext_json(cJSON *root) {
  if (!root) {
    return false;
  }
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    return false;
  }
  ESP_LOGD(TAG, "tx %s", text);
  const bool ok = sendspin_send_text(text);
  cJSON_free(text);
  return ok;
}

/* Serialises and sends `root`, then frees it. Returns false if the socket has
 * gone; the caller treats that as a disconnect rather than an error.
 *
 * The transport switches under this function: before the Noise split a
 * message goes out as cleartext text, after it as an encrypted binary frame
 * with a leading type byte. */
static bool sendspin_send_json(cJSON *root) {
  if (!sendspin_noise_ready(&s_noise)) {
    return sendspin_send_cleartext_json(root);
  }
  if (!root) {
    return false;
  }
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    return false;
  }
  ESP_LOGD(TAG, "tx %s", text);
  const bool ok = sendspin_send_encrypted(SENDSPIN_BIN_JSON,
                                          (const uint8_t *)text, strlen(text));
  cJSON_free(text);
  return ok;
}

static cJSON *sendspin_new_message(const char *type, cJSON **payload_out) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return NULL;
  }
  cJSON *payload = cJSON_CreateObject();
  if (!payload || !cJSON_AddStringToObject(root, "type", type) ||
      !cJSON_AddItemToObject(root, "payload", payload)) {
    cJSON_Delete(payload);
    cJSON_Delete(root);
    return NULL;
  }
  *payload_out = payload;
  return root;
}

/* The Noise prologue is the raw bytes of client/init and server/init exactly
 * as they crossed the wire, so neither may be re-serialised: two JSON writers
 * that order keys differently would produce the same message and a different
 * prologue, and the handshake would fail with nothing to point at. */
static bool sendspin_prologue_append(const void *data, size_t len) {
  if (s_prologue_len + len > sizeof(s_prologue)) {
    ESP_LOGE(TAG, "prologue overflow (%u bytes)",
             (unsigned)(s_prologue_len + len));
    return false;
  }
  memcpy(&s_prologue[s_prologue_len], data, len);
  s_prologue_len += len;
  return true;
}

static void sendspin_send_init(void) {
  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/init", &payload);
  if (!root) {
    return;
  }
  cJSON_AddStringToObject(payload, "client_id", s_client_id);
  cJSON_AddNumberToObject(payload, "version", 1);
  cJSON_AddStringToObject(payload, "suite", "25519_ChaChaPoly_SHA256");

  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    return;
  }
  s_prologue_len = 0;
  if (sendspin_prologue_append(text, strlen(text))) {
    ESP_LOGD(TAG, "tx %s", text);
    (void)sendspin_send_text(text);
  }
  cJSON_free(text);
}

static void sendspin_send_hello(void) {
  char name[65];
  settings_get_device_name(name, sizeof(name));

  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/hello", &payload);
  if (!root) {
    return;
  }
  cJSON_AddStringToObject(payload, "name", name);

  uint8_t mac[6];
  char mac_str[18];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);

  cJSON *info = cJSON_AddObjectToObject(payload, "device_info");
  if (info) {
    /* A server renders these as "<manufacturer>/<product_name>", and the same
     * board also advertises an AirPlay receiver, so name the transport here to
     * tell the two entries apart. */
    cJSON_AddStringToObject(info, "product_name", "Sendspin");
    cJSON_AddStringToObject(info, "manufacturer", "AirPlay-ESP32");
    cJSON_AddStringToObject(info, "software_version",
                            esp_app_get_description()->version);
    cJSON_AddStringToObject(info, "mac_address", mac_str);
  }

  cJSON *roles = cJSON_AddArrayToObject(payload, "supported_roles");
  if (roles) {
    cJSON_AddItemToArray(roles, cJSON_CreateString("player@v1"));
    /* metadata@v1 has no support object; listing it is the whole opt-in. */
    cJSON_AddItemToArray(roles, cJSON_CreateString("metadata@v1"));
    cJSON_AddItemToArray(roles, cJSON_CreateString("controller@v1"));
  }

  cJSON *support = cJSON_AddObjectToObject(payload, "player@v1_support");
  if (support) {
    cJSON *formats = cJSON_AddArrayToObject(support, "supported_formats");
    if (formats) {
      /* Priority order. FLAC and Opus are not decoded in this build, so
       * offering them would only get us audio we have to drop. */
      static const int rates[] = {44100, 48000};
      for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        cJSON *fmt = cJSON_CreateObject();
        if (!fmt) {
          continue;
        }
        cJSON_AddStringToObject(fmt, "codec", "pcm");
        cJSON_AddNumberToObject(fmt, "channels", 2);
        cJSON_AddNumberToObject(fmt, "sample_rate", rates[i]);
        cJSON_AddNumberToObject(fmt, "bit_depth", 16);
        cJSON_AddItemToArray(formats, fmt);
      }
    }
    cJSON_AddNumberToObject(support, "buffer_capacity",
                            sendspin_player_buffer_capacity());
    cJSON *commands = cJSON_AddArrayToObject(support, "supported_commands");
    if (commands) {
      cJSON_AddItemToArray(commands, cJSON_CreateString("volume"));
      cJSON_AddItemToArray(commands, cJSON_CreateString("mute"));
    }
  }

  /* An object keyed by method identifier, empty because no pairing method is
   * implemented. That is only viable alongside unpaired access: a client that
   * offers neither is "locked down" and must hang up on server/hello. */
  cJSON_AddObjectToObject(payload, "supported_pair_methods");
  cJSON *unpaired = cJSON_AddObjectToObject(payload, "unpaired_access");
  if (unpaired) {
    cJSON_AddBoolToObject(unpaired, "enabled", true);
  }

  (void)sendspin_send_json(root);
}

static void sendspin_send_state(void) {
  const bool available =
      s_output_available && sendspin_time_converged(&s_clock);
  const int volume = playback_control_get_level_percent();
  const bool muted = playback_control_is_muted();

  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/state", &payload);
  if (!root) {
    return;
  }
  cJSON_AddBoolToObject(payload, "available", available);

  cJSON *player = cJSON_AddObjectToObject(payload, "player");
  if (player) {
    /* The DAC and DMA ring are already compensated for by
     * audio_output_get_next_playout_time_ns(), so there is no delay left
     * beyond the audio port for the server to add. */
    cJSON_AddNumberToObject(player, "output_delay_ms", 0);
    cJSON_AddNumberToObject(player, "required_lead_time_ms",
                            (double)sendspin_player_min_buffer_ms());
    cJSON_AddNumberToObject(player, "min_buffer_ms",
                            (double)sendspin_player_min_buffer_ms());
    cJSON_AddNumberToObject(player, "volume", volume);
    cJSON_AddBoolToObject(player, "muted", muted);
    /* This list is only ever set_output_delay; volume and mute are advertised
     * in player@v1_support instead. */
    cJSON_AddArrayToObject(player, "supported_commands");
  }

  if (sendspin_send_json(root)) {
    s_reported_available = available;
    s_reported_volume = volume;
    s_reported_muted = muted;
    s_state_dirty = false;
  }
}

static void sendspin_send_time_request(void) {
  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/time", &payload);
  if (!root) {
    return;
  }
  /* Taken as late as the message layer allows: everything between here and
   * the socket write is measured as round-trip delay, and half of any
   * asymmetry there lands directly in the offset estimate. */
  const int64_t now_us = esp_timer_get_time();
  cJSON_AddNumberToObject(payload, "client_transmitted", (double)now_us);
  s_last_time_tx_us = now_us;
  (void)sendspin_send_json(root);
}

/* ------------------------------------------------------------------ */
/*  Playback events                                                    */
/* ------------------------------------------------------------------ */

/* The display, the LEDs and the boards that gate their amplifier on playback
 * all listen on the RTSP event bus, so a Sendspin session has to raise the
 * same events an AirPlay or a Bluetooth one would.  Without them a SqueezeAMP
 * or Esparagus board never leaves DAC_POWER_OFF and the stream is rendered
 * into a powered-down amplifier. */

typedef struct {
  rtsp_metadata_t meta; /* position_secs is filled in at emit time */
  int64_t timestamp_us; /* server clock the progress was measured at */
  int64_t progress_ms;  /* track position at timestamp_us */
  int32_t speed;        /* playback_speed; 1000 is normal, 0 is paused */
  bool has_progress;
} sendspin_meta_t;

static sendspin_meta_t s_meta;
static sendspin_meta_t s_meta_pending;
static bool s_meta_pending_valid = false;
static bool s_events_connected = false;
static bool s_events_playing = false;

static void sendspin_events_playing(bool playing) {
  if (playing == s_events_playing) {
    return;
  }
  s_events_playing = playing;
  rtsp_events_emit(playing ? RTSP_EVENT_PLAYING : RTSP_EVENT_PAUSED, NULL);
}

static void sendspin_events_connected(bool connected) {
  if (connected == s_events_connected) {
    return;
  }
  s_events_connected = connected;
  if (connected) {
    /* Listeners read this as "a new source has the output": the display drops
     * whatever it was showing and the amplifier comes up into standby. */
    rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED, NULL);
    return;
  }
  s_events_playing = false;
  memset(&s_meta, 0, sizeof(s_meta));
  s_meta_pending_valid = false;
  rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
}

/* ------------------------------------------------------------------ */
/*  Session lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void sendspin_session_close(const char *reason) {
  if (s_fd < 0) {
    return;
  }
  ESP_LOGI(TAG, "session closed (%s)", reason);
  s_fd = -1;
  s_state = SENDSPIN_IDLE;
  s_asm_active = false;
  s_asm_len = 0;
  s_reported_available = false;
  s_state_dirty = false;
  /* A server MUST NOT assume volume and mute are unchanged after a
   * reconnect, so make sure the next state report is treated as news. */
  s_reported_volume = -1;
  s_prologue_len = 0;
  s_role_player = false;
  s_role_metadata = false;
  s_role_controller = false;
  s_ctrl_commands = 0;
  if (s_cmd_queue) {
    xQueueReset(s_cmd_queue);
  }
  sendspin_noise_reset(&s_noise);
  sendspin_time_reset(&s_clock);
  sendspin_events_connected(false);

  if (sendspin_player_is_streaming()) {
    sendspin_player_stream_end();
    if (s_activity_cb) {
      s_activity_cb(false);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Message handling                                                   */
/* ------------------------------------------------------------------ */

static double sendspin_number(const cJSON *object, const char *key,
                              double fallback) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? cJSON_GetNumberValue(item) : fallback;
}

/* ------------------------------------------------------------------ */
/*  metadata@v1                                                        */
/* ------------------------------------------------------------------ */

/* Metadata timestamps are on the server's clock, the same domain the audio
 * chunks are addressed in. */
static bool sendspin_server_now_us(int64_t *server_us) {
  const int64_t local_us = esp_timer_get_time();
  int64_t offset_ns = 0;
  if (!sendspin_time_offset_ns(&s_clock, local_us, &offset_ns)) {
    return false;
  }
  *server_us = local_us + offset_ns / 1000;
  return true;
}

/* The state reports the position at its own timestamp, so a state that has
 * been sitting here -- a scheduled update that has just come due, or a late
 * join partway through a track -- has to be run forward to now. */
static uint32_t sendspin_meta_position_secs(void) {
  if (!s_meta.has_progress) {
    return 0;
  }
  int64_t ms = s_meta.progress_ms;
  int64_t now_us = 0;
  if (s_meta.speed != 0 && sendspin_server_now_us(&now_us)) {
    ms += (now_us - s_meta.timestamp_us) * s_meta.speed / 1000000;
  }
  const int64_t duration_ms = (int64_t)s_meta.meta.duration_secs * 1000;
  if (duration_ms > 0 && ms > duration_ms) {
    ms = duration_ms;
  }
  return ms > 0 ? (uint32_t)(ms / 1000) : 0;
}

/* Listeners interpolate the position themselves from the last one they were
 * given, so this only runs on a real change. */
static void sendspin_meta_apply(const sendspin_meta_t *next) {
  s_meta = *next;

  /* A server with an idle queue activates the role and immediately sends an
   * empty state. Taking that as a source would wake the amplifier and light
   * the display for as long as the server stayed connected, which for Music
   * Assistant is for ever. */
  const bool has_content = next->meta.title[0] != '\0' ||
                           next->meta.artist[0] != '\0' ||
                           next->meta.album[0] != '\0' || next->has_progress;
  if (!has_content && !sendspin_player_is_streaming()) {
    sendspin_events_connected(false);
    return;
  }

  sendspin_events_connected(true);

  rtsp_event_data_t data = {.metadata = s_meta.meta};
  data.metadata.position_secs = sendspin_meta_position_secs();
  rtsp_events_emit(RTSP_EVENT_METADATA, &data);

  if (next->has_progress) {
    sendspin_events_playing(next->speed != 0);
  }
}

static void sendspin_meta_string(const cJSON *object, const char *key,
                                 char *dst, size_t size) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsString(item) && item->valuestring) {
    snprintf(dst, size, "%s", item->valuestring);
  }
}

/* server/state carries the full state of every role object it includes: an
 * omitted object means unchanged, and an explicit null clears the role. */
static void sendspin_handle_state_metadata(const cJSON *payload) {
  const cJSON *meta = cJSON_GetObjectItemCaseSensitive(payload, "metadata");
  if (!meta) {
    return;
  }
  if (cJSON_IsNull(meta)) {
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta_pending_valid = false;
    if (!sendspin_player_is_streaming()) {
      sendspin_events_connected(false);
    }
    return;
  }
  if (!cJSON_IsObject(meta)) {
    return;
  }

  sendspin_meta_t next = {0};
  next.timestamp_us = (int64_t)sendspin_number(meta, "timestamp", 0);
  sendspin_meta_string(meta, "title", next.meta.title, sizeof(next.meta.title));
  sendspin_meta_string(meta, "artist", next.meta.artist,
                       sizeof(next.meta.artist));
  sendspin_meta_string(meta, "album", next.meta.album, sizeof(next.meta.album));

  const cJSON *progress = cJSON_GetObjectItemCaseSensitive(meta, "progress");
  if (cJSON_IsObject(progress)) {
    next.has_progress = true;
    next.progress_ms = (int64_t)sendspin_number(progress, "track_progress", 0);
    next.speed = (int32_t)sendspin_number(progress, "playback_speed", 1000);
    next.meta.duration_secs =
        (uint32_t)(sendspin_number(progress, "track_duration", 0) / 1000.0);
  }

  /* A future timestamp schedules the update -- usually the next track, timed
   * to the audible change -- and replaces any update still pending. */
  int64_t now_us = 0;
  if (next.timestamp_us > 0 && sendspin_server_now_us(&now_us) &&
      next.timestamp_us > now_us) {
    s_meta_pending = next;
    s_meta_pending_valid = true;
    return;
  }

  s_meta_pending_valid = false;
  sendspin_meta_apply(&next);
}

static void sendspin_meta_tick(void) {
  int64_t now_us = 0;
  if (!s_meta_pending_valid || !sendspin_server_now_us(&now_us) ||
      now_us < s_meta_pending.timestamp_us) {
    return;
  }
  const sendspin_meta_t due = s_meta_pending;
  s_meta_pending_valid = false;
  sendspin_meta_apply(&due);
}

/* ------------------------------------------------------------------ */
/*  controller@v1                                                      */
/* ------------------------------------------------------------------ */

/* Indexed by sendspin_command_t; s_ctrl_commands is a bitmask over the same
 * order. The protocol has many more commands, but these are the four a set of
 * hardware buttons can raise. */
static const char *const SENDSPIN_CMD_NAMES[] = {
    [SENDSPIN_CMD_PLAY] = "play",
    [SENDSPIN_CMD_PAUSE] = "pause",
    [SENDSPIN_CMD_NEXT] = "next",
    [SENDSPIN_CMD_PREVIOUS] = "previous",
};
#define SENDSPIN_CMD_COUNT \
  (sizeof(SENDSPIN_CMD_NAMES) / sizeof(SENDSPIN_CMD_NAMES[0]))

static void sendspin_handle_state_controller(const cJSON *payload) {
  const cJSON *controller =
      cJSON_GetObjectItemCaseSensitive(payload, "controller");
  if (!controller) {
    return;
  }
  if (!cJSON_IsObject(controller)) {
    s_ctrl_commands = 0;
    return;
  }

  const cJSON *commands =
      cJSON_GetObjectItemCaseSensitive(controller, "supported_commands");
  uint32_t mask = 0;
  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, commands) {
    if (!cJSON_IsString(item)) {
      continue;
    }
    for (size_t i = 0; i < SENDSPIN_CMD_COUNT; i++) {
      if (strcmp(cJSON_GetStringValue(item), SENDSPIN_CMD_NAMES[i]) == 0) {
        mask |= 1U << i;
      }
    }
  }

  if (mask != s_ctrl_commands) {
    ESP_LOGI(TAG, "controller commands: play=%s pause=%s next=%s previous=%s",
             (mask & (1U << SENDSPIN_CMD_PLAY)) ? "yes" : "no",
             (mask & (1U << SENDSPIN_CMD_PAUSE)) ? "yes" : "no",
             (mask & (1U << SENDSPIN_CMD_NEXT)) ? "yes" : "no",
             (mask & (1U << SENDSPIN_CMD_PREVIOUS)) ? "yes" : "no");
  }
  s_ctrl_commands = mask;
}

static void sendspin_handle_server_state(const cJSON *payload) {
  sendspin_handle_state_metadata(payload);
  sendspin_handle_state_controller(payload);
}

static void sendspin_send_controller_command(sendspin_command_t cmd) {
  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/command", &payload);
  if (!root) {
    return;
  }
  cJSON *controller = cJSON_AddObjectToObject(payload, "controller");
  if (controller) {
    cJSON_AddStringToObject(controller, "command", SENDSPIN_CMD_NAMES[cmd]);
  }
  ESP_LOGI(TAG, "controller command: %s", SENDSPIN_CMD_NAMES[cmd]);
  (void)sendspin_send_json(root);
}

/* Drains the queue the buttons post to. Sending has to happen here because
 * the socket belongs to this task. */
static void sendspin_command_tick(void) {
  uint8_t cmd = 0;
  while (s_cmd_queue && xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
    if (cmd < SENDSPIN_CMD_COUNT) {
      sendspin_send_controller_command((sendspin_command_t)cmd);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Noise handshake                                                    */
/* ------------------------------------------------------------------ */

/* Noise message 1 is an ephemeral key, an encrypted static key and an
 * encrypted payload; message 2 is an ephemeral key and an encrypted "{}". */
#define SENDSPIN_NOISE_MSG_MAX 256
#define SENDSPIN_NOISE_MSG2_LEN \
  (SENDSPIN_NOISE_KEY_LEN + 2 + SENDSPIN_NOISE_TAG_LEN)

static void sendspin_handle_server_init(const cJSON *payload) {
  if (s_state != SENDSPIN_INIT_SENT) {
    sendspin_session_close("unexpected server/init");
    return;
  }
  /* An exact match, not a floor: a future core format bumps this and defines
   * its own negotiation. */
  if ((int)sendspin_number(payload, "version", 0) != 1) {
    sendspin_session_close("unsupported core message version");
    return;
  }

  const cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "server_id");
  const char *id_str = cJSON_IsString(id) ? cJSON_GetStringValue(id) : NULL;
  uint8_t server_pub[crypto_scalarmult_curve25519_BYTES];
  size_t decoded = 0;
  if (!id_str || strlen(id_str) != SENDSPIN_CLIENT_ID_LEN ||
      sodium_base642bin(server_pub, sizeof(server_pub), id_str, strlen(id_str),
                        NULL, &decoded, NULL,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
      decoded != sizeof(server_pub)) {
    sendspin_session_close("malformed server_id");
    return;
  }

  if (sendspin_noise_start(&s_noise, s_client_priv, s_client_pub, server_pub,
                           s_prologue, s_prologue_len) != ESP_OK) {
    sendspin_session_close("handshake setup failed");
    return;
  }
  ESP_LOGI(TAG, "server_id %s", id_str);
  s_state = SENDSPIN_HANDSHAKE;
}

/* The message 1 payload names the PSK the server picked. We only ever hold
 * the published Sentinel, so the answer is the same either way; logging which
 * it was is the only way to tell an unpaired session apart from a server that
 * still holds a pairing record we lost. */
static void sendspin_log_psk_choice(const char *json, size_t len) {
  cJSON *root = cJSON_ParseWithLength(json, len);
  if (!root) {
    return;
  }
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "psk_id");
  const cJSON *category =
      cJSON_GetObjectItemCaseSensitive(root, "psk_category");
  const char *id_str = cJSON_IsString(id) ? cJSON_GetStringValue(id) : "";
  if (strcmp(id_str, sendspin_noise_sentinel_psk_id()) != 0) {
    /* A lookup miss. The spec's Sentinel Fallback says to answer with the
     * Sentinel anyway; the server reads that as a credential mismatch and
     * should offer its operator a re-pair. */
    ESP_LOGW(TAG,
             "server referenced an unknown PSK (%s, category %s) -- "
             "falling back to the Sentinel; re-pair on the server to "
             "clear it",
             id_str,
             cJSON_IsString(category) ? cJSON_GetStringValue(category)
                                      : "unspecified");
  }
  cJSON_Delete(root);
}

static void sendspin_handle_noise_handshake(const cJSON *payload) {
  if (s_state != SENDSPIN_HANDSHAKE) {
    /* An in-band re-handshake, which this build does not implement. The
     * specified response to any handshake failure is a silent close. */
    ESP_LOGW(TAG, "unexpected noise/handshake in state %d", (int)s_state);
    sendspin_session_close("re-handshake unsupported");
    return;
  }

  const cJSON *data = cJSON_GetObjectItemCaseSensitive(payload, "data");
  const char *b64 = cJSON_IsString(data) ? cJSON_GetStringValue(data) : NULL;
  uint8_t msg[SENDSPIN_NOISE_MSG_MAX];
  size_t msg_len = 0;
  if (!b64 ||
      sodium_base642bin(msg, sizeof(msg), b64, strlen(b64), NULL, &msg_len,
                        NULL, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
    sendspin_session_close("malformed noise/handshake");
    return;
  }

  char psk_json[192];
  size_t psk_len = 0;
  if (sendspin_noise_read_message1(&s_noise, msg, msg_len, (uint8_t *)psk_json,
                                   sizeof(psk_json) - 1, &psk_len) != ESP_OK) {
    /* Almost always a prologue mismatch or the wrong static key rather than
     * anything to do with the PSK, which is not mixed in yet. */
    sendspin_session_close("handshake message 1 rejected");
    return;
  }
  psk_json[psk_len] = '\0';
  sendspin_log_psk_choice(psk_json, psk_len);

  static const uint8_t empty_object[2] = {'{', '}'};
  uint8_t reply[SENDSPIN_NOISE_MSG2_LEN];
  size_t reply_len = 0;
  if (sendspin_noise_write_message2(&s_noise, sendspin_noise_sentinel_psk(),
                                    empty_object, sizeof(empty_object), reply,
                                    sizeof(reply), &reply_len) != ESP_OK) {
    sendspin_session_close("handshake message 2 failed");
    return;
  }

  char reply_b64[sodium_base64_ENCODED_LEN(
      SENDSPIN_NOISE_MSG2_LEN, sodium_base64_VARIANT_URLSAFE_NO_PADDING)];
  sodium_bin2base64(reply_b64, sizeof(reply_b64), reply, reply_len,
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);

  cJSON *out = NULL;
  cJSON *root = sendspin_new_message("noise/handshake", &out);
  if (!root) {
    sendspin_session_close("out of memory");
    return;
  }
  cJSON_AddStringToObject(out, "data", reply_b64);

  /* Still cleartext: the split has happened but message 2 itself is the last
   * thing on the wire that is not a transport ciphertext. */
  s_state = SENDSPIN_ENCRYPTED;
  if (!sendspin_send_cleartext_json(root)) {
    sendspin_session_close("handshake reply not sent");
  }
}

static void sendspin_handle_server_time(const cJSON *payload,
                                        int64_t arrival_us) {
  const int64_t client_tx =
      (int64_t)sendspin_number(payload, "client_transmitted", 0);
  const int64_t server_rx =
      (int64_t)sendspin_number(payload, "server_received", 0);
  const int64_t server_tx =
      (int64_t)sendspin_number(payload, "server_transmitted", 0);
  if (client_tx == 0 || server_rx == 0 || server_tx == 0) {
    return;
  }

  const bool was_converged = sendspin_time_converged(&s_clock);
  (void)sendspin_time_update(&s_clock, client_tx, server_rx, server_tx,
                             arrival_us);
  if (!was_converged && sendspin_time_converged(&s_clock)) {
    ESP_LOGI(TAG, "clock converged: rtt=%" PRId64 " us skew=%" PRId32 " ppm",
             sendspin_time_best_rtt_us(&s_clock),
             sendspin_time_skew_ppm(&s_clock));
    s_state_dirty = true;
  }
}

static void sendspin_handle_stream_start(const cJSON *payload) {
  const cJSON *player = cJSON_GetObjectItemCaseSensitive(payload, "player");
  if (!cJSON_IsObject(player)) {
    return; /* a stream for a role we do not implement */
  }

  const cJSON *codec = cJSON_GetObjectItemCaseSensitive(player, "codec");
  const sendspin_player_format_t format = {
      .sample_rate = (uint32_t)sendspin_number(player, "sample_rate", 44100),
      .channels = (uint8_t)sendspin_number(player, "channels", 2),
      .bit_depth = (uint8_t)sendspin_number(player, "bit_depth", 16),
      .pcm = cJSON_IsString(codec) &&
             strcmp(cJSON_GetStringValue(codec), "pcm") == 0,
  };

  if (!s_output_available) {
    ESP_LOGW(TAG, "stream/start while the output is owned elsewhere");
    return;
  }

  /* Hand the output over before the player claims it: the AirPlay playback
   * task and the Sendspin renderer both drive the same DMA ring. */
  if (!sendspin_player_is_streaming() && s_activity_cb) {
    s_activity_cb(true);
  }

  if (sendspin_player_stream_start(&format) != ESP_OK) {
    if (s_activity_cb) {
      s_activity_cb(false);
    }
    return;
  }

  sendspin_events_connected(true);
  sendspin_events_playing(true);
}

static bool sendspin_role_selected(const cJSON *payload) {
  /* `roles` is optional and means "all active roles" when absent. */
  const cJSON *roles = cJSON_GetObjectItemCaseSensitive(payload, "roles");
  if (!cJSON_IsArray(roles)) {
    return true;
  }
  const cJSON *role = NULL;
  cJSON_ArrayForEach(role, roles) {
    if (cJSON_IsString(role) &&
        strcmp(cJSON_GetStringValue(role), "player@v1") == 0) {
      return true;
    }
  }
  return false;
}

/* server/activate declares what the connection is for. An empty activity set
 * is not an error: it is how a server parks a device whose operator has not
 * approved it yet, and the protocol's answer to that is to sit still, keep
 * the clock synchronised and wait. */
static void sendspin_handle_activate(const cJSON *payload) {
  const cJSON *activities =
      cJSON_GetObjectItemCaseSensitive(payload, "activities");
  const cJSON *roles =
      cJSON_GetObjectItemCaseSensitive(payload, "active_roles");

  bool playback = false;
  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, activities) {
    if (cJSON_IsString(item) &&
        strcmp(cJSON_GetStringValue(item), "playback") == 0) {
      playback = true;
    }
  }

  /* active_roles is required on the first activation and persists across
   * later ones that omit it, so re-deriving it every time would silently
   * drop the roles the moment the server changed only its activity set. */
  if (cJSON_IsArray(roles)) {
    s_role_player = false;
    s_role_metadata = false;
    s_role_controller = false;
    cJSON_ArrayForEach(item, roles) {
      if (!cJSON_IsString(item)) {
        continue;
      }
      const char *role = cJSON_GetStringValue(item);
      if (strcmp(role, "player@v1") == 0) {
        s_role_player = true;
      } else if (strcmp(role, "metadata@v1") == 0) {
        s_role_metadata = true;
      } else if (strcmp(role, "controller@v1") == 0) {
        s_role_controller = true;
      }
    }
  }
  ESP_LOGI(TAG,
           "activated: playback=%s player@v1=%s metadata@v1=%s "
           "controller@v1=%s",
           playback ? "yes" : "no", s_role_player ? "yes" : "no",
           s_role_metadata ? "yes" : "no", s_role_controller ? "yes" : "no");

  s_state = SENDSPIN_ACTIVATED;
  /* The server must not send audio before this first report. */
  sendspin_send_state();
}

/* Volume is a perceived-loudness percentage, and the DAC drivers already
 * expand the firmware's -30..0 dB scale onto their own taper, so map straight
 * onto that scale rather than applying the spec's amplitude curve on top of a
 * curve. This is also the scale the buttons and the web UI report, which is
 * what keeps the three agreeing. */
static void sendspin_handle_command(const cJSON *payload) {
  const cJSON *player = cJSON_GetObjectItemCaseSensitive(payload, "player");
  if (!cJSON_IsObject(player)) {
    return;
  }
  const cJSON *command = cJSON_GetObjectItemCaseSensitive(player, "command");
  if (!cJSON_IsString(command)) {
    return;
  }
  const char *cmd = cJSON_GetStringValue(command);

  if (strcmp(cmd, "volume") == 0) {
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(player, "volume");
    if (!cJSON_IsNumber(volume)) {
      return;
    }
    int pct = (int)cJSON_GetNumberValue(volume);
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    playback_control_set_volume_percent(pct);
  } else if (strcmp(cmd, "mute") == 0) {
    const cJSON *mute = cJSON_GetObjectItemCaseSensitive(player, "mute");
    if (!cJSON_IsBool(mute)) {
      return;
    }
    playback_control_set_muted(cJSON_IsTrue(mute));
  } else {
    ESP_LOGD(TAG, "ignoring unsupported command %s", cmd);
    return;
  }

  /* "State updates must be sent whenever any state changes, including when
   * the volume was changed through a server/command." */
  s_state_dirty = true;
}

static void sendspin_handle_message(const char *json, size_t len,
                                    int64_t arrival_us) {
  cJSON *root = cJSON_ParseWithLength(json, len);
  if (!root) {
    ESP_LOGW(TAG, "unparseable message (%u bytes)", (unsigned)len);
    return;
  }

  const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
  const char *type = cJSON_IsString(type_item) ? type_item->valuestring : NULL;
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  if (!type) {
    cJSON_Delete(root);
    return;
  }

  if (strcmp(type, "server/time") != 0) {
    ESP_LOGD(TAG, "rx %s", type);
  }

  /* Only the two cleartext handshake messages are legal before the split.
   * Anything else arriving in the clear is a downgrade attempt, whatever it
   * claims to be. */
  const bool handshake_msg =
      strcmp(type, "server/init") == 0 || strcmp(type, "noise/handshake") == 0;
  if (!sendspin_noise_ready(&s_noise) && !handshake_msg) {
    ESP_LOGW(TAG, "%s arrived before the handshake completed", type);
    sendspin_session_close("out-of-order message");
    cJSON_Delete(root);
    return;
  }

  if (strcmp(type, "server/init") == 0) {
    /* The prologue is the bytes as received, so it is captured before the
     * parse rather than rebuilt from it. */
    if (!sendspin_prologue_append(json, len)) {
      sendspin_session_close("server/init too large");
    } else {
      sendspin_handle_server_init(payload);
    }
  } else if (strcmp(type, "noise/handshake") == 0) {
    sendspin_handle_noise_handshake(payload);
  } else if (strcmp(type, "server/hello") == 0) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(payload, "name");
    ESP_LOGI(TAG, "server \"%s\"",
             cJSON_IsString(name) ? cJSON_GetStringValue(name) : "?");
    s_state = SENDSPIN_READY;
    sendspin_send_hello();
  } else if (strcmp(type, "server/activate") == 0) {
    sendspin_handle_activate(payload);
  } else if (strcmp(type, "server/state") == 0) {
    sendspin_handle_server_state(payload);
  } else if (strcmp(type, "server/command") == 0) {
    sendspin_handle_command(payload);
  } else if (strcmp(type, "server/time") == 0) {
    sendspin_handle_server_time(payload, arrival_us);
  } else if (strcmp(type, "stream/start") == 0) {
    sendspin_handle_stream_start(payload);
  } else if (strcmp(type, "stream/clear") == 0) {
    if (sendspin_role_selected(payload)) {
      sendspin_player_stream_clear();
    }
  } else if (strcmp(type, "stream/end") == 0) {
    if (sendspin_role_selected(payload) && sendspin_player_is_streaming()) {
      sendspin_player_stream_end();
      sendspin_events_connected(false);
      if (s_activity_cb) {
        s_activity_cb(false);
      }
    }
  }

  cJSON_Delete(root);
}

/* Audio chunk: [4][timestamp:8 BE][send_ahead:4 BE][encoded audio]. */
static int64_t sendspin_read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return (int64_t)v;
}

static void sendspin_handle_binary(const uint8_t *data, size_t len,
                                   int64_t arrival_us);

/* Fragmented message: [1][flags][orig_type on the first only][data].
 * flags bit 1 = first fragment, bit 0 = last. */
static void sendspin_handle_fragment(const uint8_t *data, size_t len,
                                     int64_t arrival_us) {
  if (len < 2 || !s_asm) {
    return;
  }
  const uint8_t flags = data[1];
  const bool first = (flags & 0x02U) != 0U;
  const bool last = (flags & 0x01U) != 0U;
  if ((flags & 0xFCU) != 0U) {
    sendspin_session_close("reserved fragment flags set");
    return;
  }

  size_t offset = 2;
  if (first) {
    if (len < 3) {
      sendspin_session_close("truncated first fragment");
      return;
    }
    s_asm_type = data[2];
    if (s_asm_type == SENDSPIN_BIN_FRAGMENT) {
      sendspin_session_close("nested fragmentation");
      return;
    }
    offset = 3;
    s_asm_len = 0;
    s_asm_active = true;
    s_asm[s_asm_len++] = s_asm_type;
  } else if (!s_asm_active) {
    sendspin_session_close("continuation without a first fragment");
    return;
  }

  const size_t chunk = len - offset;
  if (s_asm_len + chunk > (size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE) {
    ESP_LOGW(TAG, "reassembly overflow, dropping message");
    s_asm_active = false;
    s_asm_len = 0;
    return;
  }
  memcpy(&s_asm[s_asm_len], &data[offset], chunk);
  s_asm_len += chunk;

  if (last) {
    s_asm_active = false;
    sendspin_handle_binary(s_asm, s_asm_len, arrival_us);
    s_asm_len = 0;
  }
}

static void sendspin_handle_binary(const uint8_t *data, size_t len,
                                   int64_t arrival_us) {
  if (len < 1) {
    return;
  }

  switch (data[0]) {
  case SENDSPIN_BIN_JSON:
    sendspin_handle_message((const char *)&data[1], len - 1, arrival_us);
    break;

  case SENDSPIN_BIN_FRAGMENT:
    sendspin_handle_fragment(data, len, arrival_us);
    break;

  case SENDSPIN_BIN_AUDIO_CHUNK:
    if (len > SENDSPIN_AUDIO_HEADER) {
      /* Bytes 9-12 are send_ahead, which the spec explicitly says carries no
       * scheduling meaning; only the timestamp places the audio. */
      sendspin_player_chunk(sendspin_read_be64(&data[1]),
                            &data[SENDSPIN_AUDIO_HEADER],
                            len - SENDSPIN_AUDIO_HEADER);
    }
    break;

  default:
    ESP_LOGD(TAG, "ignoring binary type %u", (unsigned)data[0]);
    break;
  }
}

/* ------------------------------------------------------------------ */
/*  WebSocket endpoint                                                 */
/* ------------------------------------------------------------------ */

/* The server answers the WebSocket handshake itself and deliberately does not
 * invoke the URI handler for it, so this is the only place a real client's
 * arrival can be observed. A plain GET still reaches the handler below, which
 * is why opening the session cannot live there: a bare HTTP probe would claim
 * a session that no WebSocket is behind. */
static esp_err_t sendspin_ws_connected(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);
  if (s_fd >= 0 && s_fd != fd && sendspin_fd_is_live(s_fd)) {
    /* One server at a time. The protocol's own answer to a second one is
     * client/goodbye with reason another_server; dropping the newcomer is
     * the conservative version of that for a device with three sockets.
     * Only an incumbent that is demonstrably still connected gets to win,
     * or a dead one would lock the endpoint out until a reboot. */
    ESP_LOGW(TAG, "rejecting a second server on fd=%d", fd);
    return ESP_FAIL;
  }
  if (s_fd >= 0 && s_fd != fd) {
    sendspin_session_close("replaced by a new server");
  }
  ESP_LOGI(TAG, "server connected on fd=%d", fd);
  sendspin_time_reset(&s_clock);
  sendspin_noise_reset(&s_noise);
  s_prologue_len = 0;
  s_asm_active = false;
  s_asm_len = 0;
  s_reported_available = false;
  s_fd = fd;
  /* client/init is sent from the housekeeping task rather than here, so that
   * it cannot race the handshake response onto the socket. */
  s_state = SENDSPIN_NEED_INIT;
  return ESP_OK;
}

static esp_err_t sendspin_ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "WebSocket upgrade required");
  }

  /* Timestamped before anything else in the handler so the clock estimate
   * measures the network, not our own dispatch. */
  const int64_t arrival_us = esp_timer_get_time();

  httpd_ws_frame_t frame = {0};
  if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
    return ESP_OK;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    sendspin_session_close("peer closed");
    return ESP_OK;
  }

  if (frame.len == 0) {
    return ESP_OK;
  }
  if (frame.len > (size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE) {
    /* The payload cannot be consumed, and leaving it in the socket
     * desynchronises every frame after it (see log_stream.c), so the session
     * has to go rather than the message. */
    ESP_LOGE(TAG, "frame of %u bytes exceeds the receive buffer",
             (unsigned)frame.len);
    sendspin_session_close("frame too large");
    return ESP_FAIL;
  }

  frame.payload = s_rx;
  if (httpd_ws_recv_frame(req, &frame,
                          (size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE) != ESP_OK) {
    return ESP_OK;
  }

  if (frame.type == HTTPD_WS_TYPE_TEXT) {
    if (sendspin_noise_ready(&s_noise)) {
      ESP_LOGE(TAG, "cleartext frame after the handshake");
      sendspin_session_close("cleartext in transport mode");
      return ESP_OK;
    }
    sendspin_handle_message((const char *)s_rx, frame.len, arrival_us);
  } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
    if (!sendspin_noise_ready(&s_noise)) {
      ESP_LOGE(TAG, "binary frame before the handshake");
      sendspin_session_close("unencrypted binary frame");
      return ESP_OK;
    }
    size_t plain_len = 0;
    /* A single AEAD failure is terminal by design: the nonce counters have
     * diverged, so nothing after this frame would decrypt either. */
    if (sendspin_noise_decrypt(&s_noise, s_rx, frame.len, s_pt,
                               (size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                               &plain_len) != ESP_OK) {
      ESP_LOGE(TAG, "transport decryption failed on a %u byte frame",
               (unsigned)frame.len);
      sendspin_session_close("decryption failed");
      return ESP_OK;
    }
    sendspin_handle_binary(s_pt, plain_len, arrival_us);
  }
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Housekeeping                                                       */
/* ------------------------------------------------------------------ */

static void sendspin_advertise(void) {
  if (s_mdns_advertised) {
    return;
  }
  /* mdns_init() is a no-op once the AirPlay advertisement has run, and works
   * standalone when it has not (Sendspin is registered before AirPlay
   * starts). */
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return;
  }

  char name[65];
  settings_get_device_name(name, sizeof(name));

  mdns_txt_item_t txt[] = {
      {"path", SENDSPIN_WS_PATH},
      {"name", name},
  };
  err = mdns_service_add(name, "_sendspin", "_tcp", 80, txt,
                         sizeof(txt) / sizeof(txt[0]));
  if (err != ESP_OK) {
    /* Expected until something sets the mDNS hostname, which the AirPlay
     * advertisement does after us. The caller retries every tick, so the
     * signal to watch for is the absence of the success line below. */
    ESP_LOGD(TAG, "_sendspin._tcp not advertised yet: %s",
             esp_err_to_name(err));
    return;
  }
  s_mdns_advertised = true;
  ESP_LOGI(TAG, "_sendspin._tcp advertised on port 80, path " SENDSPIN_WS_PATH);
}

static void sendspin_task(void *arg) {
  (void)arg;

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(SENDSPIN_TICK_MS));

    if (!s_mdns_advertised &&
        (ethernet_is_connected() || wifi_is_connected())) {
      sendspin_advertise();
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }

    /* Reap a session that went away without saying so, before acting on it. */
    if (s_fd >= 0 && !sendspin_fd_is_live(s_fd)) {
      sendspin_session_close("socket gone");
    }

    switch (s_state) {
    case SENDSPIN_IDLE:
      break;

    case SENDSPIN_NEED_INIT:
      sendspin_send_init();
      s_state = SENDSPIN_INIT_SENT;
      break;

    case SENDSPIN_INIT_SENT:
    case SENDSPIN_HANDSHAKE:
    case SENDSPIN_ENCRYPTED:
    case SENDSPIN_READY:
      /* Nothing may leave the client between client/init and the first
       * server/activate -- not even a clock request. */
      break;

    case SENDSPIN_ACTIVATED: {
      const int64_t now_us = esp_timer_get_time();
      const uint32_t interval_ms =
          sendspin_time_converged(&s_clock)
              ? (uint32_t)CONFIG_SENDSPIN_TIME_SYNC_INTERVAL_MS
              : SENDSPIN_TIME_BURST_MS;
      if (now_us - s_last_time_tx_us >= (int64_t)interval_ms * 1000LL) {
        sendspin_send_time_request();
      }
      const bool available =
          s_output_available && sendspin_time_converged(&s_clock);
      if (s_state_dirty || available != s_reported_available ||
          playback_control_get_level_percent() != s_reported_volume ||
          playback_control_is_muted() != s_reported_muted) {
        sendspin_send_state();
      }
      sendspin_meta_tick();
      sendspin_command_tick();
      break;
    }
    }

    xSemaphoreGive(s_lock);
  }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t sendspin_init(sendspin_activity_cb_t callback) {
  if (s_rx) {
    return ESP_OK;
  }

  if (sodium_init() < 0) {
    ESP_LOGE(TAG, "libsodium init failed");
    return ESP_FAIL;
  }

  s_lock = xSemaphoreCreateMutex();
  s_tx_lock = xSemaphoreCreateMutex();
  s_cmd_queue = xQueueCreate(4, sizeof(uint8_t));
  if (!s_lock || !s_tx_lock || !s_cmd_queue) {
    return ESP_ERR_NO_MEM;
  }

  s_rx = heap_caps_malloc((size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_asm = heap_caps_malloc((size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_pt = heap_caps_malloc((size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_tx_plain = heap_caps_malloc(SENDSPIN_TX_PLAIN_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_tx_cipher = heap_caps_malloc(SENDSPIN_TX_PLAIN_MAX + SENDSPIN_NOISE_TAG_LEN,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rx || !s_asm || !s_pt || !s_tx_plain || !s_tx_cipher) {
    free(s_rx);
    free(s_asm);
    free(s_pt);
    free(s_tx_plain);
    free(s_tx_cipher);
    s_rx = NULL;
    s_asm = NULL;
    s_pt = NULL;
    s_tx_plain = NULL;
    s_tx_cipher = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    vSemaphoreDelete(s_tx_lock);
    s_tx_lock = NULL;
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = sendspin_load_identity();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "identity setup failed: %s", esp_err_to_name(err));
    return err;
  }

  sendspin_time_reset(&s_clock);
  err = sendspin_player_init(&s_clock);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "player init failed: %s", esp_err_to_name(err));
    return err;
  }

  s_activity_cb = callback;
  ESP_LOGI(TAG, "client_id %s", s_client_id);
  return ESP_OK;
}

esp_err_t sendspin_register(httpd_handle_t server) {
  if (!s_rx) {
    return ESP_ERR_INVALID_STATE;
  }
  s_server = server;

  httpd_uri_t ws_uri = {
      .uri = SENDSPIN_WS_PATH,
      .method = HTTP_GET,
      .handler = sendspin_ws_handler,
      .is_websocket = true,
      .ws_post_handshake_cb = sendspin_ws_connected,
  };
  esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to register " SENDSPIN_WS_PATH ": %s",
             esp_err_to_name(err));
    return err;
  }

  if (!s_task) {
    task_create_spiram(sendspin_task, "sendspin", SENDSPIN_TASK_STACK, NULL, 4,
                       &s_task, NULL);
  }
  ESP_LOGI(TAG, "listening on " SENDSPIN_WS_PATH);
  return ESP_OK;
}

bool sendspin_is_streaming(void) {
  return sendspin_player_is_streaming();
}

void sendspin_set_output_available(bool available) {
  if (s_output_available == available) {
    return;
  }
  s_output_available = available;
  ESP_LOGI(TAG, "output %s", available ? "released to Sendspin" : "taken over");

  if (!available && sendspin_player_is_streaming()) {
    /* Stop before reporting: the server tears the stream down on
     * available:false anyway, and leaving the renderer attached would race
     * whoever is taking the output. */
    sendspin_player_stream_end();
  }
  s_state_dirty = true;
}

bool sendspin_send_command(sendspin_command_t cmd) {
  if (!s_cmd_queue || !s_role_controller || cmd >= SENDSPIN_CMD_COUNT) {
    return false;
  }
  if ((s_ctrl_commands & (1U << (uint32_t)cmd)) == 0) {
    return false;
  }
  const uint8_t item = (uint8_t)cmd;
  return xQueueSend(s_cmd_queue, &item, 0) == pdTRUE;
}

bool sendspin_is_playing(void) {
  return s_events_playing;
}
