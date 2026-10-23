// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Missing-packet bookkeeping for the realtime (UDP) stream.
 *
 * Every sequence number skipped among the last RTP_RESEND_WINDOW packets is
 * remembered until it turns up, retransmitted or late, or until its audio is
 * due to play, so it can be asked for again for as long as it could still be
 * heard.  The window is longer than any realtime buffer: 512 packets of 352
 * frames is 4.1 s at 44.1 kHz, where a sender runs about 2 s ahead.
 *
 * Bookkeeping only, with no clock or socket of its own.  The receive task
 * owns the tracker and sends the requests. */

#define RTP_RESEND_WINDOW 512U
// How long to wait for a requested packet before asking for it again.
#define RTP_RESEND_RETRY_MS 250U

typedef struct {
  uint16_t first;
  uint16_t count;
} rtp_resend_range_t;

typedef enum {
  RTP_RESEND_NEWEST,   // Newer than any packet before it; may open holes
  RTP_RESEND_FILLED,   // Filled a hole, retransmitted or out of order
  RTP_RESEND_UNWANTED, // A duplicate, or older than any hole still open
} rtp_resend_verdict_t;

// Running totals for the receive task's status line, which may clear them.
typedef struct {
  uint32_t received; // packets newer than any before them
  uint32_t skipped;  // sequence numbers that went missing
  uint32_t resent;   // holes filled by a retransmission
  uint32_t late;     // holes filled by the original packet, out of order
  uint32_t expired;  // holes given up on: due to play, or out of the window
  uint32_t unwanted; // duplicates, and packets nobody was waiting for
  uint32_t asked;    // packets asked for, each repeat counted again
  uint32_t requests; // requests made, one per run of packets
} rtp_resend_counts_t;

typedef struct {
  // Indexed by sequence number modulo the window.
  uint32_t missing[RTP_RESEND_WINDOW / 32U];
  uint32_t asked[RTP_RESEND_WINDOW / 32U];
  // When each hole was last asked for, in 32 ms ticks.
  uint8_t asked_tick[RTP_RESEND_WINDOW];
  bool valid;
  uint16_t newest_seq;
  uint32_t newest_rtp;
  // RTP frames per packet, taken from consecutive packets.
  uint32_t rtp_step;
  rtp_resend_counts_t counts;
} rtp_resend_t;

// Forgets everything.  rtp_step is the packet length to assume until two
// consecutive packets show the real one.
void rtp_resend_reset(rtp_resend_t *r, uint32_t rtp_step);

// Records a data packet or a retransmission and says whether to use it.
rtp_resend_verdict_t rtp_resend_on_packet(rtp_resend_t *r, uint16_t seq,
                                          uint32_t rtp, bool retransmit);

/* Gives up on the holes whose audio starts before cursor_rtp, when
 * cursor_valid, then writes up to max_ranges runs of holes to ask for now,
 * oldest first: those never asked for, and those last asked for at least
 * RTP_RESEND_RETRY_MS before now_ms.  Holes written count as asked at now_ms;
 * holes that do not fit wait for the next call. */
size_t rtp_resend_collect(rtp_resend_t *r, uint32_t now_ms, bool cursor_valid,
                          uint32_t cursor_rtp, rtp_resend_range_t *ranges,
                          size_t max_ranges);

// Holes still open.
uint32_t rtp_resend_outstanding(const rtp_resend_t *r);
