/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_d2r.c
 * \brief D2R, device to Reader (38.291 §6.1): the device's frame and the Reader's receiver.
 *
 * Transmitter: transport block -> CRC (§6.1.2.1) -> block repetition (§6.1.2.2) -> optional TBCC
 * (§6.1.2.3, §8.2) -> preamble and midambles (§6.1.3, §8.3) -> small-frequency-shift modulation,
 * OOK or BPSK (§6.1.4, §8.4) -> chips. Each bit is 2 R_SFS chips: a square wave at R_SFS / T_bit
 * whose sign carries the bit (OOK adds a constant half). The device's backscatter multiplies the
 * carrier it receives by these chips (§6.1.5); the caller, which models the channel, does that.
 *
 * Receiver (an implementation choice, not specified by 3GPP): the Reader knows when and how a
 * device will answer (its own scheduling, §7.1.2), so for each expected D2R it searches the window
 * around the expected start for the preamble of each R_SFS it allowed, by correlating with the
 * preamble's +-1 chip pattern. That pattern has zero mean in every bit, so the carrier leak and
 * OOK's constant drop out. At the best position it takes each bit's correlation with its pattern
 * (soft bit), estimates the channel from all ambles and the noise from what they leave over, and
 * accepts the preamble when it stands DETECT_THRESHOLD_DB above that noise. Phase noise moves the
 * carrier leak only across its own direction, so the noise and the preamble's correlation are
 * measured on the leak's axis and across it apart. It then combines repetitions, decodes the TBCC
 * and checks the CRC; with SIC it rebuilds the decoded frames, fits their amplitudes together,
 * subtracts them and searches again. A strong frame leaks a weak copy into the other shifts'
 * correlations, so each shift is read a second time with the frames decoded on the other shifts
 * (by every expected D2R finishing together) taken out, and of frames with the same bits within
 * two bits of each other only the strongest counts. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aiot_phy.h"

/* A preamble is accepted when its correlation reaches this SNR over the noise measured on the
 * frame's ambles, after the bit correlation. */
#define DETECT_THRESHOLD_DB 13.0
/* The search looks this many samples past each end of the +-te window; a peak there is a frame
 * starting outside the window, not one in it. */
#define SEARCH_GUARD 1
#define PI 3.14159265358979323846
/* V at most: E = 6096 bits and a midamble every 48 bits */
#define AIOT_D2R_MAX_V (6 * (AIOT_MAX_TB_BITS + 16) + 130 * 31)

/* ---------------------------------------------------------------------------------------------
 * Transmitter
 * ------------------------------------------------------------------------------------------- */

/* B = A + L, E after block repetition and the optional TBCC (§6.1.2). */
static int coded_len(int a_bits, const aiot_d2r_params_t *p)
{
  return (a_bits + aiot_crc_length(a_bits)) * p->r_block * (p->r_code ? 3 : 1);
}

/* V for a given E (§6.1.3): floor(E / I_bit) midambles, ceil with the additional one. */
static int frame_bits_from_e(int e_bits, const aiot_d2r_params_t *p)
{
  int n_mid = p->i_add ? (e_bits + p->i_bit - 1) / p->i_bit : e_bits / p->i_bit;
  return e_bits + (1 + n_mid) * p->l_amble;
}

static int tx_args_ok(int a_bits, const aiot_d2r_params_t *p)
{
  return a_bits >= 0 && a_bits <= AIOT_MAX_TB_BITS && aiot_d2r_params_check(p) == 0;
}

int aiot_d2r_coded_bits(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, uint8_t *e, int max_e)
{
  if (!tx_args_ok(a_bits, p))
    return AIOT_EINVAL;
  int b = a_bits + aiot_crc_length(a_bits);
  int n_e = coded_len(a_bits, p);
  if (n_e > max_e)
    return AIOT_ESPACE;
  uint8_t word[AIOT_MAX_TB_BITS + 16];
  aiot_attach_crc(tb_bits, a_bits, word);
  if (!p->r_code) {
    aiot_repeat_block(word, b, p->r_block, e);
    return n_e;
  }
  uint8_t rep[2 * (AIOT_MAX_TB_BITS + 16)];
  aiot_repeat_block(word, b, p->r_block, rep);
  aiot_tbcc_encode(rep, b * p->r_block, e);
  return n_e;
}

int aiot_d2r_amble_layout(int e_bits, const aiot_d2r_params_t *p, int *lay, int max_v)
{
  /* only L_amble, I_bit and I_add matter here, so any positive I_bit is accepted */
  if (e_bits < 0 || (p->l_amble != 7 && p->l_amble != 31) || p->i_bit < 1)
    return AIOT_EINVAL;
  int v = frame_bits_from_e(e_bits, p);
  if (v > max_v)
    return AIOT_ESPACE;
  int l = p->l_amble;
  for (int i = 0; i < v; i++)
    lay[i] = -1;
  for (int j = 0; j < l; j++)
    lay[j] = j;
  if (e_bits >= p->i_bit) {
    for (int k = 1; k <= e_bits / p->i_bit; k++) {
      int at = k * (l + p->i_bit);
      for (int j = 0; j < l && at + j < v; j++)
        lay[at + j] = j;
    }
  }
  if (p->i_add)
    for (int j = 0; j < l; j++)
      lay[v - l + j] = j;
  return v;
}

int aiot_d2r_assemble(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, uint8_t *v, int max_v)
{
  if (!tx_args_ok(a_bits, p))
    return AIOT_EINVAL;
  int n_e = coded_len(a_bits, p);
  int n_v = frame_bits_from_e(n_e, p);
  if (n_v > max_v)
    return AIOT_ESPACE;
  uint8_t e[6 * (AIOT_MAX_TB_BITS + 16)];
  uint8_t amble[31];
  int lay[AIOT_D2R_MAX_V];
  if ((size_t)n_v > sizeof(lay) / sizeof(lay[0]))
    return AIOT_ESPACE;
  aiot_d2r_coded_bits(tb_bits, a_bits, p, e, (int)sizeof(e));
  aiot_m_sequence(p->l_amble, amble);
  aiot_d2r_amble_layout(n_e, p, lay, n_v);
  int ie = 0;
  for (int i = 0; i < n_v; i++) {
    if (lay[i] >= 0)
      v[i] = amble[lay[i]];
    else if (ie < n_e)
      v[i] = e[ie++];
    else
      return AIOT_EINVAL;
  }
  return ie == n_e ? n_v : AIOT_EINVAL;
}

int aiot_d2r_sfs_chips(const uint8_t *v, int n, const aiot_d2r_params_t *p, double *chips, int max_chips)
{
  if (n < 0 || aiot_d2r_params_check(p) != 0)
    return AIOT_EINVAL;
  int per_bit = 2 * p->r_sfs;
  if ((int64_t)n * per_bit > max_chips)
    return AIOT_ESPACE;
  for (int i = 0; i < n; i++) {
    int b = v[i] & 1;
    for (int c = 0; c < per_bit; c++) {
      int j = c & 1;
      /* §8.4.1.1 OOK: (1, 0) for a 0, (0, 1) for a 1; §8.4.2 BPSK: (+1, -1) and (-1, +1) */
      chips[(size_t)i * per_bit + c] =
          p->modulation == AIOT_OOK ? (double)((2 * j - 1) * b + (1 - j)) : (double)(2 * (2 * j - 1) * b + (1 - 2 * j));
    }
  }
  return n * per_bit;
}

int aiot_d2r_waveform(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, double *out, int max_out)
{
  if (!tx_args_ok(a_bits, p))
    return AIOT_EINVAL;
  int n_v = frame_bits_from_e(coded_len(a_bits, p), p);
  int cs = aiot_chip_samples(p);
  int64_t total = (int64_t)n_v * aiot_bit_samples(p);
  if (total > max_out)
    return AIOT_ESPACE;
  uint8_t *v = malloc((size_t)n_v);
  double *chips = malloc(sizeof(double) * (size_t)n_v * 2 * p->r_sfs);
  if (!v || !chips) {
    free(v);
    free(chips);
    return AIOT_ENOMEM;
  }
  aiot_d2r_assemble(tb_bits, a_bits, p, v, n_v);
  int nc = aiot_d2r_sfs_chips(v, n_v, p, chips, n_v * 2 * p->r_sfs);
  for (int c = 0; c < nc; c++)
    for (int s = 0; s < cs; s++)
      out[(size_t)c * cs + s] = chips[c];
  free(v);
  free(chips);
  return (int)total;
}

/* ---------------------------------------------------------------------------------------------
 * A radix-2 FFT in double precision, for the preamble search. The Reader's receiver sees its own
 * carrier tens of dB above a D2R frame (40 dB in tests/test_d2r.cpp), so the correlation runs in
 * double precision; OAI's DFTs (openair1/PHY/TOOLS) are 16-bit fixed point.
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  size_t n;
  cd_t *tw; /* exp(-2 pi i k / n), k < n / 2 */
} fft_plan_t;

static int fft_plan(fft_plan_t *f, size_t n)
{
  f->n = n;
  f->tw = malloc(sizeof(cd_t) * (n / 2 + 1));
  if (!f->tw)
    return AIOT_ENOMEM;
  for (size_t k = 0; k < n / 2; k++) {
    double a = -2.0 * PI * (double)k / (double)n;
    f->tw[k].r = cos(a);
    f->tw[k].i = sin(a);
  }
  return 0;
}

static void fft(const fft_plan_t *f, cd_t *x)
{
  size_t n = f->n;
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      cd_t t = x[i];
      x[i] = x[j];
      x[j] = t;
    }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    size_t half = len / 2, step = n / len;
    for (size_t i = 0; i < n; i += len) {
      for (size_t k = 0; k < half; k++) {
        cd_t w = f->tw[k * step];
        cd_t *a = &x[i + k], *b = &x[i + k + half];
        double tr = b->r * w.r - b->i * w.i, ti = b->r * w.i + b->i * w.r;
        b->r = a->r - tr;
        b->i = a->i - ti;
        a->r += tr;
        a->i += ti;
      }
    }
  }
}

/* ---------------------------------------------------------------------------------------------
 * Receiver: one expected D2R, one shift factor
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  fft_plan_t plan;
  cd_t *ref_f; /* conj(FFT(pre_ref)) */
  cd_t *work;
  const double *pre_ref;
  int lp; /* preamble length in samples */
  int64_t nwin; /* samples of seg the search looks at: hi - lo + lp */
  /* what the detection test needs */
  int n_bits, bs, l, n_am;
  const double *pat; /* the +-1 pattern of one bit 0 */
  const int *lay; /* amble layout */
  const double *amble; /* +-1 amble */
  double noise_bit; /* thermal noise of one soft bit */
  double ur, ui; /* turns the carrier leak's direction onto the real axis */
  cd_t *z; /* out: soft bits at the best position */
  double hr, hi; /* out: channel */
} search_t;

/* The best preamble position in s, its soft bits and channel, and whether it counts as a preamble:
 * its correlation over the noise left on the ambles reaches the threshold. */
static int search_preamble(search_t *sr, const cd_t *s, int64_t *k_out)
{
  size_t size = sr->plan.n;
  cd_t *w = sr->work;
  int64_t nwin = sr->nwin;
  memcpy(w, s, sizeof(cd_t) * (size_t)nwin);
  memset(w + nwin, 0, sizeof(cd_t) * (size - (size_t)nwin));
  fft(&sr->plan, w);
  /* c = ifft(X conj(R)), as conj(fft(conj(X conj(R)))) / size */
  for (size_t i = 0; i < size; i++) {
    double r = w[i].r * sr->ref_f[i].r - w[i].i * sr->ref_f[i].i;
    double im = w[i].r * sr->ref_f[i].i + w[i].i * sr->ref_f[i].r;
    w[i].r = r;
    w[i].i = -im;
  }
  fft(&sr->plan, w);
  int64_t npos = nwin - sr->lp + 1;
  int64_t k = 0;
  double best = -1.0;
  for (int64_t i = 0; i < npos; i++) {
    double m = w[i].r * w[i].r + w[i].i * w[i].i;
    if (m > best) {
      best = m;
      k = i;
    }
  }
  *k_out = k;

  /* soft bits: correlation of each bit with the bit-0 pattern */
  const cd_t *y = s + k;
  int bs = sr->bs;
  for (int b = 0; b < sr->n_bits; b++) {
    double zr = 0, zi = 0;
    for (int t = 0; t < bs; t++) {
      zr += y[(int64_t)b * bs + t].r * sr->pat[t];
      zi += y[(int64_t)b * bs + t].i * sr->pat[t];
    }
    sr->z[b].r = zr;
    sr->z[b].i = zi;
  }
  /* channel from every amble bit */
  double hr = 0, hi = 0;
  for (int b = 0; b < sr->n_bits; b++) {
    if (sr->lay[b] >= 0) {
      hr += sr->z[b].r * sr->amble[sr->lay[b]];
      hi += sr->z[b].i * sr->amble[sr->lay[b]];
    }
  }
  double pat_energy = (double)bs;
  hr /= sr->n_am * pat_energy;
  hi /= sr->n_am * pat_energy;
  sr->hr = hr;
  sr->hi = hi;
  /* noise per soft bit on each axis (with and across the carrier leak): what the ambles leave,
   * never less than the thermal noise */
  double res_i = 0, res_q = 0, cr = 0, ci = 0;
  for (int b = 0; b < sr->n_bits; b++) {
    if (sr->lay[b] < 0)
      continue;
    double a = sr->amble[sr->lay[b]];
    double dr = sr->z[b].r - hr * pat_energy * a, di = sr->z[b].i - hi * pat_energy * a;
    double xr = dr * sr->ur - di * sr->ui, xi = dr * sr->ui + di * sr->ur;
    res_i += xr * xr;
    res_q += xi * xi;
    if (b < sr->l) {
      cr += sr->z[b].r * a;
      ci += sr->z[b].i * a;
    }
  }
  double dof = sr->n_am > 1 ? sr->n_am - 1 : 1;
  double var_i = res_i / dof, var_q = res_q / dof;
  if (var_i < sr->noise_bit / 2)
    var_i = sr->noise_bit / 2;
  if (var_q < sr->noise_bit / 2)
    var_q = sr->noise_bit / 2;
  double c_i = cr * sr->ur - ci * sr->ui, c_q = cr * sr->ui + ci * sr->ur;
  double snr = 0.5 * (c_i * c_i / var_i + c_q * c_q / var_q) / sr->l;
  return snr >= pow(10.0, DETECT_THRESHOLD_DB / 10) && k >= SEARCH_GUARD && k <= npos - 1 - SEARCH_GUARD;
}

/* Solve G a = b for a real symmetric G (nf x nf) and complex b, Gaussian elimination with
 * partial pivoting. A singular column gets amplitude 0. */
static void solve_normal(int nf, double *g, cd_t *b, cd_t *a)
{
  double scale = 0;
  for (int i = 0; i < nf; i++)
    if (g[i * nf + i] > scale)
      scale = g[i * nf + i];
  for (int c = 0; c < nf; c++) {
    int piv = c;
    for (int r = c + 1; r < nf; r++)
      if (fabs(g[r * nf + c]) > fabs(g[piv * nf + c]))
        piv = r;
    if (fabs(g[piv * nf + c]) <= 1e-12 * scale) {
      for (int r = 0; r < nf; r++)
        g[r * nf + c] = 0;
      g[c * nf + c] = 1;
      b[c].r = b[c].i = 0;
      continue;
    }
    if (piv != c) {
      for (int j = 0; j < nf; j++) {
        double t = g[c * nf + j];
        g[c * nf + j] = g[piv * nf + j];
        g[piv * nf + j] = t;
      }
      cd_t t = b[c];
      b[c] = b[piv];
      b[piv] = t;
    }
    for (int r = c + 1; r < nf; r++) {
      double f = g[r * nf + c] / g[c * nf + c];
      for (int j = c; j < nf; j++)
        g[r * nf + j] -= f * g[c * nf + j];
      b[r].r -= f * b[c].r;
      b[r].i -= f * b[c].i;
    }
  }
  for (int c = nf - 1; c >= 0; c--) {
    double sr = b[c].r, si = b[c].i;
    for (int j = c + 1; j < nf; j++) {
      sr -= g[c * nf + j] * a[j].r;
      si -= g[c * nf + j] * a[j].i;
    }
    a[c].r = sr / g[c * nf + c];
    a[c].i = si / g[c * nf + c];
  }
}

/* Undo coding and repetition, check the CRC. Returns 1 and the transport block, or 0. */
static int read_frame(const double *soft, int a_bits, const aiot_d2r_params_t *p, uint8_t *tb)
{
  int b = a_bits + aiot_crc_length(a_bits);
  int k = b * p->r_block;
  int a;
  uint8_t c[2 * (AIOT_MAX_TB_BITS + 16)];
  if (p->r_code) {
    if (aiot_tbcc_decode(soft, 3 * k, c) != k)
      return AIOT_ENOMEM;
    for (int cp = 0; cp < p->r_block; cp++) { /* any copy whose CRC holds */
      if (aiot_check_crc(c + cp * b, b, &a) == 1) {
        memcpy(tb, c + cp * b, (size_t)a_bits);
        return 1;
      }
    }
    return 0;
  }
  double comb[AIOT_MAX_TB_BITS + 16];
  aiot_combine_repetitions(soft, k, p->r_block, comb);
  for (int i = 0; i < b; i++)
    c[i] = comb[i] < 0;
  if (aiot_check_crc(c, b, &a) == 1) {
    memcpy(tb, c, (size_t)a_bits);
    return 1;
  }
  return 0;
}

/* Search and decode the frames of e on its shift r. Appends to res->decoded and adds to
 * res->collided; cancelled are the frames decoded on other shifts and already taken out of buf. */
static int decode_shift(const cd_t *buf,
                        int64_t nbuf,
                        int64_t base,
                        const aiot_d2r_expect_t *e,
                        int r,
                        double noise_w,
                        int sic,
                        const aiot_d2r_result_t *const *cancelled,
                        int n_cancelled,
                        aiot_d2r_outcome_t *res)
{
  aiot_d2r_params_t p = e->params;
  p.r_sfs = r;
  if (aiot_d2r_params_check(&p) != 0)
    return AIOT_EINVAL;
  int a_bits = e->a_bits;
  int n_bits = aiot_frame_bits(a_bits, &p);
  int bs = aiot_bit_samples(&p), cs = aiot_chip_samples(&p);
  int64_t n = (int64_t)n_bits * bs;
  int64_t lo = e->start - e->te - SEARCH_GUARD - base;
  if (lo < 0)
    lo = 0;
  int64_t hi = e->start + e->te + SEARCH_GUARD - base;
  if (nbuf - n < hi)
    hi = nbuf - n;
  if (hi < lo)
    return 0;
  int64_t seg_len = hi - lo + n;

  int ret = AIOT_ENOMEM;
  int l = p.l_amble;
  int lp = l * bs;
  int n_e = coded_len(a_bits, &p);
  cd_t *seg = malloc(sizeof(cd_t) * (size_t)seg_len);
  cd_t *seg0 = malloc(sizeof(cd_t) * (size_t)seg_len);
  double *pat = malloc(sizeof(double) * (size_t)bs);
  double *pre_ref = malloc(sizeof(double) * (size_t)lp);
  int *lay = malloc(sizeof(int) * (size_t)n_bits);
  double *z_soft = malloc(sizeof(double) * (size_t)n_e);
  cd_t *z = malloc(sizeof(cd_t) * (size_t)n_bits);
  int8_t *waves[AIOT_MAX_SIC_ROUNDS] = {0};
  int64_t offs[AIOT_MAX_SIC_ROUNDS];
  search_t sr = {0};
  if (!seg || !seg0 || !pat || !pre_ref || !lay || !z_soft || !z)
    goto done;
  memcpy(seg, buf + lo, sizeof(cd_t) * (size_t)seg_len);
  memcpy(seg0, seg, sizeof(cd_t) * (size_t)seg_len);

  /* The +-1 pattern of one bit 0 in samples (a bit 1 is its negative) */
  for (int c = 0; c < 2 * r; c++)
    for (int s = 0; s < cs; s++)
      pat[c * cs + s] = (c & 1) ? -1.0 : 1.0;
  /* The reference: the frame's ambles at their places (+-1 per bit) */
  aiot_d2r_amble_layout(n_e, &p, lay, n_bits);
  uint8_t mseq[31];
  double amble[31];
  aiot_m_sequence(l, mseq);
  for (int i = 0; i < l; i++) {
    amble[i] = 1.0 - 2.0 * mseq[i];
    for (int s = 0; s < bs; s++)
      pre_ref[i * bs + s] = amble[i] * pat[s];
  }
  double pat_energy = (double)bs;
  int n_am = 0;
  for (int i = 0; i < n_bits; i++)
    n_am += lay[i] >= 0;
  if (n_bits - n_am != n_e) {
    ret = AIOT_EINVAL;
    goto done;
  }

  sr.lp = lp;
  sr.nwin = hi - lo + lp;
  sr.pre_ref = pre_ref;
  sr.n_bits = n_bits;
  sr.bs = bs;
  sr.l = l;
  sr.n_am = n_am;
  sr.pat = pat;
  sr.lay = lay;
  sr.amble = amble;
  sr.noise_bit = noise_w * pat_energy;
  {
    /* the carrier leak's direction: the window's mean (any direction without a leak) */
    double mr = 0, mi = 0;
    for (int64_t t = 0; t < seg_len; t++) {
      mr += seg0[t].r;
      mi += seg0[t].i;
    }
    mr /= (double)seg_len;
    mi /= (double)seg_len;
    double m = hypot(mr, mi);
    sr.ur = m > 0 ? mr / m : 1.0;
    sr.ui = m > 0 ? -mi / m : 0.0;
  }
  sr.z = z;
  size_t size = 1;
  while (size < (size_t)(sr.nwin + lp - 1))
    size <<= 1;
  if (fft_plan(&sr.plan, size) != 0)
    goto done;
  sr.ref_f = calloc(size, sizeof(cd_t));
  sr.work = malloc(sizeof(cd_t) * size);
  if (!sr.ref_f || !sr.work)
    goto done;
  for (int i = 0; i < lp; i++)
    sr.ref_f[i].r = pre_ref[i];
  fft(&sr.plan, sr.ref_f);
  for (size_t i = 0; i < size; i++)
    sr.ref_f[i].i = -sr.ref_f[i].i;

  int d0 = res->n_decoded, nf = 0, collided = 0;
  for (int round = 0; round < AIOT_MAX_SIC_ROUNDS; round++) {
    int64_t k;
    if (!search_preamble(&sr, seg, &k))
      break;
    double hr = sr.hr, hi_ = sr.hi;
    int ns = 0;
    for (int b = 0; b < n_bits; b++)
      if (lay[b] < 0)
        z_soft[ns++] = z[b].r * hr + z[b].i * hi_; /* real(z conj(h)) */
    aiot_d2r_result_t *d = &res->decoded[d0 + nf];
    int rd = read_frame(z_soft, a_bits, &p, d->bits);
    if (rd == AIOT_ENOMEM)
      goto done;
    if (rd == 0) {
      /* A preamble that does not decode where a frame decoded on another shift starts is what
       * cancelling that frame left of it, not a second frame. */
      int64_t at = base + lo + k, near = 2 * (int64_t)bs;
      int residue = 0;
      for (int i = 0; i < n_cancelled && !residue; i++) {
        int64_t gap = cancelled[i]->start - at;
        residue = (gap < 0 ? -gap : gap) <= near;
      }
      collided = !residue;
      break;
    }
    int dup = 0;
    for (int i = 0; i < nf; i++) {
      aiot_d2r_result_t *o = &res->decoded[d0 + i];
      int64_t dist = o->start - (base + lo + k);
      if (dist < 0)
        dist = -dist;
      if (!memcmp(o->bits, d->bits, (size_t)a_bits) && dist < bs)
        dup = 1;
    }
    if (dup)
      break;
    d->n_bits = a_bits;
    d->r_sfs = r;
    d->start = base + lo + k;
    d->rssi_w = hr * hr + hi_ * hi_;
    d->tag = e->tag;
    /* the +-1 waveform of the frame (the part the pattern sees; OOK's constant half drops out) */
    uint8_t *v = malloc((size_t)n_bits);
    double *chips = malloc(sizeof(double) * (size_t)n_bits * 2 * r);
    waves[nf] = malloc((size_t)n);
    if (!v || !chips || !waves[nf]) {
      free(v);
      free(chips);
      goto done;
    }
    aiot_d2r_assemble(d->bits, a_bits, &p, v, n_bits);
    int nc = aiot_d2r_sfs_chips(v, n_bits, &p, chips, n_bits * 2 * r);
    for (int c = 0; c < nc; c++) {
      double x = p.modulation == AIOT_OOK ? 2 * chips[c] - 1 : chips[c];
      for (int s = 0; s < cs; s++)
        waves[nf][(int64_t)c * cs + s] = (int8_t)x;
    }
    free(v);
    free(chips);
    offs[nf] = k;
    nf++;
    res->n_decoded = d0 + nf;

    /* fit all decoded frames together (normal equations; the waveforms are real), take them out, look again */
    double *g = malloc(sizeof(double) * (size_t)nf * nf);
    cd_t *rhs = malloc(sizeof(cd_t) * (size_t)nf);
    cd_t *amp = malloc(sizeof(cd_t) * (size_t)nf);
    if (!g || !rhs || !amp) {
      free(g);
      free(rhs);
      free(amp);
      goto done;
    }
    for (int i = 0; i < nf; i++) {
      double sr_ = 0, si_ = 0;
      for (int64_t t = 0; t < n; t++) {
        sr_ += waves[i][t] * seg0[offs[i] + t].r;
        si_ += waves[i][t] * seg0[offs[i] + t].i;
      }
      rhs[i].r = sr_;
      rhs[i].i = si_;
      for (int j = i; j < nf; j++) {
        int64_t a0 = offs[i] > offs[j] ? offs[i] : offs[j];
        int64_t a1 = (offs[i] < offs[j] ? offs[i] : offs[j]) + n;
        long acc = 0;
        for (int64_t t = a0; t < a1; t++)
          acc += waves[i][t - offs[i]] * waves[j][t - offs[j]];
        g[i * nf + j] = g[j * nf + i] = (double)acc;
      }
    }
    solve_normal(nf, g, rhs, amp);
    memcpy(seg, seg0, sizeof(cd_t) * (size_t)seg_len);
    for (int i = 0; i < nf; i++) {
      for (int64_t t = 0; t < n; t++) {
        seg[offs[i] + t].r -= amp[i].r * waves[i][t];
        seg[offs[i] + t].i -= amp[i].i * waves[i][t];
      }
      res->decoded[d0 + i].rssi_w = (amp[i].r * amp[i].r + amp[i].i * amp[i].i) * (p.modulation == AIOT_OOK ? 4 : 1);
    }
    free(g);
    free(rhs);
    free(amp);
    if (!sic) {
      /* without SIC a second frame in the window cannot be read; a preamble left is a collision */
      int64_t k2;
      collided = search_preamble(&sr, seg, &k2);
      break;
    }
  }
  res->collided += collided;
  ret = 0;

done:
  for (int i = 0; i < AIOT_MAX_SIC_ROUNDS; i++)
    free(waves[i]);
  free(sr.plan.tw);
  free(sr.ref_f);
  free(sr.work);
  free(seg);
  free(seg0);
  free(pat);
  free(pre_ref);
  free(lay);
  free(z_soft);
  free(z);
  return ret;
}

static int n_shifts(const aiot_d2r_expect_t *e);
static int shift_of(const aiot_d2r_expect_t *e, int i);

/* The +-1 waveform the bit patterns see of a decoded frame: n samples into w. */
static int frame_wave(const uint8_t *bits, int a_bits, const aiot_d2r_params_t *p, double *w, int64_t n)
{
  int n_bits = aiot_frame_bits(a_bits, p), r = p->r_sfs, cs = aiot_chip_samples(p);
  uint8_t *v = malloc((size_t)n_bits);
  double *chips = malloc(sizeof(double) * (size_t)n_bits * 2 * r);
  if (!v || !chips) {
    free(v);
    free(chips);
    return AIOT_ENOMEM;
  }
  aiot_d2r_assemble(bits, a_bits, p, v, n_bits);
  int nc = aiot_d2r_sfs_chips(v, n_bits, p, chips, n_bits * 2 * r);
  for (int c = 0; c < nc && (int64_t)(c + 1) * cs <= n; c++) {
    double x = p->modulation == AIOT_OOK ? 2 * chips[c] - 1 : chips[c];
    for (int t = 0; t < cs; t++)
      w[(int64_t)c * cs + t] = x;
  }
  free(v);
  free(chips);
  return 0;
}

/* buf (in place) with the given decoded frames fitted together and taken out. fe[i] is the
 * expected D2R frame fr[i] was decoded for. */
static int cancel_frames(cd_t *buf,
                         int64_t nbuf,
                         int64_t base,
                         const aiot_d2r_result_t *const *fr,
                         const aiot_d2r_expect_t *const *fe,
                         int nf)
{
  int ret = AIOT_ENOMEM;
  double **waves = calloc((size_t)nf, sizeof(double *));
  int64_t *offs = malloc(sizeof(int64_t) * (size_t)nf), *lens = malloc(sizeof(int64_t) * (size_t)nf);
  double *g = malloc(sizeof(double) * (size_t)nf * nf);
  cd_t *rhs = malloc(sizeof(cd_t) * (size_t)nf), *amp = malloc(sizeof(cd_t) * (size_t)nf);
  if (!waves || !offs || !lens || !g || !rhs || !amp)
    goto done;
  int64_t a = INT64_MAX, b = INT64_MIN;
  for (int i = 0; i < nf; i++) {
    aiot_d2r_params_t p = fe[i]->params;
    p.r_sfs = fr[i]->r_sfs;
    lens[i] = aiot_frame_samples(fr[i]->n_bits, &p);
    offs[i] = fr[i]->start - base;
    waves[i] = malloc(sizeof(double) * (size_t)lens[i]);
    if (!waves[i] || frame_wave(fr[i]->bits, fr[i]->n_bits, &p, waves[i], lens[i]) != 0)
      goto done;
    if (offs[i] < a)
      a = offs[i];
    if (offs[i] + lens[i] > b)
      b = offs[i] + lens[i];
  }
  if (a < 0)
    a = 0;
  if (b > nbuf)
    b = nbuf;
  ret = 0;
  if (b <= a)
    goto done;
  /* normal equations over [a, b): the waveforms are real */
  for (int i = 0; i < nf; i++) {
    int64_t lo = offs[i] > a ? offs[i] : a, hi = offs[i] + lens[i] < b ? offs[i] + lens[i] : b;
    double sr = 0, si = 0;
    for (int64_t t = lo; t < hi; t++) {
      sr += waves[i][t - offs[i]] * buf[t].r;
      si += waves[i][t - offs[i]] * buf[t].i;
    }
    rhs[i].r = sr;
    rhs[i].i = si;
    for (int j = i; j < nf; j++) {
      int64_t l2 = offs[j] > lo ? offs[j] : lo, h2 = offs[j] + lens[j] < hi ? offs[j] + lens[j] : hi;
      double acc = 0;
      for (int64_t t = l2; t < h2; t++)
        acc += waves[i][t - offs[i]] * waves[j][t - offs[j]];
      g[i * nf + j] = g[j * nf + i] = acc;
    }
  }
  solve_normal(nf, g, rhs, amp);
  for (int i = 0; i < nf; i++) {
    int64_t lo = offs[i] > a ? offs[i] : a, hi = offs[i] + lens[i] < b ? offs[i] + lens[i] : b;
    for (int64_t t = lo; t < hi; t++) {
      buf[t].r -= amp[i].r * waves[i][t - offs[i]];
      buf[t].i -= amp[i].i * waves[i][t - offs[i]];
    }
  }

done:
  for (int i = 0; waves && i < nf; i++)
    free(waves[i]);
  free(waves);
  free(offs);
  free(lens);
  free(g);
  free(rhs);
  free(amp);
  return ret;
}

typedef struct {
  double rssi;
  int i, j;
} found_t;

static int stronger_first(const void *pa, const void *pb)
{
  const found_t *a = pa, *b = pb;
  if (a->rssi != b->rssi)
    return a->rssi < b->rssi ? 1 : -1;
  if (a->i != b->i)
    return a->i < b->i ? 1 : -1;
  return a->j < b->j ? 1 : (a->j > b->j ? -1 : 0);
}

#define MAX_DECODED (AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS)

/* Of the frames decoded with the same bits within two bits of each other (what a
 * strong frame leaks into other shifts, or what cancellation leaves of it), only the strongest
 * counts; gone[i][j] is set for the others. */
static int mark_copies(const aiot_d2r_outcome_t *out, int ne, uint8_t (*gone)[MAX_DECODED])
{
  int n = 0;
  for (int i = 0; i < ne; i++) {
    memset(gone[i], 0, sizeof(gone[i]));
    n += out[i].n_decoded;
  }
  if (n < 2)
    return 0;
  found_t *f = malloc(sizeof(*f) * (size_t)n);
  const aiot_d2r_result_t **kept = malloc(sizeof(*kept) * (size_t)n);
  int ret = AIOT_ENOMEM;
  if (!f || !kept)
    goto done;
  int k = 0;
  for (int i = 0; i < ne; i++)
    for (int j = 0; j < out[i].n_decoded; j++)
      f[k++] = (found_t){out[i].decoded[j].rssi_w, i, j};
  qsort(f, (size_t)n, sizeof(*f), stronger_first);
  int nk = 0;
  for (int m = 0; m < n; m++) {
    const aiot_d2r_result_t *d = &out[f[m].i].decoded[f[m].j];
    int64_t near = 2 * (int64_t)aiot_bit_samples(&out[f[m].i].expect.params);
    int copy = 0;
    for (int q = 0; q < nk && !copy; q++) {
      int64_t gap = kept[q]->start - d->start;
      copy =
          kept[q]->n_bits == d->n_bits && memcmp(kept[q]->bits, d->bits, (size_t)d->n_bits) == 0 && (gap < 0 ? -gap : gap) <= near;
    }
    if (copy)
      gone[f[m].i][f[m].j] = 1;
    else
      kept[nk++] = d;
  }
  ret = 0;

done:
  free(f);
  free(kept);
  return ret;
}

/* The expected D2Rs whose windows are all in: each shift of each expected D2R on its own first;
 * then each is read again with what was decoded on the other shifts, by any of them, taken out,
 * and that counts. out[i] gets e[i]'s outcome (expect already set). */
static int decode_all(const cd_t *buf,
                      int64_t nbuf,
                      int64_t base,
                      const aiot_d2r_expect_t *e,
                      int ne,
                      double noise_w,
                      int sic,
                      aiot_d2r_outcome_t *out)
{
  int ret = AIOT_ENOMEM;
  aiot_d2r_outcome_t *first = malloc(sizeof(*first) * (size_t)ne);
  int(*from)[AIOT_MAX_SHIFTS + 1] = malloc(sizeof(*from) * (size_t)ne);
  int(*coll)[AIOT_MAX_SHIFTS] = malloc(sizeof(*coll) * (size_t)ne);
  int cap = ne * AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS;
  const aiot_d2r_result_t **others = malloc(sizeof(*others) * (size_t)cap);
  const aiot_d2r_expect_t **oexp = malloc(sizeof(*oexp) * (size_t)cap);
  cd_t *copy = malloc(sizeof(cd_t) * (size_t)(nbuf ? nbuf : 1));
  uint8_t(*gone)[MAX_DECODED] = malloc(sizeof(*gone) * (size_t)ne);
  if (!first || !from || !coll || !others || !oexp || !copy || !gone)
    goto done;
  for (int i = 0; i < ne; i++) {
    first[i].expect = e[i];
    first[i].n_decoded = 0;
    first[i].collided = 0;
    for (int j = 0; j < n_shifts(&e[i]); j++) {
      from[i][j] = first[i].n_decoded;
      int c0 = first[i].collided;
      ret = decode_shift(buf, nbuf, base, &e[i], shift_of(&e[i], j), noise_w, sic, NULL, 0, &first[i]);
      if (ret < 0)
        goto done;
      coll[i][j] = first[i].collided - c0;
    }
    from[i][n_shifts(&e[i])] = first[i].n_decoded;
  }
  ret = mark_copies(first, ne, gone);
  if (ret < 0)
    goto done;
  for (int i = 0; i < ne; i++) {
    aiot_d2r_outcome_t *o = &out[i];
    for (int j = 0; j < n_shifts(&e[i]); j++) {
      int r = shift_of(&e[i], j), nf = 0;
      for (int i2 = 0; i2 < ne; i2++)
        for (int j2 = 0; j2 < n_shifts(&e[i2]); j2++)
          if (shift_of(&e[i2], j2) != r)
            for (int k = from[i2][j2]; k < from[i2][j2 + 1]; k++) {
              if (gone[i2][k])
                continue;
              others[nf] = &first[i2].decoded[k];
              oexp[nf++] = &e[i2];
            }
      if (!nf) {
        for (int k = from[i][j]; k < from[i][j + 1]; k++)
          o->decoded[o->n_decoded++] = first[i].decoded[k];
        o->collided += coll[i][j];
        continue;
      }
      memcpy(copy, buf, sizeof(cd_t) * (size_t)nbuf);
      ret = cancel_frames(copy, nbuf, base, others, oexp, nf);
      if (ret < 0)
        goto done;
      ret = decode_shift(copy, nbuf, base, &e[i], r, noise_w, sic, others, nf, o);
      if (ret < 0)
        goto done;
    }
  }
  ret = mark_copies(out, ne, gone);
  if (ret < 0)
    goto done;
  for (int i = 0; i < ne; i++) {
    int w = 0;
    for (int j = 0; j < out[i].n_decoded; j++)
      if (!gone[i][j])
        out[i].decoded[w++] = out[i].decoded[j];
    out[i].n_decoded = w;
  }

done:
  free(first);
  free(from);
  free(coll);
  free(others);
  free(oexp);
  free(copy);
  free(gone);
  return ret;
}

/* ---------------------------------------------------------------------------------------------
 * Receiver: the sample buffer and the expected D2Rs
 * ------------------------------------------------------------------------------------------- */

struct aiot_d2r_rx {
  double noise_w;
  int enable_sic;
  cd_t *buf; /* the samples that may still be needed */
  int64_t n, cap;
  int64_t base; /* sample count of buf[0] */
  aiot_d2r_expect_t *ex;
  int n_ex, cap_ex;
};

aiot_d2r_rx_t *aiot_d2r_rx_new(double noise_w, int enable_sic)
{
  aiot_d2r_rx_t *rx = calloc(1, sizeof(*rx));
  if (rx) {
    rx->noise_w = noise_w;
    rx->enable_sic = enable_sic;
  }
  return rx;
}

void aiot_d2r_rx_free(aiot_d2r_rx_t *rx)
{
  if (!rx)
    return;
  free(rx->buf);
  free(rx->ex);
  free(rx);
}

static int n_shifts(const aiot_d2r_expect_t *e)
{
  return e->n_r_sfs ? e->n_r_sfs : 1;
}

static int shift_of(const aiot_d2r_expect_t *e, int i)
{
  return e->n_r_sfs ? e->r_sfs[i] : e->params.r_sfs;
}

/* Expect.last_sample */
static int64_t last_sample(const aiot_d2r_expect_t *e)
{
  int64_t m = 0;
  for (int i = 0; i < n_shifts(e); i++) {
    aiot_d2r_params_t p = e->params;
    p.r_sfs = shift_of(e, i);
    int64_t len = (int64_t)aiot_frame_bits(e->a_bits, &p) * aiot_bit_samples(&p);
    if (len > m)
      m = len;
  }
  return e->start + e->te + SEARCH_GUARD + m;
}

int aiot_d2r_expect_check(const aiot_d2r_expect_t *e)
{
  if (!e || e->a_bits < 0 || e->a_bits > AIOT_MAX_TB_BITS || e->te < 0 || e->n_r_sfs < 0 || e->n_r_sfs > AIOT_MAX_SHIFTS)
    return AIOT_EINVAL;
  for (int i = 0; i < n_shifts(e); i++) {
    aiot_d2r_params_t p = e->params;
    p.r_sfs = shift_of(e, i);
    if (aiot_d2r_params_check(&p) != 0)
      return AIOT_EINVAL;
  }
  return 0;
}

int aiot_d2r_rx_expect(aiot_d2r_rx_t *rx, const aiot_d2r_expect_t *e)
{
  if (!rx || aiot_d2r_expect_check(e) != 0)
    return AIOT_EINVAL;
  if (rx->n_ex == rx->cap_ex) {
    int cap = rx->cap_ex ? 2 * rx->cap_ex : 4;
    aiot_d2r_expect_t *ne = realloc(rx->ex, sizeof(*ne) * (size_t)cap);
    if (!ne)
      return AIOT_ENOMEM;
    rx->ex = ne;
    rx->cap_ex = cap;
  }
  rx->ex[rx->n_ex++] = *e;
  return 0;
}

int aiot_d2r_rx_receive(aiot_d2r_rx_t *rx, const cd_t *y, int n, aiot_d2r_outcome_t *out, int max_out)
{
  if (!rx || n < 0 || (n && !y))
    return AIOT_EINVAL;
  int64_t end = rx->base + rx->n + n;
  int n_done = 0;
  for (int i = 0; i < rx->n_ex; i++)
    n_done += last_sample(&rx->ex[i]) <= end;
  if (n_done > max_out)
    return AIOT_ESPACE;
  if (rx->n + n > rx->cap) {
    int64_t cap = 2 * rx->cap > rx->n + n ? 2 * rx->cap : rx->n + n;
    cd_t *nb = realloc(rx->buf, sizeof(cd_t) * (size_t)cap);
    if (!nb)
      return AIOT_ENOMEM;
    rx->buf = nb;
    rx->cap = cap;
  }
  if (n)
    memcpy(rx->buf + rx->n, y, sizeof(cd_t) * (size_t)n);
  rx->n += n;

  /* split the expectations into done (in order) and still waiting */
  aiot_d2r_expect_t *done = n_done ? malloc(sizeof(*done) * (size_t)n_done) : NULL;
  if (n_done && !done)
    return AIOT_ENOMEM;
  int nd = 0, nk = 0;
  for (int i = 0; i < rx->n_ex; i++) {
    if (last_sample(&rx->ex[i]) <= end)
      done[nd++] = rx->ex[i];
    else
      rx->ex[nk++] = rx->ex[i];
  }
  rx->n_ex = nk;
  if (!nd && !nk) {
    rx->base = end;
    rx->n = 0;
    return 0;
  }
  int ret = nd;
  for (int i = 0; i < nd; i++) {
    out[i].expect = done[i];
    out[i].n_decoded = 0;
    out[i].collided = 0;
  }
  if (nd) {
    int rc = decode_all(rx->buf, rx->n, rx->base, done, nd, rx->noise_w, rx->enable_sic, out);
    if (rc < 0)
      ret = rc;
  }
  free(done);
  int64_t keep_from = end;
  for (int i = 0; i < rx->n_ex; i++)
    if (rx->ex[i].start - rx->ex[i].te - SEARCH_GUARD < keep_from)
      keep_from = rx->ex[i].start - rx->ex[i].te - SEARCH_GUARD;
  int64_t drop = keep_from - rx->base;
  if (drop < 0)
    drop = 0;
  memmove(rx->buf, rx->buf + drop, sizeof(cd_t) * (size_t)(rx->n - drop));
  rx->base += drop;
  rx->n -= drop;
  return ret;
}
