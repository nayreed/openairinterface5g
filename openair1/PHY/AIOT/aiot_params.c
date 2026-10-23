/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_params.c
 * \brief Numerology, sequences and D2R parameter tables (38.291 §4.3, §7.1, §8.3; 38.391 Table 6.2.1.6-1). */

#include "aiot_phy.h"

/* The bit durations 38.391 allows, in tau, with the largest R_SFS and the I_bit scale S of each. */
static const struct {
  int num, den, max_r_sfs, scale;
} bit_durations[] = {
    {2, 1, 128, 1},
    {1, 1, 64, 2},
    {1, 2, 32, 4},
    {1, 4, 16, 8},
    {1, 8, 8, 16},
    {1, 16, 4, 32},
    {1, 32, 2, 64},
    {1, 96, 1, 192},
};
#define N_BIT_DURATIONS ((int)(sizeof(bit_durations) / sizeof(bit_durations[0])))

static const int i_bit_base[4] = {48, 96, 168, 240};

int aiot_m_valid(int m)
{
  return m == 2 || m == 6 || m == 12 || m == 24;
}

int aiot_n_rb_min(int m)
{
  switch (m) {
    case 2:
    case 6:
      return 1;
    case 12:
      return 2;
    case 24:
      return 3;
    default:
      return AIOT_EINVAL;
  }
}

int aiot_m_sequence(int length, uint8_t *out)
{
  int n0, t0, t1;
  if (length == 7) {
    /* x[m+3] = x[m+2] + x[m] */
    out[0] = 1, out[1] = 0, out[2] = 0;
    n0 = 3, t0 = 2, t1 = 0;
  } else if (length == 31) {
    /* x[m+5] = x[m+3] + x[m] */
    out[0] = 0, out[1] = 1, out[2] = 0, out[3] = 0, out[4] = 1;
    n0 = 5, t0 = 3, t1 = 0;
  } else {
    return AIOT_EINVAL;
  }
  for (int n = n0; n < length; n++) {
    int m = n - n0;
    out[n] = out[m + t0] ^ out[m + t1];
  }
  return length;
}

/* The row of a bit duration (num/den reduced or not), or -1. */
static int bit_row(int num, int den)
{
  if (num <= 0 || den <= 0)
    return -1;
  for (int i = 0; i < N_BIT_DURATIONS; i++)
    if ((int64_t)num * bit_durations[i].den == (int64_t)den * bit_durations[i].num)
      return i;
  return -1;
}

int aiot_allowed_r_sfs(int t_bit_num, int t_bit_den, int *out)
{
  int row = bit_row(t_bit_num, t_bit_den);
  if (row < 0)
    return AIOT_EINVAL;
  int n = 0;
  for (int r = 1; r <= bit_durations[row].max_r_sfs; r *= 2)
    out[n++] = r;
  return n;
}

int aiot_i_bit_values(int t_bit_num, int t_bit_den, int *out)
{
  int row = bit_row(t_bit_num, t_bit_den);
  if (row < 0)
    return AIOT_EINVAL;
  for (int i = 0; i < 4; i++)
    out[i] = bit_durations[row].scale * i_bit_base[i];
  return 4;
}

int aiot_bit_samples(const aiot_d2r_params_t *p)
{
  int64_t n = (int64_t)AIOT_TAU * p->t_bit_num;
  if (p->t_bit_den <= 0 || n % p->t_bit_den)
    return AIOT_EINVAL;
  return (int)(n / p->t_bit_den);
}

int aiot_chip_samples(const aiot_d2r_params_t *p)
{
  int bit = aiot_bit_samples(p);
  if (bit < 0 || p->r_sfs <= 0 || bit % (2 * p->r_sfs))
    return AIOT_EINVAL;
  return bit / (2 * p->r_sfs);
}

int aiot_d2r_params_check(const aiot_d2r_params_t *p)
{
  int row = bit_row(p->t_bit_num, p->t_bit_den);
  if (row < 0)
    return AIOT_EINVAL;
  int r = p->r_sfs;
  if (r < 1 || r > bit_durations[row].max_r_sfs || (r & (r - 1)))
    return AIOT_EINVAL;
  if (p->r_block != 1 && p->r_block != 2)
    return AIOT_EINVAL;
  if (p->l_amble != 7 && p->l_amble != 31)
    return AIOT_EINVAL;
  int ok = 0;
  for (int i = 0; i < 4; i++)
    ok |= p->i_bit == bit_durations[row].scale * i_bit_base[i];
  if (!ok)
    return AIOT_EINVAL;
  if (p->modulation != AIOT_OOK && p->modulation != AIOT_BPSK)
    return AIOT_EINVAL;
  if (aiot_chip_samples(p) < 0)
    return AIOT_EINVAL;
  return 0;
}

int aiot_frame_bits(int a_bits, const aiot_d2r_params_t *p)
{
  int b = a_bits + (a_bits > 24 ? 16 : 6);
  int e = b * p->r_block * (p->r_code ? 3 : 1);
  /* floor(E / I_bit) midambles, or ceil(E / I_bit) with the additional one (§6.1.3) */
  int n_mid = p->i_add ? (e + p->i_bit - 1) / p->i_bit : e / p->i_bit;
  return e + (1 + n_mid) * p->l_amble;
}

int aiot_frame_samples(int a_bits, const aiot_d2r_params_t *p)
{
  return aiot_frame_bits(a_bits, p) * aiot_bit_samples(p);
}

/* Fractions of tau as num/den with den > 0. */
typedef struct {
  int64_t n, d;
} frac_t;

static int64_t gcd64(int64_t a, int64_t b)
{
  if (a < 0)
    a = -a;
  while (b) {
    int64_t t = a % b;
    a = b;
    b = t;
  }
  return a ? a : 1;
}

static frac_t fr(int64_t n, int64_t d)
{
  int64_t g = gcd64(n, d);
  frac_t f = {n / g, d / g};
  return f;
}

static frac_t fr_add(frac_t a, frac_t b)
{
  return fr(a.n * b.d + b.n * a.d, a.d * b.d);
}

static frac_t fr_mul(frac_t a, frac_t b)
{
  return fr(a.n * b.n, a.d * b.d);
}

static int fr_cmp(frac_t a, frac_t b) /* sign of a - b */
{
  int64_t x = a.n * b.d - b.n * a.d;
  return (x > 0) - (x < 0);
}

/* Table 7.1.2-3, in tau */
static frac_t t_offset(frac_t t_chip, int m)
{
  if (fr_cmp(t_chip, fr(1, 2)) >= 0)
    return fr(10, 1);
  if (fr_cmp(t_chip, fr(1, 16)) >= 0)
    return fr(5, 1);
  if (fr_cmp(t_chip, fr(1, 32)) == 0)
    return fr(1, 1);
  return m == 2 ? fr(1, 1) : fr(1, 4);
}

int aiot_t_r_to_d(const aiot_d2r_params_t *p,
                  int m_chip_symb,
                  aiot_d2r_msg_t msg,
                  int n_tbs_bytes,
                  const int *candidate_r_sfs,
                  int n_candidates,
                  int earlier_occasion,
                  int d2r_chips)
{
  frac_t t_bit = fr(p->t_bit_num, p->t_bit_den);
  frac_t t_chip = fr(t_bit.n, t_bit.d * 2 * p->r_sfs);
  frac_t t_chip_prime;
  if (msg == AIOT_MSG1 || msg == AIOT_MSG2) {
    if (n_candidates <= 0 || !candidate_r_sfs) {
      t_chip_prime = t_chip;
    } else {
      t_chip_prime = fr(t_bit.n, t_bit.d * 2 * candidate_r_sfs[0]);
      for (int i = 1; i < n_candidates; i++) {
        frac_t c = fr(t_bit.n, t_bit.d * 2 * candidate_r_sfs[i]);
        if (fr_cmp(c, t_chip_prime) > 0)
          t_chip_prime = c;
      }
    }
  } else if (msg == AIOT_CFA || msg == AIOT_DATA) {
    t_chip_prime = t_chip;
  } else {
    return AIOT_EINVAL;
  }
  frac_t t_off = t_offset(t_chip_prime, m_chip_symb);
  frac_t t;
  if (msg == AIOT_MSG1) {
    t = earlier_occasion ? fr_mul(fr(5, 4), fr_add(t_off, fr_mul(fr(d2r_chips, 1), t_chip))) : t_off;
  } else if ((msg == AIOT_MSG2 || msg == AIOT_CFA) && n_tbs_bytes <= 32) {
    /* Table 7.1.2-1 (only with channel coding) */
    int extra = p->r_code && fr_cmp(t_chip_prime, fr(1, 32)) <= 0;
    t = fr_add(t_off, fr(extra, 1));
  } else {
    /* Table 7.1.2-2 (only with channel coding) */
    int d = p->r_code ? (n_tbs_bytes <= 32 ? 2 : n_tbs_bytes <= 64 ? 4 : 8) : 0;
    t = fr_add(t_off, fr(d, 1));
  }
  frac_t n = fr_mul(t, fr(AIOT_TAU, 1));
  if (n.d != 1)
    return AIOT_EINVAL;
  return (int)n.n;
}

int aiot_timing_tolerance(int t_sfo_samples)
{
  return (t_sfo_samples + 9) / 10;
}

int aiot_t_d2r_min(const aiot_d2r_params_t *p)
{
  int three_chips = 3 * aiot_chip_samples(p);
  /* 10 us is 57.6 samples: rounded up, so that the gap is never shorter than 10 us */
  int ten_us = (AIOT_SAMPLE_RATE_HZ + 99999) / 100000;
  return three_chips > ten_us ? three_chips : ten_us;
}
