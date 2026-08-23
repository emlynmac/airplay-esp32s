#pragma once

#include <stddef.h>
#include <stdint.h>

/* Immutable metadata carried with one compressed audio access unit from
 * ingress through decrypt/decode.  Keeping the epoch beside the RTP timestamp
 * closes the gap where a seek/flush can occur after a packet passes the RTP
 * gate but before its decoder work completes. */
typedef struct {
  uint32_t epoch;
  uint32_t rtp_timestamp;
  const uint8_t *payload;
  size_t payload_len;
} audio_encoded_packet_t;
