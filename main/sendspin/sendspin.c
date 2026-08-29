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
#include "mdns.h"
#include "nvs.h"
#include "sodium.h"

#include "ethernet.h"
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

typedef enum {
  SENDSPIN_IDLE = 0,  /* no socket */
  SENDSPIN_NEED_INIT, /* socket up, client/init not sent yet */
  SENDSPIN_INIT_SENT, /* waiting for server/init */
  SENDSPIN_READY,     /* hello exchanged; clock sync running */
  SENDSPIN_ACTIVATED, /* server/activate seen; state reported */
} sendspin_state_t;

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;

static volatile int s_fd = -1;
static volatile sendspin_state_t s_state = SENDSPIN_IDLE;

static sendspin_time_t s_clock;
static sendspin_activity_cb_t s_activity_cb = NULL;

/* Availability has two independent inputs: whether another source owns the
 * output, and whether the clock estimate is good enough to place audio. The
 * protocol only has one flag, so it is the AND of the two. */
static bool s_output_available = true;
static bool s_reported_available = false;
static bool s_state_dirty = false;

static uint8_t *s_rx = NULL;  /* one WebSocket frame */
static uint8_t *s_asm = NULL; /* fragment reassembly */
static size_t s_asm_len = 0;
static uint8_t s_asm_type = 0;
static bool s_asm_active = false;

static char s_client_id[SENDSPIN_CLIENT_ID_LEN + 1];
static bool s_mdns_advertised = false;
static int64_t s_last_time_tx_us = 0;

/* ------------------------------------------------------------------ */
/*  Identity                                                           */
/* ------------------------------------------------------------------ */

/* client_id is the base64url of a Curve25519 public key. Nothing in this
 * milestone uses the private half, but generating a real keypair now means
 * the identity the server pins today is the same one the Noise handshake
 * will authenticate later, instead of changing under it. */
static esp_err_t sendspin_load_identity(void) {
  uint8_t sk[crypto_scalarmult_curve25519_BYTES];
  uint8_t pk[crypto_scalarmult_curve25519_BYTES];

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  size_t len = sizeof(sk);
  err = nvs_get_blob(nvs, SENDSPIN_NVS_KEY_SK, sk, &len);
  if (err != ESP_OK || len != sizeof(sk)) {
    randombytes_buf(sk, sizeof(sk));
    err = nvs_set_blob(nvs, SENDSPIN_NVS_KEY_SK, sk, sizeof(sk));
    if (err == ESP_OK) {
      err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "identity not persisted: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "generated a new client identity");
  }
  nvs_close(nvs);

  if (crypto_scalarmult_curve25519_base(pk, sk) != 0) {
    sodium_memzero(sk, sizeof(sk));
    return ESP_FAIL;
  }
  sodium_memzero(sk, sizeof(sk));

  sodium_bin2base64(s_client_id, sizeof(s_client_id), pk, sizeof(pk),
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
  esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "send failed on fd=%d: %s", fd, esp_err_to_name(err));
    return false;
  }
  return true;
}

/* Serialises and sends `root`, then frees it. Returns false if the socket has
 * gone; the caller treats that as a disconnect rather than an error. */
static bool sendspin_send_json(cJSON *root) {
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

static void sendspin_send_init(void) {
  cJSON *payload = NULL;
  cJSON *root = sendspin_new_message("client/init", &payload);
  if (!root) {
    return;
  }
  cJSON_AddStringToObject(payload, "client_id", s_client_id);
  cJSON_AddNumberToObject(payload, "version", 1);
  cJSON_AddStringToObject(payload, "suite", "25519_ChaChaPoly_SHA256");
  (void)sendspin_send_json(root);
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
    cJSON_AddStringToObject(info, "product_name", "ESP32 AirPlay Receiver");
    cJSON_AddStringToObject(info, "manufacturer", "airplay-contrib");
    cJSON_AddStringToObject(info, "software_version",
                            esp_app_get_description()->version);
    cJSON_AddStringToObject(info, "mac_address", mac_str);
  }

  cJSON *roles = cJSON_AddArrayToObject(payload, "supported_roles");
  if (roles) {
    cJSON_AddItemToArray(roles, cJSON_CreateString("player@v1"));
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
    /* Empty: volume is owned by the device's own UI and buttons in this
     * milestone, so the server must not expect to drive it. */
    cJSON_AddArrayToObject(support, "supported_commands");
  }

  cJSON_AddArrayToObject(payload, "supported_pair_methods");
  cJSON *unpaired = cJSON_AddObjectToObject(payload, "unpaired_access");
  if (unpaired) {
    cJSON_AddBoolToObject(unpaired, "enabled", true);
  }

  (void)sendspin_send_json(root);
}

static void sendspin_send_state(void) {
  const bool available =
      s_output_available && sendspin_time_converged(&s_clock);

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
    cJSON_AddArrayToObject(player, "supported_commands");
  }

  if (sendspin_send_json(root)) {
    s_reported_available = available;
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
  sendspin_time_reset(&s_clock);

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
  }
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

  if (strcmp(type, "server/init") == 0) {
    s_state = SENDSPIN_READY;
    sendspin_send_hello();
  } else if (strcmp(type, "noise/handshake") == 0) {
    ESP_LOGE(TAG, "server requires the Noise transport, which this build does "
                  "not implement");
    sendspin_session_close("encryption unsupported");
  } else if (strcmp(type, "server/hello") == 0) {
    /* The spec has the server greet first; tolerate either order. */
    if (s_state < SENDSPIN_READY) {
      s_state = SENDSPIN_READY;
      sendspin_send_hello();
    }
  } else if (strcmp(type, "server/activate") == 0) {
    s_state = SENDSPIN_ACTIVATED;
    /* The server must not send audio before this first report. */
    sendspin_send_state();
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
      if (s_activity_cb) {
        s_activity_cb(false);
      }
    }
  } else if (strcmp(type, "client/goodbye") == 0) {
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(payload, "reason");
    ESP_LOGI(TAG, "server said goodbye: %s",
             cJSON_IsString(reason) ? cJSON_GetStringValue(reason) : "?");
    sendspin_session_close("goodbye");
  }

  cJSON_Delete(root);
}

/* Audio chunk: [4][timestamp:8 BE][send_ahead:4 BE][encoded audio]. */
#define SENDSPIN_BIN_JSON        0
#define SENDSPIN_BIN_FRAGMENT    1
#define SENDSPIN_BIN_AUDIO_CHUNK 4
#define SENDSPIN_AUDIO_HEADER    13

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

static esp_err_t sendspin_ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
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
    s_asm_active = false;
    s_asm_len = 0;
    s_reported_available = false;
    s_fd = fd;
    /* client/init is sent from the housekeeping task rather than here: the
     * handshake response has not been written yet at this point. */
    s_state = SENDSPIN_NEED_INIT;
    return ESP_OK;
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
    sendspin_handle_message((const char *)s_rx, frame.len, arrival_us);
  } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
    sendspin_handle_binary(s_rx, frame.len, arrival_us);
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
      break;

    case SENDSPIN_READY:
    case SENDSPIN_ACTIVATED: {
      const int64_t now_us = esp_timer_get_time();
      const uint32_t interval_ms =
          sendspin_time_converged(&s_clock)
              ? (uint32_t)CONFIG_SENDSPIN_TIME_SYNC_INTERVAL_MS
              : SENDSPIN_TIME_BURST_MS;
      if (now_us - s_last_time_tx_us >= (int64_t)interval_ms * 1000LL) {
        sendspin_send_time_request();
      }
      if (s_state == SENDSPIN_ACTIVATED) {
        const bool available =
            s_output_available && sendspin_time_converged(&s_clock);
        if (s_state_dirty || available != s_reported_available) {
          sendspin_send_state();
        }
      }
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
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }

  s_rx = heap_caps_malloc((size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_asm = heap_caps_malloc((size_t)CONFIG_SENDSPIN_RX_BUFFER_SIZE,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rx || !s_asm) {
    free(s_rx);
    free(s_asm);
    s_rx = NULL;
    s_asm = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
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
