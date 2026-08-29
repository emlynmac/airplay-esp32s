#include "sendspin_time.h"

#include <string.h>

/* An exchange is only useful if it was not delayed. Anything slower than this
 * multiple of the best round trip seen so far carries an unknown amount of
 * one-sided queueing delay, which biases the offset by half of it. */
#define SENDSPIN_TIME_RTT_TOLERANCE 2

/* ...but on a quiet LAN the best RTT can be a few hundred microseconds, and
 * then the multiple above rejects almost everything. Always allow this much
 * absolute slack on top. */
#define SENDSPIN_TIME_RTT_SLACK_US 2000LL

/* Largest rate error the fit will report. Two crystals within spec differ by
 * well under 200 ppm; anything larger means the regression is fitting noise
 * or a step in the server's clock, and extrapolating it would be worse than
 * assuming the clocks run at the same rate. */
#define SENDSPIN_TIME_MAX_SKEW_PPM 300.0

void sendspin_time_reset(sendspin_time_t *t) {
  if (!t) {
    return;
  }
  memset(t, 0, sizeof(*t));
}

/* Least squares over the window. Both axes are taken relative to the oldest
 * retained sample so the doubles stay small: a monotonic clock that has been
 * up for a week is ~6e11 us, and squaring that loses the resolution the fit
 * is trying to find. */
static void sendspin_time_refit(sendspin_time_t *t) {
  int64_t origin_us = t->samples[0].local_us;
  int64_t oldest_us = origin_us;
  int64_t newest_us = origin_us;
  for (uint8_t i = 1; i < t->count; i++) {
    if (t->samples[i].local_us < oldest_us) {
      oldest_us = t->samples[i].local_us;
    }
    if (t->samples[i].local_us > newest_us) {
      newest_us = t->samples[i].local_us;
    }
  }
  origin_us = oldest_us;

  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_xx = 0.0;
  double sum_xy = 0.0;
  const double n = (double)t->count;
  const int64_t origin_offset = t->samples[0].offset_us;

  for (uint8_t i = 0; i < t->count; i++) {
    const double x = (double)(t->samples[i].local_us - origin_us);
    const double y = (double)(t->samples[i].offset_us - origin_offset);
    sum_x += x;
    sum_y += y;
    sum_xx += x * x;
    sum_xy += x * y;
  }

  double skew = 0.0;
  const double denom = (n * sum_xx) - (sum_x * sum_x);
  const int64_t span_us = newest_us - oldest_us;
  if (denom > 0.0 && span_us >= SENDSPIN_TIME_MIN_SPAN_US &&
      t->count >= SENDSPIN_TIME_MIN_SAMPLES) {
    skew = ((n * sum_xy) - (sum_x * sum_y)) / denom;
    const double limit = SENDSPIN_TIME_MAX_SKEW_PPM / 1e6;
    if (skew > limit) {
      skew = limit;
    } else if (skew < -limit) {
      skew = -limit;
    }
  }

  /* Anchor the fit at the newest sample rather than at the intercept: the
   * renderer only ever evaluates it at or slightly after "now", so keeping
   * the lever arm short bounds the error the skew term can contribute. */
  const double mean_x = sum_x / n;
  const double mean_y = sum_y / n;
  const double at_newest =
      mean_y + (skew * ((double)(newest_us - origin_us) - mean_x));

  t->base_us = newest_us;
  t->base_offset_us = origin_offset + (int64_t)at_newest;
  t->skew = skew;
  t->valid = t->count >= SENDSPIN_TIME_MIN_SAMPLES;
}

bool sendspin_time_update(sendspin_time_t *t, int64_t client_transmitted,
                          int64_t server_received, int64_t server_transmitted,
                          int64_t client_received) {
  if (!t) {
    return false;
  }

  const int64_t rtt_us = (client_received - client_transmitted) -
                         (server_transmitted - server_received);
  if (rtt_us < 0 || client_received < client_transmitted) {
    /* Either clock stepped mid-exchange, or the server echoed a stale
     * client_transmitted. Neither is a usable measurement. */
    t->rejected++;
    return false;
  }

  /* Standard four-timestamp offset, which assumes the two one-way delays are
   * equal. That assumption is exactly why the outlier gate below matters. */
  const int64_t offset_us = ((server_received - client_transmitted) +
                             (server_transmitted - client_received)) /
                            2;
  const int64_t midpoint_us = client_transmitted + ((rtt_us) / 2);

  if (t->best_rtt_us == 0 || rtt_us < t->best_rtt_us) {
    t->best_rtt_us = rtt_us;
  } else if (t->count > 0 &&
             rtt_us > (t->best_rtt_us * SENDSPIN_TIME_RTT_TOLERANCE) +
                          SENDSPIN_TIME_RTT_SLACK_US) {
    t->rejected++;
    return false;
  }

  const sendspin_time_sample_t sample = {
      .local_us = midpoint_us,
      .offset_us = offset_us,
      .rtt_us = rtt_us,
  };

  if (t->count < SENDSPIN_TIME_WINDOW) {
    t->samples[t->count++] = sample;
  } else {
    t->samples[t->next] = sample;
    t->next = (uint8_t)((t->next + 1U) % SENDSPIN_TIME_WINDOW);
  }
  t->accepted++;

  sendspin_time_refit(t);
  return true;
}

bool sendspin_time_converged(const sendspin_time_t *t) {
  return t && t->valid;
}

bool sendspin_time_offset_ns(const sendspin_time_t *t, int64_t local_us,
                             int64_t *offset_ns) {
  if (!t || !t->valid || !offset_ns) {
    return false;
  }
  const double elapsed_us = (double)(local_us - t->base_us);
  const double offset_us = (double)t->base_offset_us + (t->skew * elapsed_us);
  *offset_ns = (int64_t)(offset_us * 1000.0);
  return true;
}

int32_t sendspin_time_skew_ppm(const sendspin_time_t *t) {
  if (!t || !t->valid) {
    return 0;
  }
  return (int32_t)(t->skew * 1e6);
}

int64_t sendspin_time_best_rtt_us(const sendspin_time_t *t) {
  return t ? t->best_rtt_us : 0;
}
