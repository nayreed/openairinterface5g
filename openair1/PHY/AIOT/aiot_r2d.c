/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_r2d.c
 * \brief R2D, Reader to device (38.291 §6.2): the Reader's transmitter and the device's envelope-detector
 * receiver.
 *
 * Transmitter: transport block -> CRC (§6.2.2.1) -> Manchester line code (§6.2.2.2, §8.5) -> R-TAS
 * SIP and CAP, postamble (§6.2.3, §6.2.4) -> chips and OOK (§6.2.5, §8.4.1.2) -> padding (§6.2.5)
 * -> OFDM symbols (§6.2.6) -> baseband (§6.2.7). How chips become subcarriers is left to the
 * implementation (§6.2.7): each symbol's chips are drawn as an on/off waveform over the 384-sample
 * useful part, only the N_RB x 12 subcarriers around DC are kept, and the cyclic prefix is added
 * (DFT-s-OFDM in effect, for every M and N_RB). A transmission starts on a half-subframe boundary,
 * so its first symbol has the long CP (l0 = 0), which the device relies on to find its symbols.
 *
 * Receiver (an implementation choice, not specified by 3GPP): the device squares the signal (its
 * envelope detector), finds the SIP by correlating the envelope with the SIP's shape, reads M from
 * the CAP (§7.2.1), decides each Manchester bit by comparing its two chips' energy, and stops at
 * the postamble, "on, on" at a bit boundary, which Manchester data never has. Sums run in a fixed
 * order and the first maximum wins, so the same samples always give the same decisions. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aiot_phy.h"

#define PI 3.14159265358979323846

static const uint8_t SIP[AIOT_N_SIP] = {1, 1, 0, 0, 1, 0, 0, 0}; /* 6.2.3.1 */
static const uint8_t CAP[AIOT_N_CAP] = {1, 0, 1, 0}; /* 6.2.3.2 */
static const uint8_t POSTAMBLE[4] = {1, 1, 1, 1}; /* 6.2.4 */
static const int M_LIST[4] = {2, 6, 12, 24};

#define MAX_RB 32 /* N_RB x 12 subcarriers must fit in the 384-point symbol */

/* ---------------------------------------------------------------------------------------------
 * Transmitter
 * ------------------------------------------------------------------------------------------- */

int aiot_manchester(const uint8_t *bits, int n, uint8_t *out)
{
  /* 8.5: x_j = (2j - 1) v + (1 - j), so 0 -> (1, 0) and 1 -> (0, 1) */
  for (int i = 0; i < n; i++) {
    out[2 * i] = bits[i] ? 0 : 1;
    out[2 * i + 1] = bits[i] ? 1 : 0;
  }
  return 2 * n;
}

/* With M = 24 the last two chips of each symbol after the SIP are set to 1 (6.2.5). */
static int forced(int m, int chip)
{
  return m == 24 && (chip - AIOT_N_SIP) % m >= m - 2;
}

int aiot_r2d_chips(const uint8_t *tb_bits, int a_bits, int m_chip_symb, uint8_t *chips, int max_chips)
{
  int m = m_chip_symb;
  if (!aiot_m_valid(m) || a_bits < 0 || (a_bits && !tb_bits) || !chips)
    return AIOT_EINVAL;
  int len = a_bits + aiot_crc_length(a_bits);
  uint8_t *body = malloc((size_t)(2 * len + 4));
  if (!body)
    return AIOT_ENOMEM;
  uint8_t *tmp = malloc((size_t)len);
  if (!tmp) {
    free(body);
    return AIOT_ENOMEM;
  }
  /* 6.2.2: CRC, then Manchester, then the postamble (6.2.4) */
  static const uint8_t none = 0;
  aiot_attach_crc(a_bits ? tb_bits : &none, a_bits, tmp);
  aiot_manchester(tmp, len, body);
  free(tmp);
  memcpy(body + 2 * len, POSTAMBLE, 4);
  int body_n = 2 * len + 4;

  int n = 0, rc = 0;
#define PUT(c)            \
  do {                    \
    if (n >= max_chips) { \
      rc = AIOT_ESPACE;   \
      goto done;          \
    }                     \
    chips[n++] = (c);     \
  } while (0)
  for (int i = 0; i < AIOT_N_SIP; i++)
    PUT(SIP[i]);
  for (int i = 0; i < AIOT_N_CAP; i++)
    PUT(CAP[i]);
  int k = 0;
  while (k < body_n) {
    if (forced(m, n))
      PUT(1);
    else
      PUT(body[k++]);
  }
  /* Padding until (chips - N_SIP) mod M = 0: zeros cannot form another SIP, and with M = 24 the
   * last two chips of the symbol are ones as the forced chips are. */
  while ((n - AIOT_N_SIP) % m) {
    uint8_t pad = forced(m, n) ? 1 : 0;
    PUT(pad);
  }
#undef PUT
  rc = n;
done:
  free(body);
  return rc;
}

int aiot_r2d_n_symbols(int n_chips, int m_chip_symb)
{
  return 2 + (n_chips - AIOT_N_SIP) / m_chip_symb;
}

/* Sample index where symbol n starts (the sum of the CPs and useful parts of symbols 0..n-1);
 * the long CP is at l mod 7 == 0. */
static int64_t symbol_start(int n)
{
  return (int64_t)n * (AIOT_N_FFT + AIOT_CP_SHORT) + 3 * (int64_t)((n + 6) / 7);
}

static int cp_length(int l)
{
  return l % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
}

int aiot_r2d_n_samples(int n_sym)
{
  return (int)symbol_start(n_sym);
}

int aiot_r2d_n_resource_elements(int n_sym, int n_rb)
{
  return n_sym * n_rb * AIOT_SC_PER_RB;
}

int aiot_r2d_resource_elements(const uint8_t *chips, int n_chips, int m_chip_symb, int n_rb, cd_t *re, int max_re)
{
  int m = m_chip_symb;
  if (!aiot_m_valid(m) || n_rb < aiot_n_rb_min(m) || n_rb > MAX_RB || n_chips < AIOT_N_SIP || !chips || !re)
    return AIOT_EINVAL;
  if ((n_chips - AIOT_N_SIP) % m)
    return AIOT_EINVAL;
  int n_sym = aiot_r2d_n_symbols(n_chips, m);
  const int K = n_rb * AIOT_SC_PER_RB;
  if (n_sym * K > max_re)
    return AIOT_ESPACE;

  /* Each symbol's chips are drawn as an on/off waveform over the 384-sample useful part, and only
   * the subcarriers q - K/2, q = 0..K-1, are kept (6.2.7). The kept bins are computed with a direct
   * DFT: K of the 384 bins, fewer operations than a full FFT. They are scaled by 1/384, so that
   * the waveform is their plain sum over the subcarriers. */
  const int N = AIOT_N_FFT;
  double wc[AIOT_N_FFT], ws[AIOT_N_FFT];
  for (int i = 0; i < N; i++) {
    wc[i] = cos(2.0 * PI * i / N);
    ws[i] = sin(2.0 * PI * i / N);
  }
  uint8_t x[AIOT_N_FFT];
  for (int l = 0; l < n_sym; l++) {
    const uint8_t *c;
    int per;
    if (l < 2) {
      c = chips + AIOT_SIP_CHIPS_PER_SYMBOL * l;
      per = AIOT_SIP_CHIPS_PER_SYMBOL;
    } else {
      c = chips + AIOT_N_SIP + (l - 2) * m;
      per = m;
    }
    int w = N / per;
    for (int i = 0; i < N; i++)
      x[i] = c[i / w] ? 1 : 0;
    cd_t *X = re + (size_t)l * K;
    for (int q = 0; q < K; q++) {
      int kk = (q - K / 2 + N) % N;
      double r = 0.0, im = 0.0;
      for (int i = 0; i < N; i++) {
        if (!x[i])
          continue;
        int t = (kk * i) % N;
        r += wc[t];
        im -= ws[t];
      }
      X[q].r = r / N;
      X[q].i = im / N;
    }
  }
  return n_sym * K;
}

int aiot_r2d_waveform(const uint8_t *chips, int n_chips, int m_chip_symb, int n_rb, cd_t *out, int max_out)
{
  int m = m_chip_symb;
  if (!aiot_m_valid(m) || n_rb < aiot_n_rb_min(m) || n_rb > MAX_RB || n_chips < AIOT_N_SIP || !chips || !out)
    return AIOT_EINVAL;
  int n_sym = aiot_r2d_n_symbols(n_chips, m);
  int total = aiot_r2d_n_samples(n_sym);
  if (total > max_out)
    return AIOT_ESPACE;
  const int N = AIOT_N_FFT;
  const int K = n_rb * AIOT_SC_PER_RB;
  cd_t *re = malloc((size_t)n_sym * K * sizeof(cd_t));
  if (!re)
    return AIOT_ENOMEM;
  int rc = aiot_r2d_resource_elements(chips, n_chips, m, n_rb, re, n_sym * K);
  if (rc < 0) {
    free(re);
    return rc;
  }

  /* Each symbol: the inverse DFT of its K subcarriers, then the cyclic prefix. */
  double wc[AIOT_N_FFT], ws[AIOT_N_FFT];
  for (int i = 0; i < N; i++) {
    wc[i] = cos(2.0 * PI * i / N);
    ws[i] = sin(2.0 * PI * i / N);
  }
  cd_t u[AIOT_N_FFT];
  int pos = 0;
  for (int l = 0; l < n_sym; l++) {
    const cd_t *X = re + (size_t)l * K;
    for (int i = 0; i < N; i++) {
      double r = 0.0, im = 0.0;
      for (int q = 0; q < K; q++) {
        int kk = (q - K / 2 + N) % N;
        int t = (kk * i) % N;
        r += X[q].r * wc[t] - X[q].i * ws[t];
        im += X[q].r * ws[t] + X[q].i * wc[t];
      }
      u[i].r = r;
      u[i].i = im;
    }
    int cp = cp_length(l);
    memcpy(out + pos, u + N - cp, (size_t)cp * sizeof(cd_t));
    pos += cp;
    memcpy(out + pos, u, (size_t)N * sizeof(cd_t));
    pos += N;
  }
  free(re);
  return pos;
}

int aiot_r2d_transmission(const uint8_t *tb_bits, int a_bits, int m_chip_symb, int n_rb, cd_t *out, int max_out)
{
  if (!aiot_m_valid(m_chip_symb) || a_bits < 0)
    return AIOT_EINVAL;
  int max_chips = 4 * (a_bits + 16) + 64; /* the body, the forced chips of M = 24 and the padding */
  uint8_t *chips = malloc((size_t)max_chips);
  if (!chips)
    return AIOT_ENOMEM;
  int n = aiot_r2d_chips(tb_bits, a_bits, m_chip_symb, chips, max_chips);
  int rc = n < 0 ? n : aiot_r2d_waveform(chips, n, m_chip_symb, n_rb, out, max_out);
  free(chips);
  return rc;
}

/* ---------------------------------------------------------------------------------------------
 * Device receiver
 * ------------------------------------------------------------------------------------------- */

#define DECIM 6 /* the SIP search runs on the envelope averaged over 6 samples */
#define SIP_SHAPE_MIN 0.8 /* normalized correlation needed to accept a SIP */
#define SIP_CONTRAST_MIN 4.0 /* on/off energy ratio of the SIP needed to accept it (6 dB) */
#define SIP_LEN (2 * AIOT_N_FFT + AIOT_CP_LONG + AIOT_CP_SHORT) /* the SIP's two symbols with their CPs */
#define TMPL_DEC (SIP_LEN / DECIM)
#define MAX_BITS (8 * AIOT_MAX_TBS_BYTES + 16 + 8) /* the largest R2D TB and its CRC, plus one byte */

struct aiot_r2d_rx {
  double *env; /* squared envelope not yet consumed */
  size_t len, cap;
  double *cs; /* running sum of env, one leading zero; valid when cs_ok */
  size_t cs_cap;
  int cs_ok;
  int64_t base; /* sample count of env[0] */
  int enabled;
  double tmpl_full[SIP_LEN];
  double tmpl_dec[TMPL_DEC];
  double tmpl_dec_energy;
};

/* Pairwise summation: the rounding error of a mean over a long envelope grows with log n, not n. */
static double pairwise_sum(const double *a, size_t n)
{
  if (n < 8) {
    double res = 0.0;
    for (size_t i = 0; i < n; i++)
      res += a[i];
    return res;
  } else if (n <= 128) {
    double r[8];
    size_t i;
    for (i = 0; i < 8; i++)
      r[i] = a[i];
    for (i = 8; i < n - (n % 8); i += 8)
      for (int j = 0; j < 8; j++)
        r[j] += a[i + j];
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; i++)
      res += a[i];
    return res;
  }
  size_t n2 = n / 2;
  n2 -= n2 % 8;
  return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

static double envelope(cd_t s)
{
  double a = hypot(s.r, s.i);
  return a * a;
}

aiot_r2d_rx_t *aiot_r2d_rx_new(void)
{
  aiot_r2d_rx_t *rx = calloc(1, sizeof(*rx));
  if (!rx)
    return NULL;
  rx->enabled = 1;
  /* The envelope a device sees for the SIP: its two symbols, on/off, with their CPs. */
  uint8_t chips[AIOT_N_SIP + 2];
  memcpy(chips, SIP, AIOT_N_SIP);
  chips[AIOT_N_SIP] = chips[AIOT_N_SIP + 1] = 0;
  cd_t wave[3 * (AIOT_N_FFT + AIOT_CP_LONG)];
  if (aiot_r2d_waveform(chips, AIOT_N_SIP + 2, 2, 1, wave, (int)(sizeof(wave) / sizeof(wave[0]))) < SIP_LEN) {
    free(rx);
    return NULL;
  }
  double tmpl[SIP_LEN], dec[TMPL_DEC];
  for (int i = 0; i < SIP_LEN; i++)
    tmpl[i] = envelope(wave[i]);
  double mean = pairwise_sum(tmpl, SIP_LEN) / SIP_LEN;
  for (int i = 0; i < SIP_LEN; i++)
    rx->tmpl_full[i] = tmpl[i] - mean;
  for (int i = 0; i < TMPL_DEC; i++) {
    double s = 0.0;
    for (int j = 0; j < DECIM; j++)
      s += tmpl[i * DECIM + j];
    dec[i] = s / DECIM;
  }
  double dmean = pairwise_sum(dec, TMPL_DEC) / TMPL_DEC;
  double e = 0.0;
  for (int i = 0; i < TMPL_DEC; i++) {
    rx->tmpl_dec[i] = dec[i] - dmean;
    e += rx->tmpl_dec[i] * rx->tmpl_dec[i];
  }
  rx->tmpl_dec_energy = e;
  return rx;
}

void aiot_r2d_rx_free(aiot_r2d_rx_t *rx)
{
  if (!rx)
    return;
  free(rx->env);
  free(rx->cs);
  free(rx);
}

void aiot_r2d_rx_set_enabled(aiot_r2d_rx_t *rx, int enabled)
{
  if (rx)
    rx->enabled = !!enabled;
}

static void consume(aiot_r2d_rx_t *rx, int64_t n)
{
  if (n > (int64_t)rx->len)
    n = (int64_t)rx->len;
  if (n < 0)
    n = 0;
  memmove(rx->env, rx->env + n, (rx->len - (size_t)n) * sizeof(double));
  rx->len -= (size_t)n;
  rx->base += n;
  rx->cs_ok = 0; /* the running sum restarts at the new first sample */
}

/* Running sum of env (one leading zero), rebuilt only when env has changed. */
static const double *csum(aiot_r2d_rx_t *rx)
{
  if (!rx->cs_ok) {
    if (rx->cs_cap < rx->len + 1) {
      double *p = realloc(rx->cs, (rx->len + 1) * sizeof(double));
      if (!p)
        return NULL;
      rx->cs = p;
      rx->cs_cap = rx->len + 1;
    }
    double s = 0.0;
    rx->cs[0] = 0.0;
    for (size_t i = 0; i < rx->len; i++) {
      s += rx->env[i];
      rx->cs[i + 1] = s;
    }
    rx->cs_ok = 1;
  }
  return rx->cs;
}

/* Mean envelope energy over the middle of chip number `chip` of a frame whose symbol 0 starts at
 * `start`, M = m; the edges are left out, where the band-limited chip rings. Returns 0 when the
 * chip is not all in the buffer yet. */
static int chip_energy(aiot_r2d_rx_t *rx, const double *cs, int64_t start, int chip, int m, double *out)
{
  int l, k, per;
  if (chip < AIOT_N_SIP) {
    l = chip / AIOT_SIP_CHIPS_PER_SYMBOL;
    k = chip % AIOT_SIP_CHIPS_PER_SYMBOL;
    per = AIOT_SIP_CHIPS_PER_SYMBOL;
  } else {
    l = 2 + (chip - AIOT_N_SIP) / m;
    k = (chip - AIOT_N_SIP) % m;
    per = m;
  }
  int64_t sym = start + symbol_start(l) + cp_length(l);
  int w = AIOT_N_FFT / per;
  int edge = w / 8 > 1 ? w / 8 : 1;
  int64_t a = sym + (int64_t)k * w + edge, b = sym + (int64_t)(k + 1) * w - edge;
  if (b > (int64_t)rx->len)
    return 0;
  *out = (cs[b] - cs[a]) / (double)(b - a);
  return 1;
}

/* Normalized correlation of window i of the decimated envelope with the decimated SIP template. */
static double norm_corr(const aiot_r2d_rx_t *rx, const double *d, const double *dsum, const double *dsq, size_t i)
{
  double c = 0.0;
  for (int k = 0; k < TMPL_DEC; k++)
    c += d[i + k] * rx->tmpl_dec[k];
  double s1 = dsum[i + TMPL_DEC] - dsum[i];
  double s2 = dsq[i + TMPL_DEC] - dsq[i];
  double var = s2 - s1 * s1 / TMPL_DEC;
  if (var < 1e-300)
    var = 1e-300;
  return c / sqrt(var * rx->tmpl_dec_energy);
}

/* The first SIP in the buffer: 1 and (start of symbol 0 in env, on level, off level), 0 if none,
 * or a negative error. */
static int find_sip(aiot_r2d_rx_t *rx, int *start_out, double *on_out, double *off_out)
{
  const double *env = rx->env;
  if (!rx->enabled || rx->len < SIP_LEN + DECIM)
    return 0;
  size_t n_dec = rx->len / DECIM;
  if (n_dec < TMPL_DEC)
    return 0;
  size_t n_win = n_dec - TMPL_DEC + 1;
  double *buf = malloc((3 * n_dec + 2 + n_win) * sizeof(double));
  if (!buf)
    return AIOT_ENOMEM;
  double *d = buf, *dsum = d + n_dec, *dsq = dsum + n_dec + 1, *rho = dsq + n_dec + 1;
  for (size_t i = 0; i < n_dec; i++) {
    double s = 0.0;
    for (int j = 0; j < DECIM; j++)
      s += env[i * DECIM + j];
    d[i] = s / DECIM;
  }
  dsum[0] = dsq[0] = 0.0;
  for (size_t i = 0; i < n_dec; i++) {
    dsum[i + 1] = dsum[i] + d[i];
    dsq[i + 1] = dsq[i] + d[i] * d[i];
  }
  for (size_t i = 0; i < n_win; i++)
    rho[i] = -2.0; /* normalized correlation of window i with the template, computed on demand */

#define RHO(i) (rho[i] > -1.5 ? rho[i] : (rho[i] = norm_corr(rx, d, dsum, dsq, (i))))

  const double *cs = csum(rx);
  if (!cs) {
    free(buf);
    return AIOT_ENOMEM;
  }
  int found = 0;
  for (size_t i = 0; i < n_win && !found; i++) {
    if (!(RHO(i) > SIP_SHAPE_MIN))
      continue;
    /* take the best match among neighbours, then refine at full rate */
    size_t j = i, hi_n = i + 4 < n_win ? i + 4 : n_win;
    for (size_t q = i + 1; q < hi_n; q++)
      if (RHO(q) > RHO(j))
        j = q;
    int64_t lo = (int64_t)j * DECIM - DECIM, hi = (int64_t)j * DECIM + DECIM;
    if (lo < 0)
      lo = 0;
    if ((int64_t)rx->len - SIP_LEN < hi)
      hi = (int64_t)rx->len - SIP_LEN;
    if (hi < lo)
      continue;
    int64_t best = lo;
    double best_v = 0.0;
    for (int64_t s = lo; s <= hi; s++) {
      double v = 0.0;
      for (int q = 0; q < SIP_LEN; q++)
        v += env[s + q] * rx->tmpl_full[q];
      if (s == lo || v > best_v) {
        best = s;
        best_v = v;
      }
    }
    double lev[AIOT_N_SIP];
    for (int c = 0; c < AIOT_N_SIP; c++)
      if (!chip_energy(rx, cs, best, c, 1, &lev[c]))
        lev[c] = NAN;
    double on = 0.0, off = 0.0;
    int n_on = 0, n_off = 0;
    for (int c = 0; c < AIOT_N_SIP; c++) {
      if (SIP[c]) {
        on += lev[c];
        n_on++;
      } else {
        off += lev[c];
        n_off++;
      }
    }
    on /= n_on;
    off /= n_off;
    if (on > SIP_CONTRAST_MIN * (off > 1e-300 ? off : 1e-300)) {
      *start_out = (int)best;
      *on_out = on;
      *off_out = off;
      found = 1;
    }
  }
  free(buf);
  return found;
}

/* Read the frame whose SIP starts at `start` (7.2): 2 and the frame (end = sample after its last
 * chip, in env), 1 when it needs more samples, 0 when it is not a frame, or a negative error. */
static int decode(aiot_r2d_rx_t *rx, int start, double on, double off, aiot_r2d_frame_t *fr, int64_t *end)
{
  const double *cs = csum(rx);
  if (!cs)
    return AIOT_ENOMEM;
  double thr = 0.5 * (on + off);
  /* CAP: try each M, keep the one whose four chips best show on, off, on, off (7.2.1) */
  int best_m = 0;
  double best_score = -INFINITY;
  for (int mi = 0; mi < 4; mi++) {
    int m = M_LIST[mi];
    double e[AIOT_N_CAP];
    for (int c = 0; c < AIOT_N_CAP; c++)
      if (!chip_energy(rx, cs, start, AIOT_N_SIP + c, m, &e[c]))
        return 1;
    double score = (e[0] + e[2] - e[1] - e[3]) / (on - off);
    if (score > best_score) {
      best_m = m;
      best_score = score;
    }
  }
  if (best_score < 1.0)
    return 0;
  int m = best_m;
  /* PRDCH: Manchester pairs from chip N_SIP + N_CAP (7.2.2) until the postamble */
  int chip = AIOT_N_SIP + AIOT_N_CAP;
  uint8_t bits[MAX_BITS + 1];
  int nb = 0;
  while (nb <= MAX_BITS) {
    int pair[2], np = 0;
    while (np < 2) {
      if (!forced(m, chip))
        pair[np++] = chip;
      chip++;
    }
    double e0, e1;
    if (!chip_energy(rx, cs, start, pair[0], m, &e0) || !chip_energy(rx, cs, start, pair[1], m, &e1))
      return 1;
    if (e0 > thr && e1 > thr) {
      /* postamble: the frame ends after its four chips and the padding */
      int n_chips = chip, left = 2; /* the postamble's last two chips, then padding */
      while (left) {
        if (!forced(m, n_chips))
          left--;
        n_chips++;
      }
      while ((n_chips - AIOT_N_SIP) % m)
        n_chips++;
      *end = start + symbol_start(aiot_r2d_n_symbols(n_chips, m));
      /* the transport block is B - L bits, B = bits, with the CRC check of aiot_check_crc */
      if (nb < 7 || (nb >= 31 && nb <= 40))
        return 0;
      int a;
      if (aiot_check_crc(bits, nb, &a) != 1)
        return 0;
      memcpy(fr->bits, bits, (size_t)a);
      fr->n_bits = a;
      fr->m_chip_symb = m;
      return 2;
    }
    bits[nb++] = e0 > e1 ? 0 : 1;
  }
  return 0;
}

int aiot_r2d_rx_receive(aiot_r2d_rx_t *rx, const cd_t *y, int n, aiot_r2d_frame_t *frames, int max_frames)
{
  if (!rx || n < 0 || (n && !y) || max_frames < 0 || (max_frames && !frames))
    return AIOT_EINVAL;
  if (rx->len + (size_t)n > rx->cap) {
    size_t cap = rx->cap ? rx->cap : 4096;
    while (cap < rx->len + (size_t)n)
      cap *= 2;
    double *p = realloc(rx->env, cap * sizeof(double));
    if (!p)
      return AIOT_ENOMEM;
    rx->env = p;
    rx->cap = cap;
  }
  for (int i = 0; i < n; i++)
    rx->env[rx->len + i] = envelope(y[i]);
  rx->len += (size_t)n;
  rx->cs_ok = 0;

  int nf = 0;
  for (;;) {
    if (nf >= max_frames)
      return nf; /* the caller's room is full; whatever follows stays in the buffer */
    int start;
    double on, off;
    int f = find_sip(rx, &start, &on, &off);
    if (f < 0)
      return f;
    if (!f)
      break;
    int64_t end = 0;
    int res = decode(rx, start, on, off, &frames[nf], &end);
    if (res < 0)
      return res;
    if (res == 1) {
      /* keep from the SIP on, and try again with more samples */
      consume(rx, start);
      return nf;
    }
    if (res == 2) {
      frames[nf].end_sample = rx->base + end;
      nf++;
      consume(rx, end);
    } else {
      /* not a frame: skip past this SIP candidate */
      consume(rx, start + SIP_LEN / 2);
    }
  }
  /* Keep what a SIP starting near the end may still need. */
  int64_t keep = (int64_t)rx->len - SIP_LEN - 2 * AIOT_N_FFT;
  consume(rx, keep > 0 ? keep : 0);
  return nf;
}
