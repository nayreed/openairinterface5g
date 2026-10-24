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
#include "PHY/TOOLS/tools_defs.h" /* rotate_cpx_vector() */
#include "PHY/impl_defs_top.h" /* MAX_FRAME_NUMBER */
#include "common/utils/LOG/log.h"
#include "common/utils/fsn.h"
#include "common/utils/utils.h"

_Static_assert(NR_AIOT_R2D_MAX_SC == AIOT_N_FFT * 3 / 4, "NR_AIOT_R2D_MAX_SC is 3/4 of AIOT_N_FFT");
_Static_assert(AIOT_CELL_FRAMES == MAX_FRAME_NUMBER, "A-IoT counts the cell's frames as NR does");
_Static_assert(NR_AIOT_R2D_TX_SLOT_PERIOD <= 32, "the TX slot mask is 32 bits");

static fsn_t cell_fsn(int abs_slot)
{
  return (fsn_t){.f = abs_slot / AIOT_CELL_SLOTS_PER_FRAME, .s = abs_slot % AIOT_CELL_SLOTS_PER_FRAME, .mu = 0};
}

int nr_aiot_cell_slot_diff(int a, int b)
{
  return fsn_get_diff(cell_fsn(b), cell_fsn(a));
}

/* Slots from a to b, forward, with the SFN wrap: what the scheduler compares in. */
static int slot_diff(int a, int b)
{
  const int d = nr_aiot_cell_slot_diff(a, b);
  return d < 0 ? d + AIOT_CELL_WRAP_SLOTS : d;
}

/* b is before a */
static bool in_past(int a, int b)
{
  return nr_aiot_cell_slot_diff(a, b) < 0;
}

static int16_t sat16(double v)
{
  v = round(v);
  return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

int nr_aiot_r2d_init(const nr_aiot_r2d_config_t *cfg,
                     int slots_per_frame,
                     int symbols_per_slot,
                     int ofdm_symbol_size,
                     int prefix0,
                     int prefix,
                     int n_prb,
                     int tx_amp,
                     nr_aiot_r2d_t *r2d)
{
  memset(r2d, 0, sizeof(*r2d));
  r2d->now_slot = -1;
  r2d->now_ts = -1;
  r2d->timed_slot = -1;
  r2d->timed_ts = -1;
  r2d->tx_slots = (1u << NR_AIOT_R2D_TX_SLOT_PERIOD) - 1;
  r2d->cfg = *cfg;
  r2d->cfg.prb_start = -1;
  /* the grid's sizes, also without R2D: a Reader may run the node's clock (nr_aiot_r2d_start_clock()) */
  r2d->slots_per_frame = slots_per_frame;
  r2d->symbols_per_slot = symbols_per_slot;
  r2d->ofdm_symbol_size = ofdm_symbol_size;
  r2d->n_prb = n_prb;
  pthread_mutex_init(&r2d->lock, NULL);
  if (cfg->prb_start < 0 || cfg->n_rb_max == 0)
    return 0; /* no R2D: a gNB configured without one, or zeroed (the simulators) */
  if (slots_per_frame != AIOT_CELL_SLOTS_PER_FRAME || symbols_per_slot != NR_SYMBOLS_PER_SLOT) {
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
  if (cfg->n_rb_max * NR_NB_SC_PER_RB > NR_AIOT_R2D_MAX_SC) {
    LOG_E(NR_PHY, "A-IoT R2D on %d PRBs is wider than the 384-point symbol allows\n", cfg->n_rb_max);
    return -1;
  }
  /* An R2D resource element is at most on_amplitude * sqrt(K) (an all-on symbol has |X| <= 1 on every
   * subcarrier): it must fit 16 bits, so that no chip is ever clipped. */
  const double peak = tx_amp * pow(10.0, cfg->power_offset_db / 20.0) * sqrt(cfg->n_rb_max * (double)NR_NB_SC_PER_RB);
  if (peak > INT16_MAX) {
    LOG_E(NR_PHY,
          "A-IoT R2D EPRE %+.1f dB over PDSCH or PUSCH on %d PRBs would clip: at most %+.1f dB\n",
          cfg->power_offset_db,
          cfg->n_rb_max,
          cfg->power_offset_db - 20.0 * log10(peak / INT16_MAX));
    return -1;
  }
  r2d->cfg = *cfg;
  r2d->clock = true;
  /* aiot_r2d_resource_elements() gives a waveform whose "on" chips are about 1 and whose mean power,
   * with half the chips on, is about 1/2 over its K subcarriers, i.e. 1 / (2K) per resource element.
   * A QPSK resource element of the data channel has power tx_amp^2 / 2. The R2D's own K is used at
   * scheduling time. */
  r2d->on_amplitude = tx_amp * pow(10.0, cfg->power_offset_db / 20.0);
  if (cfg->carrier) {
    /* A tone on subcarrier k (from DC) is exp(j 2 pi k t / N) at sample t of the slot; symbol s's
     * useful part starts at s N plus the cyclic prefixes up to it (prefix0 for symbols 0 and 7, prefix
     * for the others), and a slot is 15 N samples at 15 kHz, so the phase repeats every slot. */
    r2d->carrier_sc = NR_NB_SC_PER_RB * cfg->prb_start + NR_NB_SC_PER_RB / 2 * cfg->n_rb_max;
    const int k = r2d->carrier_sc - NR_NB_SC_PER_RB / 2 * n_prb, N = ofdm_symbol_size;
    const double a = r2d->on_amplitude * sqrt((double)NR_NB_SC_PER_RB * cfg->n_rb_max);
    int64_t start = 0;
    for (int s = 0; s < symbols_per_slot; s++) {
      start += s % (NR_SYMBOLS_PER_SLOT / 2) == 0 ? prefix0 : prefix;
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

uint32_t nr_aiot_r2d_slot_mask(int slots_per_frame, bool (*sends)(const void *ctx, int frame, int slot), const void *ctx)
{
  uint32_t mask = 0;
  for (int i = 0; i < NR_AIOT_R2D_TX_SLOT_PERIOD; i++)
    if (sends(ctx, i / slots_per_frame, i % slots_per_frame))
      mask |= 1u << i;
  return mask;
}

void nr_aiot_r2d_start_clock(nr_aiot_r2d_t *r2d)
{
  r2d->clock = r2d->slots_per_frame > 0;
}

void nr_aiot_r2d_free(nr_aiot_r2d_t *r2d)
{
  if (r2d->slots_per_frame == 0)
    return; /* never initialised, or freed already */
  for (int i = 0; i < r2d->n_pending; i++)
    free(r2d->pending[i].re);
  r2d->n_pending = 0;
  r2d->clock = false;
  r2d->slots_per_frame = 0;
  pthread_mutex_destroy(&r2d->lock);
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
  if (frame < 0 || frame >= AIOT_CELL_FRAMES || slot < 0 || slot >= r2d->slots_per_frame || (symbol != 0 && symbol != 7))
    return AIOT_EINVAL;
  if (!aiot_m_valid(m_chip_symb) || n_rb < aiot_n_rb_min(m_chip_symb) || n_rb > r2d->cfg.n_rb_max)
    return AIOT_EINVAL;
  if (a_bits < 1 || a_bits > AIOT_MAX_TB_BITS || !tb)
    return AIOT_EINVAL;

  const int max_chips = AIOT_R2D_MAX_CHIPS(a_bits);
  uint8_t *chips = malloc_or_fail(max_chips);
  const int n_chips = aiot_r2d_chips(tb, a_bits, m_chip_symb, chips, max_chips);
  if (n_chips < 0) {
    free(chips);
    return n_chips;
  }
  const int n_sym = aiot_r2d_n_symbols(n_chips, m_chip_symb);
  const int K = n_rb * NR_NB_SC_PER_RB;
  cd_t *re = malloc_or_fail((size_t)n_sym * K * sizeof(*re));
  c16_t *re16 = malloc_or_fail((size_t)n_sym * K * sizeof(*re16));
  int rc = aiot_r2d_resource_elements(chips, n_chips, m_chip_symb, n_rb, re, n_sym * K);
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
  pthread_mutex_lock(&r2d->lock);
  rc = n_sym;
  for (int d = 0; d * r2d->symbols_per_slot < last && rc > 0; d++)
    if (!(r2d->tx_slots >> ((start + d) % AIOT_CELL_WRAP_SLOTS % NR_AIOT_R2D_TX_SLOT_PERIOD) & 1))
      rc = AIOT_EINVAL; /* a slot the node does not send in */
  if (rc > 0 && r2d->now_slot >= 0 && !in_past(start, r2d->now_slot))
    rc = NR_AIOT_R2D_ETOOLATE;
  else if (rc > 0 && r2d->n_pending == NR_AIOT_R2D_MAX_PENDING)
    rc = AIOT_ESPACE;
  for (int i = 0; i < r2d->n_pending && rc > 0; i++) {
    const nr_aiot_r2d_tx_t *p = &r2d->pending[i];
    /* both in symbols from the earlier start slot */
    int a0, a1, b0, b1;
    if (in_past(start, p->start_slot)) {
      const int d = slot_diff(p->start_slot, start) * r2d->symbols_per_slot;
      a0 = p->start_symbol, a1 = p->start_symbol + p->n_sym, b0 = first + d, b1 = last + d;
    } else {
      const int d = slot_diff(start, p->start_slot) * r2d->symbols_per_slot;
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
  const int max_chips = AIOT_R2D_MAX_CHIPS(a_bits);
  uint8_t *chips = malloc_or_fail(max_chips);
  const int n_chips = aiot_r2d_chips(tb, a_bits, m_chip_symb, chips, max_chips);
  free(chips);
  return n_chips < 0 ? n_chips : aiot_r2d_n_symbols(n_chips, m_chip_symb);
}

void nr_aiot_r2d_set_slot_time(nr_aiot_r2d_t *r2d, int frame, int slot, int64_t ts)
{
  if (!r2d->clock)
    return; /* no R2D, and no Reader plans on the node's slots */
  pthread_mutex_lock(&r2d->lock);
  r2d->timed_slot = frame * r2d->slots_per_frame + slot;
  r2d->timed_ts = ts;
  pthread_mutex_unlock(&r2d->lock);
}

int nr_aiot_r2d_now(nr_aiot_r2d_t *r2d, int64_t *ts)
{
  if (!r2d->clock) {
    *ts = -1;
    return -1;
  }
  pthread_mutex_lock(&r2d->lock);
  int now = r2d->now_slot;
  *ts = r2d->now_ts;
  pthread_mutex_unlock(&r2d->lock);
  return now;
}

/* A transmission has symbols in slot now (a pending one that is due). */
static bool due(const nr_aiot_r2d_t *r2d, const nr_aiot_r2d_tx_t *p, int now)
{
  return !in_past(p->start_slot, now) && p->last_mapped != now;
}

/* take(), with the lock held. out NULL: the slot is not sent. */
static int take_locked(nr_aiot_r2d_t *r2d, int now, nr_aiot_r2d_slot_t *out)
{
  const int nsym = r2d->symbols_per_slot;
  int taken = 0;
  r2d->now_slot = now;
  r2d->now_ts = r2d->timed_slot == now ? r2d->timed_ts : -1;
  if (r2d->cfg.prb_start < 0)
    return 0; /* only the clock: the R2D is sent by another node (a split Reader) */
  for (int i = 0; i < r2d->n_pending;) {
    nr_aiot_r2d_tx_t *p = &r2d->pending[i];
    if (!due(r2d, p, now)) {
      i++; /* starts later, or this slot again (a repeated TX of it): its symbols are on the grid already */
      continue;
    }
    bool done = false;
    const int d = slot_diff(p->start_slot, now);
    const int l0 = d * nsym - p->start_symbol; /* R2D symbol at symbol 0 of this slot */
    if (d != p->next_slot || !out) {
      /* a slot of the transmission was not sent (its start came too late, or the node did not send a
       * slot it spans): the device would get a broken frame, so the rest is not sent */
      if (aiot_log_due(++r2d->dropped))
        LOG_W(NR_PHY,
              "A-IoT R2D of %d.%d dropped: slot %d of it was not sent (%lu dropped so far)\n",
              p->start_slot / r2d->slots_per_frame,
              p->start_slot % r2d->slots_per_frame,
              out ? p->next_slot : d,
              (unsigned long)r2d->dropped);
      done = true;
    } else {
      p->next_slot++;
      p->last_mapped = now;
      const int K = p->n_rb * NR_NB_SC_PER_RB;
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
  return taken;
}

static void empty_slot(const nr_aiot_r2d_t *r2d, int frame, int slot, nr_aiot_r2d_slot_t *out)
{
  out->frame = frame;
  out->slot = slot;
  out->prb_start = r2d->cfg.prb_start;
  out->carrier = r2d->cfg.carrier && r2d->cfg.prb_start >= 0;
  memset(out->n_rb, 0, sizeof(out->n_rb));
}

int nr_aiot_r2d_take(nr_aiot_r2d_t *r2d, int frame, int slot, nr_aiot_r2d_slot_t *out)
{
  if (out)
    empty_slot(r2d, frame, slot, out);
  if (!r2d->clock)
    return 0;
  pthread_mutex_lock(&r2d->lock);
  const int taken = take_locked(r2d, frame * r2d->slots_per_frame + slot, out);
  pthread_mutex_unlock(&r2d->lock);
  return taken;
}

nr_aiot_r2d_slot_t *nr_aiot_r2d_take_new(nr_aiot_r2d_t *r2d, int frame, int slot)
{
  if (!r2d->clock)
    return NULL;
  const int now = frame * r2d->slots_per_frame + slot;
  pthread_mutex_lock(&r2d->lock);
  bool any = r2d->cfg.carrier && r2d->cfg.prb_start >= 0;
  for (int i = 0; i < r2d->n_pending && !any; i++)
    any = due(r2d, &r2d->pending[i], now);
  nr_aiot_r2d_slot_t *out = NULL;
  if (any) {
    out = malloc_or_fail(sizeof(*out));
    empty_slot(r2d, frame, slot, out);
  }
  take_locked(r2d, now, out);
  pthread_mutex_unlock(&r2d->lock);
  return out;
}

/* An R2D symbol may be written where no NR channel is: returns the resource elements in use. */
static int in_use(const c16_t *dst, int K)
{
  int n = 0;
  for (int q = 0; q < K; q++)
    n += dst[q].r || dst[q].i;
  return n;
}

int nr_aiot_r2d_put(nr_aiot_r2d_t *r2d, const nr_aiot_r2d_slot_t *in, const c16_t *rotation, c16_t *txdataF, bool *symbol_used)
{
  if (!in)
    return 0;
  const int nsym = r2d->symbols_per_slot, fft = r2d->ofdm_symbol_size;
  int written = 0, collisions = 0, collided = 0, blocked = 0;
  for (int s = 0; s < nsym; s++) {
    if (!in->n_rb[s])
      continue;
    const int K = in->n_rb[s] * NR_NB_SC_PER_RB;
    c16_t *dst = txdataF + s * fft + in->prb_start * NR_NB_SC_PER_RB;
    const int used = in_use(dst, K);
    if (used) {
      collisions += used;
      collided++;
      continue; /* the NR channel keeps its resource elements */
    }
    if (rotation)
      rotate_cpx_vector(in->re[s], rotation[s], dst, K, 15);
    else
      memcpy(dst, in->re[s], K * sizeof(*dst));
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
  if (!collisions && !blocked)
    return written;
  pthread_mutex_lock(&r2d->lock);
  r2d->collisions += collisions;
  r2d->collided_symbols += collided;
  const uint64_t events = collided ? ++r2d->collided_slots : 0;
  r2d->carrier_blocked += blocked;
  pthread_mutex_unlock(&r2d->lock);
  /* a misconfiguration repeats every slot: logged at the 1st, 2nd, 4th... slot it hits */
  if (aiot_log_due(events))
    LOG_E(NR_PHY,
          "A-IoT R2D in %d.%d: %d symbols not sent, NR channels hold %d of their resource elements (%lu slots so far): PRBs %d to "
          "%d must be blacklisted\n",
          in->frame,
          in->slot,
          collided,
          collisions,
          (unsigned long)events,
          in->prb_start,
          in->prb_start + r2d->cfg.n_rb_max - 1);
  return written;
}

int nr_aiot_r2d_map(nr_aiot_r2d_t *r2d, int frame, int slot, const c16_t *rotation, c16_t *txdataF)
{
  if (!r2d->clock)
    return 0;
  nr_aiot_r2d_slot_t s;
  nr_aiot_r2d_take(r2d, frame, slot, &s);
  return nr_aiot_r2d_put(r2d, &s, rotation, txdataF, NULL);
}
