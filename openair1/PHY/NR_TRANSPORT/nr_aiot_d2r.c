/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_d2r.c
 * \brief An NR node as an Ambient IoT Reader: D2R reception in PRBs of the band it receives (nr_aiot_d2r.h).
 *
 * Each radio sample x[t] (radio timestamp t, rate fs) is turned to w[t] = x[t] exp(-j 2 pi f t / fs),
 * f the centre of the D2R PRBs, so that the D2R is at 0 Hz. A-IoT sample m is then
 *
 *   y[m] = sum_k h[k] w[m D + G - k],  k = 0 .. 2G,
 *
 * a low-pass filter centred on radio sample m D. With D = 1 the filter is h = {1}: the receiver gets
 * the samples as they are, as Amber's Reader does in the same room. With D > 1 it stops what would
 * alias into the D2R.
 */

#include "nr_aiot_d2r.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common/utils/LOG/log.h"
#include "common/utils/system.h"

#define FILTER_SPAN 8 /* G = FILTER_SPAN * D radio samples each side */
#define FILTER_CUTOFF_HZ 1900000.0 /* passes the D2R PRBs, stops what aliases into them */
#define MAX_GAP_S 1 /* a longer gap than this restarts the receiver once its windows are done */
#define ZERO_CHUNK (AIOT_SAMPLES_PER_MS * 10) /* A-IoT samples received at a time across a gap */
#define STATS_EVERY_S 10 /* the worker logs its counts this often */
#define D2R_QUEUE_MS 100 /* the worker's room: a real radio's samples are lost if it falls further behind */

typedef struct {
  c16_t *x;
  int64_t ts;
  int n;
} d2r_slot_t;

struct nr_aiot_d2r {
  nr_aiot_d2r_config_t cfg;
  int64_t fs; /* radio samples per second */
  int d; /* radio samples per A-IoT sample */
  int64_t shift_hz; /* centre of the D2R PRBs, from the carrier's centre */
  double noise_w; /* counts squared, I plus Q */

  /* the filter and the shifted radio samples it still needs */
  double *h;
  int g; /* h has 2 g + 1 taps */
  cd_t *w;
  int64_t w0; /* radio timestamp of w[0] */
  int64_t nw, cap_w;
  int64_t next_ts; /* the radio timestamp expected next; -1 before the first slot */
  int64_t next_m; /* the A-IoT sample made next */
  cd_t *y; /* A-IoT samples made by one call */
  int64_t cap_y;

  /* the A-IoT receiver; its sample 0 is A-IoT sample a0 */
  aiot_d2r_rx_t *rx;
  int64_t a0;
  int in_rx; /* windows given to rx and not yet decoded */
  int hold_windows; /* 1: windows stay queued (while the receiver is about to start again) */
  int64_t last_end; /* the A-IoT sample after the last window given to rx ends */
  aiot_d2r_outcome_t *scratch;
  int cap_scratch;

  /* shared with the other threads */
  pthread_mutex_t lock;
  pthread_cond_t cond;
  aiot_d2r_expect_t *ex; /* windows waiting to be given to rx */
  int n_ex, cap_ex;
  aiot_d2r_outcome_t *out; /* decoded windows waiting for pop */
  int n_out, cap_out;
  int64_t received_until; /* A-IoT clock */
  nr_aiot_d2r_stats_t stats;

  /* the worker */
  int running, stop;
  pthread_t thread;
  d2r_slot_t *ring;
  int ring_n;
  uint64_t head, tail; /* slots head .. tail - 1 wait, in ring[i % ring_n] */
  int max_slot_samples;
  int wait_when_full;
  int64_t next_log; /* A-IoT clock */
};

static int grow(void **p, int64_t *cap, int64_t need, size_t size)
{
  if (need <= *cap)
    return 0;
  int64_t c = *cap ? 2 * *cap : 1024;
  while (c < need)
    c *= 2;
  void *np = realloc(*p, size * (size_t)c);
  if (!np)
    return AIOT_ENOMEM;
  *p = np;
  *cap = c;
  return 0;
}

/* Windowed sinc (Blackman), unit gain at 0 Hz. */
static int make_filter(nr_aiot_d2r_t *d)
{
  d->g = d->d > 1 ? FILTER_SPAN * d->d : 0;
  int nt = 2 * d->g + 1;
  d->h = malloc(sizeof(double) * (size_t)nt);
  if (!d->h)
    return AIOT_ENOMEM;
  if (nt == 1) {
    d->h[0] = 1.0;
    return 0;
  }
  double fc = FILTER_CUTOFF_HZ / (double)d->fs, sum = 0;
  for (int k = 0; k < nt; k++) {
    double n = k - d->g;
    double sinc = n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
    double win = 0.42 - 0.5 * cos(2 * M_PI * k / (nt - 1)) + 0.08 * cos(4 * M_PI * k / (nt - 1));
    d->h[k] = sinc * win;
    sum += d->h[k];
  }
  for (int k = 0; k < nt; k++)
    d->h[k] /= sum;
  return 0;
}

static aiot_d2r_rx_t *new_rx(const nr_aiot_d2r_t *d)
{
  return aiot_d2r_rx_new(d->noise_w, d->cfg.enable_sic);
}

nr_aiot_d2r_t *nr_aiot_d2r_new(const nr_aiot_d2r_config_t *cfg, int n_prb, int scs_hz, int64_t sample_rate_hz)
{
  if (scs_hz != AIOT_SCS_HZ) {
    LOG_E(NR_PHY, "A-IoT D2R: the carrier's subcarrier spacing is %d Hz; A-IoT has only %d Hz\n", scs_hz, AIOT_SCS_HZ);
    return NULL;
  }
  if (sample_rate_hz < AIOT_SAMPLE_RATE_HZ || sample_rate_hz % AIOT_SAMPLE_RATE_HZ) {
    LOG_E(NR_PHY,
          "A-IoT D2R: the radio samples at %ld Hz, which is not a whole multiple of %d Hz (try three-quarter sampling, -E)\n",
          (long)sample_rate_hz,
          AIOT_SAMPLE_RATE_HZ);
    return NULL;
  }
  /* the carrier's PRBs, then the guard band the samples hold above them (and wrapping round below) */
  const int n_sampled = (int)(sample_rate_hz / (12 * AIOT_SCS_HZ));
  if (!cfg || cfg->prb_start < 0 || cfg->n_rb < 1 || cfg->prb_start + cfg->n_rb > n_sampled) {
    LOG_E(NR_PHY,
          "A-IoT D2R: PRBs %d to %d are not in the %d PRBs the radio samples (the carrier's %d, then its guard band)\n",
          cfg ? cfg->prb_start : -1,
          cfg ? cfg->prb_start + cfg->n_rb - 1 : -1,
          n_sampled,
          n_prb);
    return NULL;
  }
  if (!(cfg->noise_counts > 0)) {
    LOG_E(NR_PHY, "A-IoT D2R: the noise floor must be above 0 counts (is %g)\n", cfg->noise_counts);
    return NULL;
  }
  nr_aiot_d2r_t *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->cfg = *cfg;
  d->fs = sample_rate_hz;
  d->d = (int)(sample_rate_hz / AIOT_SAMPLE_RATE_HZ);
  /* subcarrier k of the carrier is (k - 6 n_prb) * 15 kHz from its centre */
  d->shift_hz = (int64_t)(12 * cfg->prb_start + 6 * cfg->n_rb - 6 * n_prb) * AIOT_SCS_HZ;
  d->noise_w = 2 * cfg->noise_counts * cfg->noise_counts;
  d->next_ts = -1;
  d->rx = new_rx(d);
  pthread_mutex_init(&d->lock, NULL);
  pthread_cond_init(&d->cond, NULL);
  if (!d->rx || make_filter(d) != 0 || grow((void **)&d->w, &d->cap_w, d->g + 1, sizeof(cd_t)) != 0) {
    nr_aiot_d2r_free(d);
    return NULL;
  }
  return d;
}

void nr_aiot_d2r_free(nr_aiot_d2r_t *d)
{
  if (!d)
    return;
  if (d->running) {
    pthread_mutex_lock(&d->lock);
    d->stop = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->thread, NULL);
  }
  if (d->ring)
    for (int i = 0; i < d->ring_n; i++)
      free(d->ring[i].x);
  free(d->ring);
  aiot_d2r_rx_free(d->rx);
  free(d->h);
  free(d->w);
  free(d->y);
  free(d->scratch);
  free(d->ex);
  free(d->out);
  pthread_mutex_destroy(&d->lock);
  pthread_cond_destroy(&d->cond);
  free(d);
}

double nr_aiot_d2r_shift_hz(const nr_aiot_d2r_t *d)
{
  return (double)d->shift_hz;
}

int nr_aiot_d2r_decimation(const nr_aiot_d2r_t *d)
{
  return d->d;
}

int nr_aiot_d2r_expect(nr_aiot_d2r_t *d, const aiot_d2r_expect_t *e)
{
  if (!d || !e)
    return AIOT_EINVAL;
  pthread_mutex_lock(&d->lock);
  int ret = 0;
  if (d->n_ex == d->cap_ex) {
    int cap = d->cap_ex ? 2 * d->cap_ex : 8;
    aiot_d2r_expect_t *ne = realloc(d->ex, sizeof(*ne) * (size_t)cap);
    if (!ne)
      ret = AIOT_ENOMEM;
    else {
      d->ex = ne;
      d->cap_ex = cap;
    }
  }
  if (ret == 0)
    d->ex[d->n_ex++] = *e;
  pthread_mutex_unlock(&d->lock);
  return ret;
}

/* The A-IoT sample after the last one window e can need (as the receiver's own last_sample). */
static int64_t window_end(const aiot_d2r_expect_t *e)
{
  int64_t m = 0;
  int n = e->n_r_sfs ? e->n_r_sfs : 1;
  for (int i = 0; i < n; i++) {
    aiot_d2r_params_t p = e->params;
    p.r_sfs = e->n_r_sfs ? e->r_sfs[i] : e->params.r_sfs;
    int64_t len = (int64_t)aiot_frame_bits(e->a_bits, &p) * aiot_bit_samples(&p);
    if (len > m)
      m = len;
  }
  return e->start + e->te + 1 + m;
}

/* Gives rx the windows opened since the last call. */
static int take_windows(nr_aiot_d2r_t *d)
{
  if (d->hold_windows)
    return 0;
  pthread_mutex_lock(&d->lock);
  int ret = 0;
  for (int i = 0; i < d->n_ex; i++) {
    aiot_d2r_expect_t e = d->ex[i];
    if (e.start - e.te - 1 < d->received_until)
      d->stats.late++;
    e.start -= d->a0;
    ret = aiot_d2r_rx_expect(d->rx, &e);
    if (ret < 0) {
      LOG_E(NR_PHY, "A-IoT D2R: the window at %ld is not valid (%d), dropped\n", (long)d->ex[i].start, ret);
      ret = 0;
      continue;
    }
    d->in_rx++;
    int64_t end = window_end(&d->ex[i]);
    if (end > d->last_end)
      d->last_end = end;
  }
  d->n_ex = 0;
  pthread_mutex_unlock(&d->lock);
  return ret;
}

/* Hands n A-IoT samples (from sample a0 + fed) to rx, and queues what it decoded. */
static int receive(nr_aiot_d2r_t *d, const cd_t *y, int64_t n)
{
  int ret = take_windows(d);
  if (ret < 0)
    return ret;
  if (d->in_rx > d->cap_scratch) {
    aiot_d2r_outcome_t *s = realloc(d->scratch, sizeof(*s) * (size_t)d->in_rx);
    if (!s)
      return AIOT_ENOMEM;
    d->scratch = s;
    d->cap_scratch = d->in_rx;
  }
  int got = aiot_d2r_rx_receive(d->rx, y, (int)n, d->scratch, d->in_rx);
  pthread_mutex_lock(&d->lock);
  d->received_until += n; /* rx has taken the samples, whatever it returns */
  pthread_mutex_unlock(&d->lock);
  if (got < 0)
    return got;
  d->in_rx -= got;
  pthread_mutex_lock(&d->lock);
  if (got && d->n_out + got > d->cap_out) {
    int cap = d->cap_out ? d->cap_out : 8;
    while (cap < d->n_out + got)
      cap *= 2;
    aiot_d2r_outcome_t *no = realloc(d->out, sizeof(*no) * (size_t)cap);
    if (!no) {
      pthread_mutex_unlock(&d->lock);
      return AIOT_ENOMEM;
    }
    d->out = no;
    d->cap_out = cap;
  }
  for (int i = 0; i < got; i++) {
    aiot_d2r_outcome_t *o = &d->out[d->n_out++];
    *o = d->scratch[i];
    o->expect.start += d->a0;
    for (int j = 0; j < o->n_decoded; j++)
      o->decoded[j].start += d->a0;
    d->stats.windows++;
    d->stats.decoded += o->n_decoded;
    d->stats.collided += o->collided;
  }
  pthread_mutex_unlock(&d->lock);
  return 0;
}

/* Makes every A-IoT sample whose filter span has arrived, and receives them. */
static int make_and_receive(nr_aiot_d2r_t *d)
{
  int64_t w_end = d->w0 + d->nw; /* radio timestamp after the last shifted sample */
  int64_t n = 0;
  /* sample m needs w up to radio sample m D + g */
  int64_t last = (w_end - 1 - d->g) / d->d; /* w_end - 1 - g >= 0 here */
  if (w_end - 1 - d->g >= 0 && last >= d->next_m)
    n = last - d->next_m + 1;
  if (n <= 0)
    return 0;
  if (grow((void **)&d->y, &d->cap_y, n, sizeof(cd_t)) != 0)
    return AIOT_ENOMEM;
  int nt = 2 * d->g + 1;
  for (int64_t i = 0; i < n; i++) {
    const cd_t *s = d->w + ((d->next_m + i) * d->d + d->g - d->w0); /* w[m D + g] */
    double r = 0, im = 0;
    for (int k = 0; k < nt; k++) {
      r += d->h[k] * s[-k].r;
      im += d->h[k] * s[-k].i;
    }
    d->y[i].r = r;
    d->y[i].i = im;
  }
  d->next_m += n;
  /* keep only what the next sample's filter span needs */
  int64_t keep_from = d->next_m * d->d - d->g;
  int64_t drop = keep_from - d->w0;
  if (drop > 0) {
    memmove(d->w, d->w + drop, sizeof(cd_t) * (size_t)(d->nw - drop));
    d->nw -= drop;
    d->w0 += drop;
  }
  return receive(d, d->y, n);
}

/* Appends n shifted radio samples from timestamp d->next_ts (x NULL: zeros). */
static int append(nr_aiot_d2r_t *d, const c16_t *x, int64_t n)
{
  if (grow((void **)&d->w, &d->cap_w, d->nw + n, sizeof(cd_t)) != 0)
    return AIOT_ENOMEM;
  cd_t *w = d->w + d->nw;
  if (!x) {
    memset(w, 0, sizeof(cd_t) * (size_t)n);
  } else {
    /* exp(-j 2 pi f t / fs), from the exact phase at the first sample */
    int64_t ph = (int64_t)(((__int128)(d->next_ts % d->fs) * d->shift_hz) % d->fs);
    if (ph < 0)
      ph += d->fs;
    double a = -2 * M_PI * (double)ph / (double)d->fs, step = -2 * M_PI * (double)d->shift_hz / (double)d->fs;
    double cr = cos(a), ci = sin(a), sr = cos(step), si = sin(step);
    for (int64_t t = 0; t < n; t++) {
      double xr = x[t].r, xi = x[t].i;
      w[t].r = xr * cr - xi * ci;
      w[t].i = xr * ci + xi * cr;
      double nr = cr * sr - ci * si;
      ci = cr * si + ci * sr;
      cr = nr;
    }
  }
  d->nw += n;
  d->next_ts += n;
  return make_and_receive(d);
}

/* Starts a fresh receiver whose A-IoT sample 0 is the first made from radio timestamp ts. */
static void restart(nr_aiot_d2r_t *d, int64_t ts)
{
  int64_t m0 = ts >= 0 ? (ts + d->d - 1) / d->d : -((-ts) / d->d);
  d->w0 = ts - d->g; /* g zeros before the first sample (w has room for them: see nr_aiot_d2r_new) */
  memset(d->w, 0, sizeof(cd_t) * (size_t)d->g);
  d->nw = d->g;
  d->next_ts = ts;
  d->next_m = m0;
  d->a0 = m0;
  pthread_mutex_lock(&d->lock);
  d->received_until = m0;
  pthread_mutex_unlock(&d->lock);
}

/* Receives zeros for the radio samples next_ts .. ts - 1. After a gap longer than MAX_GAP_S, once
 * the windows the receiver holds have ended, the receiver starts again at ts (unless one of them
 * ends after ts: then the whole gap is received as zeros). */
static int fill_gap(nr_aiot_d2r_t *d, int64_t ts)
{
  int64_t gap = ts - d->next_ts, fill = gap;
  int again = 0;
  if (gap > MAX_GAP_S * d->fs) {
    /* only the windows rx already holds wait (those still queued go to the new receiver): A-IoT
     * sample last_end - 1 is made once w reaches radio sample (last_end - 1) D + g */
    int64_t need = d->in_rx ? (d->last_end - 1) * d->d + d->g + 1 - d->next_ts : 0;
    if (need < gap) {
      fill = need > 0 ? need : 0;
      again = 1;
    }
  }
  pthread_mutex_lock(&d->lock);
  d->stats.zero_filled += fill / d->d;
  pthread_mutex_unlock(&d->lock);
  d->hold_windows = again; /* the windows rx holds end within fill; the others wait for the new rx */
  int ret = 0;
  while (fill > 0 && ret == 0) {
    int64_t n = fill < (int64_t)ZERO_CHUNK * d->d ? fill : (int64_t)ZERO_CHUNK * d->d;
    ret = append(d, NULL, n);
    fill -= n;
  }
  d->hold_windows = 0;
  if (ret < 0)
    return ret;
  if (again) {
    LOG_W(NR_PHY, "A-IoT D2R: %ld radio samples did not arrive; the receiver starts again\n", (long)gap);
    aiot_d2r_rx_free(d->rx);
    d->rx = new_rx(d);
    if (!d->rx)
      return AIOT_ENOMEM;
    d->in_rx = 0;
    restart(d, ts);
  }
  return 0;
}

static int feed(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n)
{
  if (n < 0 || (n && !x))
    return AIOT_EINVAL;
  if (d->next_ts < 0)
    restart(d, ts);
  if (ts < d->next_ts) { /* already received */
    int64_t skip = d->next_ts - ts;
    if (skip >= n)
      return 0;
    x += skip;
    n -= (int)skip;
    ts = d->next_ts;
  }
  if (ts > d->next_ts) {
    int ret = fill_gap(d, ts);
    if (ret < 0)
      return ret;
  }
  return append(d, x, n);
}

int nr_aiot_d2r_feed(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n)
{
  if (!d || d->running)
    return AIOT_EINVAL;
  return feed(d, ts, x, n);
}

static void *worker(void *arg)
{
  nr_aiot_d2r_t *d = arg;
  pthread_mutex_lock(&d->lock);
  for (;;) {
    while (d->head == d->tail && !d->stop)
      pthread_cond_wait(&d->cond, &d->lock);
    if (d->head == d->tail)
      break; /* stopped, and nothing left */
    d2r_slot_t *s = &d->ring[d->head % d->ring_n];
    pthread_mutex_unlock(&d->lock);
    int ret = feed(d, s->ts, s->x, s->n);
    if (ret < 0)
      LOG_E(NR_PHY, "A-IoT D2R: receiving the slot at %ld failed (%d)\n", (long)s->ts, ret);
    pthread_mutex_lock(&d->lock);
    d->head++;
    pthread_cond_broadcast(&d->cond);
    if (d->received_until >= d->next_log) {
      nr_aiot_d2r_stats_t st = d->stats;
      d->next_log = d->received_until + STATS_EVERY_S * (int64_t)AIOT_SAMPLE_RATE_HZ;
      pthread_mutex_unlock(&d->lock);
      LOG_I(NR_PHY,
            "A-IoT D2R: %" PRIu64 " windows, %" PRIu64 " D2Rs decoded, %" PRIu64 " collided, %" PRIu64 " late; %" PRIu64
            " samples missed, %" PRIu64 " slots overrun\n",
            st.windows,
            st.decoded,
            st.collided,
            st.late,
            st.zero_filled,
            st.overruns);
      pthread_mutex_lock(&d->lock);
    }
  }
  pthread_mutex_unlock(&d->lock);
  return NULL;
}

int nr_aiot_d2r_start(nr_aiot_d2r_t *d, int max_slots, int max_slot_samples, int wait_when_full)
{
  if (!d || d->running || max_slots < 1 || max_slot_samples < 1)
    return AIOT_EINVAL;
  d->ring = calloc((size_t)max_slots, sizeof(*d->ring));
  if (!d->ring)
    return AIOT_ENOMEM;
  d->ring_n = max_slots;
  for (int i = 0; i < max_slots; i++) {
    d->ring[i].x = malloc(sizeof(c16_t) * (size_t)max_slot_samples);
    if (!d->ring[i].x)
      return AIOT_ENOMEM; /* nr_aiot_d2r_free frees what was made */
  }
  d->max_slot_samples = max_slot_samples;
  d->wait_when_full = wait_when_full;
  d->head = d->tail = 0;
  threadCreate(&d->thread, worker, d, "aiot_d2r", -1, OAI_PRIORITY_RT_LOW);
  d->running = 1;
  return 0;
}

nr_aiot_d2r_t *nr_aiot_d2r_open(const nr_aiot_d2r_config_t *cfg,
                                int n_prb,
                                int scs_hz,
                                int64_t sample_rate_hz,
                                int slot_samples,
                                int wait_when_full,
                                const char *band)
{
  nr_aiot_d2r_t *d = nr_aiot_d2r_new(cfg, n_prb, scs_hz, sample_rate_hz);
  if (!d)
    return NULL;
  const int slots = (int)(sample_rate_hz * D2R_QUEUE_MS / 1000 / slot_samples);
  if (nr_aiot_d2r_start(d, slots, slot_samples, wait_when_full) != 0) {
    LOG_E(NR_PHY, "A-IoT D2R: cannot start the receiver thread\n");
    nr_aiot_d2r_free(d);
    return NULL;
  }
  LOG_I(NR_PHY,
        "Ambient IoT D2R in %s PRBs %d to %d, %.0f Hz from the carrier\n",
        band,
        cfg->prb_start,
        cfg->prb_start + cfg->n_rb - 1,
        nr_aiot_d2r_shift_hz(d));
  return d;
}

/* Hands one piece of at most max_slot_samples samples to the worker. */
static int push_piece(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n)
{
  pthread_mutex_lock(&d->lock);
  while (d->wait_when_full && d->tail - d->head == (uint64_t)d->ring_n)
    pthread_cond_wait(&d->cond, &d->lock);
  if (d->tail - d->head == (uint64_t)d->ring_n) {
    d->stats.overruns++;
    pthread_mutex_unlock(&d->lock);
    return AIOT_ESPACE;
  }
  d2r_slot_t *s = &d->ring[d->tail % d->ring_n];
  pthread_mutex_unlock(&d->lock);
  /* only this thread writes slot tail, and the worker reads it only once tail has moved past it */
  memcpy(s->x, x, sizeof(c16_t) * (size_t)n);
  s->ts = ts;
  s->n = n;
  pthread_mutex_lock(&d->lock);
  d->tail++;
  pthread_cond_broadcast(&d->cond);
  pthread_mutex_unlock(&d->lock);
  return 0;
}

int nr_aiot_d2r_push(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n)
{
  if (!d || !d->running || n < 0 || (n && !x))
    return AIOT_EINVAL;
  int done = 0;
  do {
    const int k = n - done < d->max_slot_samples ? n - done : d->max_slot_samples;
    const int ret = push_piece(d, ts + done, x + done, k);
    if (ret < 0)
      return ret;
    done += k;
  } while (done < n);
  return 0;
}

void nr_aiot_d2r_drain(nr_aiot_d2r_t *d)
{
  if (!d || !d->running)
    return;
  pthread_mutex_lock(&d->lock);
  while (d->head != d->tail)
    pthread_cond_wait(&d->cond, &d->lock);
  pthread_mutex_unlock(&d->lock);
}

int nr_aiot_d2r_pop(nr_aiot_d2r_t *d, aiot_d2r_outcome_t *out, int max)
{
  if (!d || max < 0 || (max && !out))
    return AIOT_EINVAL;
  pthread_mutex_lock(&d->lock);
  int n = d->n_out < max ? d->n_out : max;
  memcpy(out, d->out, sizeof(*out) * (size_t)n);
  memmove(d->out, d->out + n, sizeof(*out) * (size_t)(d->n_out - n));
  d->n_out -= n;
  pthread_mutex_unlock(&d->lock);
  return n;
}

int64_t nr_aiot_d2r_received_until(nr_aiot_d2r_t *d)
{
  pthread_mutex_lock(&d->lock);
  int64_t t = d->received_until;
  pthread_mutex_unlock(&d->lock);
  return t;
}

int64_t nr_aiot_d2r_wait(nr_aiot_d2r_t *d, int64_t after, int timeout_ms)
{
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += timeout_ms / 1000;
  until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&d->lock);
  while (d->received_until <= after && !d->stop)
    if (pthread_cond_timedwait(&d->cond, &d->lock, &until) != 0)
      break;
  int64_t t = d->received_until;
  pthread_mutex_unlock(&d->lock);
  return t;
}

nr_aiot_d2r_stats_t nr_aiot_d2r_stats(nr_aiot_d2r_t *d)
{
  pthread_mutex_lock(&d->lock);
  nr_aiot_d2r_stats_t s = d->stats;
  pthread_mutex_unlock(&d->lock);
  return s;
}
