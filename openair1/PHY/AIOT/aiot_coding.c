/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_coding.c
 * \brief CRC, tail-biting convolutional code and block repetition (38.291 clause 8), on OAI's own coders:
 * the A-IoT CRCs are OAI's crc6 and crc16 (gCRC6, gCRC16 of 38.212), and the TBCC is LTE's
 * (36.212 §5.1.3.1, generators 133, 171, 165), encoded by ccodelte_encode and decoded by
 * phy_viterbi_lte_sse2. Bits here are one per byte; OAI's coders take them packed, MSB first. */

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "aiot_phy.h"
#include "PHY/CODING/coding_defs.h"

static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void init_tables(void)
{
  crcTableInit();
  ccodelte_init();
  phy_generate_viterbi_tables_lte();
}

void aiot_pack_bits(const uint8_t *bits, int n, uint8_t *bytes)
{
  memset(bytes, 0, (size_t)(n + 7) / 8);
  for (int i = 0; i < n; i++)
    if (bits[i] & 1)
      bytes[i >> 3] |= 0x80 >> (i & 7);
}

void aiot_unpack_bits(const uint8_t *bytes, int n, uint8_t *bits)
{
  for (int i = 0; i < n; i++)
    bits[i] = (bytes[i >> 3] >> (7 - (i & 7))) & 1;
}

/* The longest word a CRC is computed on: the largest B, and the byte more a receiver tests */
#define CRC_MAX_BITS (AIOT_MAX_WORD_BITS + 8)
/* The TBCC's memory, constraint length 7 */
#define TBCC_MEMORY 6

int aiot_crc(const uint8_t *bits, int n, int L, uint8_t *parity)
{
  if ((L != 6 && L != AIOT_MAX_CRC_LEN) || n < 0 || n > CRC_MAX_BITS)
    return AIOT_EINVAL;
  pthread_once(&tables_once, init_tables);
  uint8_t bytes[(CRC_MAX_BITS + 7) / 8];
  aiot_pack_bits(bits, n, bytes);
  /* OAI returns the parity in the top L bits of 32 */
  unsigned reg = (L == AIOT_MAX_CRC_LEN ? crc16(bytes, n) : crc6(bytes, n)) >> (32 - L);
  for (int i = 0; i < L; i++)
    parity[i] = (reg >> (L - 1 - i)) & 1;
  return L;
}

int aiot_crc_length(int a_bits)
{
  return a_bits > 24 ? AIOT_MAX_CRC_LEN : 6;
}

int aiot_attach_crc(const uint8_t *bits, int a_bits, uint8_t *out)
{
  int L = aiot_crc_length(a_bits);
  if (a_bits > 0)
    memmove(out, bits, (size_t)a_bits);
  int rc = aiot_crc(out, a_bits, L, out + a_bits);
  return rc < 0 ? rc : a_bits + L;
}

int aiot_check_crc(const uint8_t *word, int b_bits, int *a_bits)
{
  /* The receiver knows only B = A + L: L = 6 if B <= 30, 16 if B >= 41. */
  int L;
  if (b_bits <= 30)
    L = 6;
  else if (b_bits >= 41)
    L = AIOT_MAX_CRC_LEN;
  else
    return AIOT_EINVAL;
  if (b_bits < L)
    return AIOT_EINVAL;
  int a = b_bits - L;
  uint8_t parity[AIOT_MAX_CRC_LEN];
  int rc = aiot_crc(word, a, L, parity);
  if (rc < 0)
    return rc;
  if (a_bits)
    *a_bits = a;
  for (int i = 0; i < L; i++)
    if (parity[i] != (word[a + i] & 1))
      return 0;
  return 1;
}

/* ---------------------------------------------------------------------------------------------
 * TBCC, rate 1/3, constraint length 7
 * ------------------------------------------------------------------------------------------- */

int aiot_tbcc_encode(const uint8_t *c, int k, uint8_t *e)
{
  if (k < 1 || k > AIOT_MAX_BLOCK_BITS)
    return AIOT_EINVAL;
  pthread_once(&tables_once, init_tables);
  /* With K < 6 the state wraps round the block more than once; repeating the block until it is
   * at least 6 bits long gives the same start state, and its first 3K outputs are the code. */
  int reps = k < TBCC_MEMORY ? (TBCC_MEMORY + k - 1) / k : 1, n = k * reps;
  uint8_t short_block[2 * TBCC_MEMORY];
  if (reps > 1)
    aiot_repeat_block(c, k, reps, short_block);
  uint8_t bytes[(AIOT_MAX_BLOCK_BITS + 7) / 8];
  aiot_pack_bits(reps > 1 ? short_block : c, n, bytes);
  uint8_t short_coded[AIOT_TBCC_RATE * 2 * TBCC_MEMORY];
  ccodelte_encode(n, 0, bytes, reps > 1 ? short_coded : e, 0);
  if (reps > 1)
    memcpy(e, short_coded, (size_t)AIOT_TBCC_RATE * k);
  return AIOT_TBCC_RATE * k;
}

/* phy_viterbi_lte_sse2 keeps its trellis for at most this many steps */
#define VITERBI_MAX_STEPS 8192
/* OAI's decoder takes 4-bit soft values; the mean magnitude is scaled to this */
#define SOFT_SCALE 3.0

int aiot_tbcc_decode(const double *soft, int n_soft, uint8_t *out)
{
  if (n_soft % 3)
    return AIOT_EINVAL;
  int K = n_soft / 3;
  if (K == 0)
    return 0;
  /* Tail-biting, as LTE's callers do it: decode an odd number of copies of the block, enough
   * that very short blocks settle, and keep the middle one. */
  int side = (24 + K - 1) / K;
  int copies = 2 * side + 1;
  int n = K * copies;
  if (n > VITERBI_MAX_STEPS)
    return AIOT_EINVAL;
  pthread_once(&tables_once, init_tables);

  double mean = 0;
  for (int i = 0; i < n_soft; i++)
    mean += fabs(soft[i]);
  mean /= n_soft;
  /* aiot_phy.h's soft values are positive for bit 0, phy_viterbi_lte_sse2()'s for bit 1: hence the minus */
  double g = mean > 0 ? -SOFT_SCALE / mean : 0;
  int8_t *y = malloc((size_t)n * 3);
  uint8_t *dec = calloc((size_t)n / 8 + 1, 1);
  if (!y || !dec) {
    free(y);
    free(dec);
    return AIOT_ENOMEM;
  }
  for (int i = 0; i < n_soft; i++) {
    long q = lround(soft[i] * g);
    y[i] = (int8_t)(q > 7 ? 7 : q < -8 ? -8 : q);
  }
  for (int c = 1; c < copies; c++)
    memcpy(y + (size_t)c * n_soft, y, (size_t)n_soft);
  phy_viterbi_lte_sse2(y, dec, (uint16_t)n);
  for (int i = 0; i < K; i++) {
    int j = side * K + i;
    out[i] = (dec[j >> 3] >> (7 - (j & 7))) & 1;
  }
  free(y);
  free(dec);
  return K;
}

int aiot_repeat_block(const uint8_t *bits, int n, int r_block, uint8_t *out)
{
  if (r_block < 1)
    return AIOT_EINVAL;
  for (int r = 0; r < r_block; r++)
    memmove(out + (size_t)r * n, bits, (size_t)n);
  return n * r_block;
}

int aiot_combine_repetitions(const double *soft, int n, int r_block, double *out)
{
  if (r_block < 1 || n % r_block)
    return AIOT_EINVAL;
  int k = n / r_block;
  for (int i = 0; i < k; i++) {
    double s = 0;
    for (int r = 0; r < r_block; r++)
      s += soft[r * k + i];
    out[i] = s;
  }
  return k;
}
