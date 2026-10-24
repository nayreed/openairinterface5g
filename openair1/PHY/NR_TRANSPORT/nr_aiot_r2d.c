/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_r2d.c
 * \brief An NR node as an Ambient IoT Reader: R2D transmissions on PRBs of the node's own grid. See
 * nr_aiot_r2d.h. */

#include "nr_aiot_r2d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "PHY/AIOT/aiot_phy.h"
#include "common/utils/LOG/log.h"

#define FRAMES 1024 /* SFN wraps at 1024 frames */

_Static_assert(NR_AIOT_R2D_MAX_SC == AIOT_N_FFT * 3 / 4, "NR_AIOT_R2D_MAX_SC is 3/4 of AIOT_N_FFT");

/* Slots from a to b, forward, with the SFN wrap; values over half the wrap mean b is before a. */
static int slot_diff(const nr_aiot_r2d_t *r2d, int a, int b)
{
  const int wrap = FRAMES * r2d->slots_per_frame;
  return ((b - a) % wrap + wrap) % wrap;
}

static int in_past(const nr_aiot_r2d_t *r2d, int a, int b)
{
  return slot_diff(r2d, a, b) > FRAMES * r2d->slots_per_frame / 2;
}

static int16_t sat16(double v)
{
  v = round(v);
  return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

int nr_aiot_r2d_init(nr_aiot_r2d_t *r2d,
                     const nr_aiot_r2d_config_t *cfg,
                     int slots_per_frame,
                     int symbols_per_slot,
                     int ofdm_symbol_size,
                     int n_prb,
                     int tx_amp)
{
  memset(r2d, 0, sizeof(*r2d));
  r2d->now_slot = -1;
  r2d->now_ts = -1;
  r2d->timed_slot = -1;
  r2d->timed_ts = -1;
  r2d->tx_slots = (1u << NR_AIOT_R2D_TX_SLOT_PERIOD) - 1;
  r2d->cfg = *cfg;
  r2d->cfg.prb_start = -1;
  /* the grid's sizes, also without R2D: the node's clock (nr_aiot_r2d_now()) runs on its slots */
  r2d->slots_per_frame = slots_per_frame;
  r2d->symbols_per_slot = symbols_per_slot;
  r2d->ofdm_symbol_size = ofdm_symbol_size;
  r2d->n_prb = n_prb;
  pthread_mutex_init(&r2d->lock, NULL);
  if (cfg->prb_start < 0 || cfg->n_rb_max == 0)
    return 0; /* no R2D: a gNB configured without one, or zeroed (the simulators) */
  if (slots_per_frame != 10 || symbols_per_slot != NR_AIOT_R2D_SYMBOLS_PER_SLOT) {
    LOG_E(NR_PHY, "A-IoT R2D needs 15 kHz subcarrier spacing (10 slots of 14 symbols per frame)\n");
    return -1;
  }
  if (cfg->n_rb_max < 1 || cfg->prb_start + cfg->n_rb_max > n_prb) {
    LOG_E(NR_PHY,
          "A-IoT R2D PRBs %d to %d are not in the carrier's %d PRBs\n",
          cfg->prb_start,
          cfg->prb_start + cfg->n_rb_max - 1,
          n_prb);
    return -1;
  }
  if (cfg->n_rb_max * 12 > NR_AIOT_R2D_MAX_SC) {
    LOG_E(NR_PHY, "A-IoT R2D on %d PRBs is wider than the 384-point symbol allows\n", cfg->n_rb_max);
    return -1;
  }
  /* An R2D resource element is at most on_amplitude * sqrt(K) (an all-on symbol has |X| <= 1 on every
   * subcarrier): it must fit 16 bits, so that no chip is ever clipped. */
  const double peak = tx_amp * pow(10.0, cfg->power_offset_db / 20.0) * sqrt(cfg->n_rb_max * 12.0);
  if (peak > INT16_MAX) {
    LOG_E(NR_PHY,
          "A-IoT R2D EPRE %+.1f dB over PDSCH or PUSCH on %d PRBs would clip: at most %+.1f dB\n",
          cfg->power_offset_db,
          cfg->n_rb_max,
          cfg->power_offset_db - 20.0 * log10(peak / INT16_MAX));
    return -1;
  }
  r2d->cfg = *cfg;
  /* aiot_r2d_resource_elements() gives a waveform whose "on" chips are about 1 and whose mean power,
   * with half the chips on, is about 1/2 over its K subcarriers, i.e. 1 / (2K) per resource element.
   * A QPSK resource element of the data channel has power tx_amp^2 / 2. The R2D's own K is used at
   * scheduling time. */
  r2d->on_amplitude = tx_amp * pow(10.0, cfg->power_offset_db / 20.0);
  if (cfg->carrier) {
    /* A tone on subcarrier k (from DC) is exp(j 2 pi k t / N) at sample t of the slot; symbol s's
     * useful part starts at s N plus the cyclic prefixes up to it (N 160/2048 for symbols 0 and 7,
     * N 144/2048 for the others), and a slot is 15 N samples, so the phase repeats every slot. */
    r2d->carrier_sc = 12 * cfg->prb_start + 6 * cfg->n_rb_max;
    const int k = r2d->carrier_sc - 6 * n_prb, N = ofdm_symbol_size;
    const double a = r2d->on_amplitude * sqrt(12.0 * cfg->n_rb_max);
    int64_t start = 0;
    for (int s = 0; s < symbols_per_slot; s++) {
      start += (s % 7 == 0 ? 160 : 144) * N / 2048;
      const double phi = 2 * M_PI * (double)((k * start) % N) / N;
      r2d->carrier_re[s] = (c16_t){sat16(a * cos(phi)), sat16(a * sin(phi))};
    }
  }
  LOG_I(NR_PHY,
        "A-IoT R2D on PRBs %d to %d, EPRE %+.1f dB over PDSCH or PUSCH%s\n",
        cfg->prb_start,
        cfg->prb_start + cfg->n_rb_max - 1,
        cfg->power_offset_db,
        cfg->carrier ? ", with the carrier wave at their centre" : "");
  return 0;
}

void nr_aiot_r2d_set_tx_slots(nr_aiot_r2d_t *r2d, uint32_t tx_slots)
{
  pthread_mutex_lock(&r2d->lock);
  r2d->tx_slots = tx_slots;
  pthread_mutex_unlock(&r2d->lock);
}

void nr_aiot_r2d_free(nr_aiot_r2d_t *r2d)
{
  for (int i = 0; i < r2d->n_pending; i++)
    free(r2d->pending[i].re);
  r2d->n_pending = 0;
  pthread_mutex_destroy(&r2d->lock);
}

/* A Q15 product back to 16 bits, rounded and saturated (as rotate_cpx_vector does for PDSCH). */
static int16_t rot_q15(int64_t v)
{
  v = (v + (1 << 14)) >> 15;
  return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

int nr_aiot_r2d_schedule(nr_aiot_r2d_t *r2d,
                         int frame,
                         int slot,
                         int symbol,
                         const uint8_t *tb,
                         int a_bits,
                         int m_chip_symb,
                         int n_rb)
{
  if (r2d->cfg.prb_start < 0)
    return AIOT_EINVAL;
  if (frame < 0 || frame >= FRAMES || slot < 0 || slot >= r2d->slots_per_frame || (symbol != 0 && symbol != 7))
    return AIOT_EINVAL;
  if (!aiot_m_valid(m_chip_symb) || n_rb < aiot_n_rb_min(m_chip_symb) || n_rb > r2d->cfg.n_rb_max)
    return AIOT_EINVAL;
  if (a_bits < 1 || a_bits > AIOT_MAX_TB_BITS || !tb)
    return AIOT_EINVAL;

  const int max_chips = 4 * (a_bits + 16) + 64;
  uint8_t *chips = malloc(max_chips);
  if (!chips)
    return AIOT_ENOMEM;
  const int n_chips = aiot_r2d_chips(tb, a_bits, m_chip_symb, chips, max_chips);
  if (n_chips < 0) {
    free(chips);
    return n_chips;
  }
  const int n_sym = aiot_r2d_n_symbols(n_chips, m_chip_symb);
  const int K = n_rb * 12;
  cd_t *re = malloc((size_t)n_sym * K * sizeof(*re));
  c16_t *re16 = malloc((size_t)n_sym * K * sizeof(*re16));
  int rc = re && re16 ? aiot_r2d_resource_elements(chips, n_chips, m_chip_symb, n_rb, re, n_sym * K) : AIOT_ENOMEM;
  free(chips);
  if (rc < 0) {
    free(re);
    free(re16);
    return rc;
  }
  /* see nr_aiot_r2d_init(): s^2 / (2K) = tx_amp^2 / 2 */
  const double s = r2d->on_amplitude * sqrt((double)K);
  for (int i = 0; i < n_sym * K; i++)
    re16[i] = (c16_t){sat16(s * re[i].r), sat16(s * re[i].i)};
  free(re);

  const int start = frame * r2d->slots_per_frame + slot;
  const int first = symbol, last = symbol + n_sym; /* symbols counted from the start slot */
  const int wrap = FRAMES * r2d->slots_per_frame;
  pthread_mutex_lock(&r2d->lock);
  rc = n_sym;
  for (int d = 0; d * r2d->symbols_per_slot < last && rc > 0; d++)
    if (!(r2d->tx_slots >> ((start + d) % wrap % NR_AIOT_R2D_TX_SLOT_PERIOD) & 1))
      rc = AIOT_EINVAL; /* a slot the node does not send in */
  if (rc > 0 && r2d->now_slot >= 0 && !in_past(r2d, start, r2d->now_slot))
    rc = NR_AIOT_R2D_ETOOLATE;
  else if (rc > 0 && r2d->n_pending == NR_AIOT_R2D_MAX_PENDING)
    rc = AIOT_ESPACE;
  for (int i = 0; i < r2d->n_pending && rc > 0; i++) {
    const nr_aiot_r2d_tx_t *p = &r2d->pending[i];
    /* both in symbols from the earlier start slot */
    int a0, a1, b0, b1;
    if (in_past(r2d, start, p->start_slot)) {
      const int d = slot_diff(r2d, p->start_slot, start) * r2d->symbols_per_slot;
      a0 = p->start_symbol, a1 = p->start_symbol + p->n_sym, b0 = first + d, b1 = last + d;
    } else {
      const int d = slot_diff(r2d, start, p->start_slot) * r2d->symbols_per_slot;
      a0 = p->start_symbol + d, a1 = p->start_symbol + p->n_sym + d, b0 = first, b1 = last;
    }
    if (a0 < b1 && b0 < a1)
      rc = AIOT_EINVAL;
  }
  if (rc > 0) {
    r2d->pending[r2d->n_pending++] = (nr_aiot_r2d_tx_t){.start_slot = start,
                                                        .start_symbol = symbol,
                                                        .n_sym = n_sym,
                                                        .prb_start = r2d->cfg.prb_start,
                                                        .n_rb = n_rb,
                                                        .re = re16,
                                                        .last_mapped = -1};
    re16 = NULL;
  }
  pthread_mutex_unlock(&r2d->lock);
  free(re16);
  return rc;
}

int nr_aiot_r2d_n_symbols(const uint8_t *tb, int a_bits, int m_chip_symb)
{
  if (!aiot_m_valid(m_chip_symb) || a_bits < 1 || a_bits > AIOT_MAX_TB_BITS || !tb)
    return AIOT_EINVAL;
  const int max_chips = 4 * (a_bits + 16) + 64; /* as schedule() */
  uint8_t *chips = malloc(max_chips);
  if (!chips)
    return AIOT_ENOMEM;
  const int n_chips = aiot_r2d_chips(tb, a_bits, m_chip_symb, chips, max_chips);
  free(chips);
  return n_chips < 0 ? n_chips : aiot_r2d_n_symbols(n_chips, m_chip_symb);
}

void nr_aiot_r2d_set_slot_time(nr_aiot_r2d_t *r2d, int frame, int slot, int64_t ts)
{
  if (r2d->slots_per_frame == 0)
    return; /* not initialised */
  pthread_mutex_lock(&r2d->lock);
  r2d->timed_slot = frame * r2d->slots_per_frame + slot;
  r2d->timed_ts = ts;
  pthread_mutex_unlock(&r2d->lock);
}

int nr_aiot_r2d_now(nr_aiot_r2d_t *r2d, int64_t *ts)
{
  pthread_mutex_lock(&r2d->lock);
  int now = r2d->now_slot;
  *ts = r2d->now_ts;
  pthread_mutex_unlock(&r2d->lock);
  return now;
}

int nr_aiot_r2d_take(nr_aiot_r2d_t *r2d, int frame, int slot, nr_aiot_r2d_slot_t *out)
{
  out->frame = frame;
  out->slot = slot;
  out->prb_start = r2d->cfg.prb_start;
  out->carrier = r2d->cfg.carrier && r2d->cfg.prb_start >= 0;
  memset(out->n_rb, 0, sizeof(out->n_rb));
  if (r2d->slots_per_frame == 0)
    return 0; /* not initialised */
  const int now = frame * r2d->slots_per_frame + slot;
  const int nsym = r2d->symbols_per_slot;
  int taken = 0;
  pthread_mutex_lock(&r2d->lock);
  r2d->now_slot = now;
  r2d->now_ts = r2d->timed_slot == now ? r2d->timed_ts : -1;
  if (r2d->cfg.prb_start < 0) {
    pthread_mutex_unlock(&r2d->lock);
    return 0; /* only the clock: the R2D is sent by another node (a split Reader) */
  }
  for (int i = 0; i < r2d->n_pending;) {
    nr_aiot_r2d_tx_t *p = &r2d->pending[i];
    int done = 0;
    if (in_past(r2d, p->start_slot, now)) {
      i++; /* starts later */
      continue;
    }
    if (p->last_mapped == now) {
      i++; /* this slot again (a repeated TX of it): its symbols are on the grid already */
      continue;
    }
    const int d = slot_diff(r2d, p->start_slot, now);
    const int l0 = d * nsym - p->start_symbol; /* R2D symbol at symbol 0 of this slot */
    if (d != p->next_slot) {
      /* a slot of the transmission was not mapped (its start came too late, or the node did not send
       * a slot it spans): the device would get a broken frame, so the rest is not sent */
      r2d->dropped++;
      LOG_W(NR_PHY,
            "A-IoT R2D of %d.%d dropped: slot %d of it was not sent\n",
            p->start_slot / r2d->slots_per_frame,
            p->start_slot % r2d->slots_per_frame,
            p->next_slot);
      done = 1;
    } else {
      p->next_slot++;
      p->last_mapped = now;
      const int K = p->n_rb * 12;
      for (int s = 0; s < nsym; s++) {
        const int l = l0 + s;
        if (l < 0 || l >= p->n_sym)
          continue;
        memcpy(out->re[s], p->re + (size_t)l * K, K * sizeof(*out->re[s]));
        out->n_rb[s] = p->n_rb;
        taken++;
      }
      done = l0 + nsym >= p->n_sym;
      if (done)
        r2d->sent++;
    }
    if (done) {
      free(p->re);
      r2d->pending[i] = r2d->pending[--r2d->n_pending];
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&r2d->lock);
  return taken;
}

int nr_aiot_r2d_put(nr_aiot_r2d_t *r2d, const nr_aiot_r2d_slot_t *in, c16_t *txdataF, const c16_t *rotation, bool *symbol_used)
{
  const int nsym = r2d->symbols_per_slot, fft = r2d->ofdm_symbol_size;
  int written = 0, collisions = 0, blocked = 0, last_prb = in->prb_start;
  for (int s = 0; s < nsym; s++) {
    if (!in->n_rb[s])
      continue;
    const int K = in->n_rb[s] * 12;
    c16_t *dst = txdataF + s * fft + in->prb_start * 12;
    const c16_t *src = in->re[s];
    for (int q = 0; q < K; q++) {
      if (dst[q].r || dst[q].i)
        collisions++;
      c16_t v = src[q];
      if (rotation) {
        const c16_t rot = rotation[s];
        v = (c16_t){rot_q15((int64_t)src[q].r * rot.r - (int64_t)src[q].i * rot.i),
                    rot_q15((int64_t)src[q].r * rot.i + (int64_t)src[q].i * rot.r)};
      }
      dst[q] = v;
    }
    if (in->prb_start + in->n_rb[s] - 1 > last_prb)
      last_prb = in->prb_start + in->n_rb[s] - 1;
    if (symbol_used)
      symbol_used[s] = true;
    written++;
  }
  for (int s = 0; in->carrier && s < nsym; s++) {
    if (in->n_rb[s])
      continue;
    c16_t *dst = txdataF + s * fft + r2d->carrier_sc;
    if (dst->r || dst->i)
      blocked++;
    else
      *dst = r2d->carrier_re[s];
    if (symbol_used)
      symbol_used[s] = true;
  }
  if (collisions || blocked) {
    pthread_mutex_lock(&r2d->lock);
    r2d->collisions += collisions;
    r2d->carrier_blocked += blocked;
    pthread_mutex_unlock(&r2d->lock);
  }
  if (collisions)
    LOG_E(NR_PHY,
          "A-IoT R2D in %d.%d overwrote %d resource elements of NR channels: its PRBs %d to %d must be blacklisted\n",
          in->frame,
          in->slot,
          collisions,
          in->prb_start,
          last_prb);
  return written;
}

int nr_aiot_r2d_map(nr_aiot_r2d_t *r2d, int frame, int slot, c16_t *txdataF, const c16_t *rotation, bool *symbol_used)
{
  nr_aiot_r2d_slot_t s;
  nr_aiot_r2d_take(r2d, frame, slot, &s);
  return nr_aiot_r2d_put(r2d, &s, txdataF, rotation, symbol_used);
}
