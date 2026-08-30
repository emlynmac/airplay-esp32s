#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file sendspin_noise.h
 * @brief Noise KKpsk2 transport for Sendspin, responder side.
 *
 * Sendspin encrypts every connection; there is no cleartext mode.  The
 * handshake is `Noise_KKpsk2_25519_ChaChaPoly_SHA256`, in which the *server*
 * is the Noise initiator and the client the responder, regardless of which
 * side opened the WebSocket:
 *
 *     client -> server  client/init      (cleartext, carries our client_id)
 *     server -> client  server/init      (cleartext, carries its server_id)
 *     server -> client  noise/handshake  message 1: e, es, ss
 *     client -> server  noise/handshake  message 2: e, ee, se, psk
 *     ...both sides switch to transport mode...
 *
 * Both static keys are known in advance -- they are the two ids just
 * exchanged -- so the handshake authenticates whatever the pre-shared key
 * authenticates, and nothing more.
 *
 * This build only ever uses the Sentinel PSK, a constant published in the
 * specification.  That makes the session "unpaired": confidentiality and
 * replay protection hold, but neither peer's identity is proven, so a server
 * may only use an unpaired client for playback if its operator has approved
 * it.  Pairing (which would replace the Sentinel with a secret long-term PSK)
 * is not implemented, and the client advertises no pairing methods.
 *
 * The one-line summary of why the layering matters: the handshake messages
 * are cleartext JSON in WebSocket *text* frames, and everything after it is a
 * Noise ciphertext in a WebSocket *binary* frame whose first plaintext byte
 * is the Sendspin message type.
 */

#define SENDSPIN_NOISE_KEY_LEN    32
#define SENDSPIN_NOISE_HASH_LEN   32
#define SENDSPIN_NOISE_TAG_LEN    16
#define SENDSPIN_NOISE_PSK_ID_LEN 43 /* base64url of a 32-byte hash */

/** One direction of the transport: a key plus its nonce counter. */
typedef struct {
  uint8_t k[SENDSPIN_NOISE_KEY_LEN];
  uint64_t n;
  bool has_key;
} sendspin_noise_cipher_t;

typedef struct {
  /* SymmetricState */
  uint8_t ck[SENDSPIN_NOISE_HASH_LEN];
  uint8_t h[SENDSPIN_NOISE_HASH_LEN];
  sendspin_noise_cipher_t handshake_cipher;

  /* HandshakeState */
  uint8_t s_priv[SENDSPIN_NOISE_KEY_LEN];
  uint8_t s_pub[SENDSPIN_NOISE_KEY_LEN];
  uint8_t e_priv[SENDSPIN_NOISE_KEY_LEN];
  uint8_t e_pub[SENDSPIN_NOISE_KEY_LEN];
  uint8_t rs[SENDSPIN_NOISE_KEY_LEN]; /* server static */
  uint8_t re[SENDSPIN_NOISE_KEY_LEN]; /* server ephemeral */

  /* Transport. The client is the responder, so it decrypts with the
   * initiator-to-responder cipher and encrypts with the other one. */
  sendspin_noise_cipher_t recv;
  sendspin_noise_cipher_t send;
  bool transport;
} sendspin_noise_t;

/** The published Sentinel PSK and its psk_id, derived once at init. */
const uint8_t *sendspin_noise_sentinel_psk(void);
const char *sendspin_noise_sentinel_psk_id(void);

/**
 * Begin a handshake.
 *
 * @param prologue Exact wire bytes of client/init followed by server/init.
 *                 Both sides hash it, so any tampering with the cleartext
 *                 exchange makes the handshake fail.
 */
esp_err_t sendspin_noise_start(sendspin_noise_t *ns,
                               const uint8_t client_priv[32],
                               const uint8_t client_pub[32],
                               const uint8_t server_pub[32],
                               const uint8_t *prologue, size_t prologue_len);

/**
 * Consume Noise message 1 and decrypt its payload, which names the PSK the
 * server chose.  Readable without the PSK: it is only mixed in at the end of
 * message 2.
 */
esp_err_t sendspin_noise_read_message1(sendspin_noise_t *ns, const uint8_t *msg,
                                       size_t msg_len, uint8_t *payload_out,
                                       size_t payload_cap, size_t *payload_len);

/**
 * Produce Noise message 2 and switch to transport mode.  @p payload is the
 * literal two bytes "{}" per the specification.
 */
esp_err_t sendspin_noise_write_message2(sendspin_noise_t *ns,
                                        const uint8_t psk[32],
                                        const uint8_t *payload,
                                        size_t payload_len, uint8_t *out,
                                        size_t out_cap, size_t *out_len);

static inline bool sendspin_noise_ready(const sendspin_noise_t *ns) {
  return ns && ns->transport;
}

/** Seal one transport message. @p out needs @p len + 16 bytes. */
esp_err_t sendspin_noise_encrypt(sendspin_noise_t *ns, const uint8_t *plain,
                                 size_t len, uint8_t *out, size_t out_cap,
                                 size_t *out_len);

/** Open one transport message. A failure here is fatal to the session. */
esp_err_t sendspin_noise_decrypt(sendspin_noise_t *ns, const uint8_t *cipher,
                                 size_t len, uint8_t *out, size_t out_cap,
                                 size_t *out_len);

/** Wipe all key material. */
void sendspin_noise_reset(sendspin_noise_t *ns);
