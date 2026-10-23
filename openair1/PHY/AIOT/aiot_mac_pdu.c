/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac_pdu.c
 * \brief 3GPP TS 38.391 V19.0.0 clause 6: Rel-19 Ambient IoT MAC PDUs.
 */

#include <string.h>

#include "aiot_mac_pdu.h"

#define MAX_TBS AIOT_MAC_MAX_TBS
#define RIDR_ENTRY_MIN_BITS 17 /* Echoed Random ID (16) + AIPI (1) */
#define SCRATCH_BITS 2048 /* above any PDU body: the largest valid one is 8 * 125 bits plus checks */

/* ---------------------------------------------------------------------------------------------
 * Bit helpers
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  uint8_t *b;
  int n;
  int cap;
  int err;
} wr_t;

/* Appends the low `width` bits of v, MSB first; v outside 0..2^width-1 is an error. */
static void put(wr_t *w, int64_t v, int width)
{
  if (v < 0 || v >= ((int64_t)1 << width)) {
    w->err = AIOT_EINVAL;
    return;
  }
  if (w->n + width > w->cap) {
    w->err = AIOT_ESPACE;
    return;
  }
  for (int i = 0; i < width; i++)
    w->b[w->n++] = (v >> (width - 1 - i)) & 1;
}

static void put_bits(wr_t *w, const uint8_t *src, int n)
{
  if (w->n + n > w->cap) {
    w->err = AIOT_ESPACE;
    return;
  }
  memcpy(w->b + w->n, src, n);
  w->n += n;
}

static void put_bytes(wr_t *w, const uint8_t *src, int n)
{
  for (int i = 0; i < n; i++)
    put(w, src[i], 8);
}

typedef struct {
  const uint8_t *b;
  int n;
  int pos;
} rd_t;

static int rd_left(const rd_t *r)
{
  return r->n - r->pos;
}

/* Reads a width-bit field; -1 when fewer bits are left. */
static int rd_take(rd_t *r, int width)
{
  if (rd_left(r) < width)
    return -1;
  int v = 0;
  for (int i = 0; i < width; i++)
    v = (v << 1) | r->b[r->pos + i];
  r->pos += width;
  return v;
}

/* Points at the next n bits, or NULL when fewer are left. */
static const uint8_t *rd_take_bits(rd_t *r, int n)
{
  if (rd_left(r) < n)
    return NULL;
  const uint8_t *p = r->b + r->pos;
  r->pos += n;
  return p;
}

static int bits_clean(const uint8_t *bits, int n)
{
  if (n < 0 || (n > 0 && !bits))
    return 0;
  for (int i = 0; i < n; i++)
    if (bits[i] > 1)
      return 0;
  return 1;
}

static int bits_to_int(const uint8_t *b, int n)
{
  int v = 0;
  for (int i = 0; i < n; i++)
    v = (v << 1) | b[i];
  return v;
}

static int flag_ok(int v)
{
  return v == 0 || v == 1;
}

/* ---------------------------------------------------------------------------------------------
 * D2R Scheduling Info (6.2.1.6, Table 6.2.1.6-1)
 * ------------------------------------------------------------------------------------------- */

static const struct {
  int num, den;
  int max_r_sfs; /* largest R_SFS allowed with this bit duration */
  int scale; /* S, the Interval Bits scale factor */
} bit_dur[8] = {
    {2, 1, 128, 1},
    {1, 1, 64, 2},
    {1, 2, 32, 4},
    {1, 4, 16, 8},
    {1, 8, 8, 16},
    {1, 16, 4, 32},
    {1, 32, 2, 64},
    {1, 96, 1, 192},
};
static const int r_sfs_values[8] = {1, 2, 4, 8, 16, 32, 64, 128};
static const int i_bit_base[4] = {48, 96, 168, 240};
/* TRI 1, BitDur 3, FreqBroadcast 8 / FreqUnicast 3, Block 1, Coding 1, Interval 2, Seq 1, AddMid 1, TBS 7 */
static const int sched_width[4] = {18, 24, 24, 19};

int aiot_mac_sched_width(int variant)
{
  return (variant >= 0 && variant < 4) ? sched_width[variant] : AIOT_EINVAL;
}

/* Index of the bit duration num/den (any equal fraction), or -1. */
static int bit_dur_index(int num, int den)
{
  if (num <= 0 || den <= 0)
    return -1;
  for (int i = 0; i < 8; i++)
    if ((int64_t)num * bit_dur[i].den == (int64_t)bit_dur[i].num * den)
      return i;
  return -1;
}

static int r_sfs_index(int r)
{
  for (int i = 0; i < 8; i++)
    if (r_sfs_values[i] == r)
      return i;
  return -1;
}

/* The I_bit of the scheduling info; 0 stands for codepoint 0. */
static int resolved_i_bit(const aiot_sched_info_t *s, int row)
{
  return s->i_bit == 0 ? bit_dur[row].scale * i_bit_base[0] : s->i_bit;
}

int aiot_mac_sched_validate(const aiot_sched_info_t *s)
{
  if (!s || s->variant < 0 || s->variant > 3)
    return AIOT_EINVAL;
  int row = bit_dur_index(s->t_bit_num, s->t_bit_den);
  if (row < 0)
    return AIOT_EINVAL;
  if (s->variant == AIOT_SCHED_PAGING_CBRA) {
    if (s->x != 1 && s->x != 2)
      return AIOT_EINVAL;
    if (s->d2r_tbs != 0)
      return AIOT_EINVAL;
  } else {
    if (s->x != 0)
      return AIOT_EINVAL;
    if (s->d2r_tbs < 1 || s->d2r_tbs > MAX_TBS)
      return AIOT_EINVAL;
  }
  if (s->n_sfs < 1 || s->n_sfs > 8)
    return AIOT_EINVAL;
  for (int i = 0; i < s->n_sfs; i++) {
    int r = s->r_sfs[i];
    if (i > 0 && r <= s->r_sfs[i - 1]) /* unique and ascending */
      return AIOT_EINVAL;
    if (r_sfs_index(r) < 0)
      return AIOT_EINVAL;
    /* 38.391 gates the values by bit duration only for the Broadcast field */
    if (r > bit_dur[row].max_r_sfs && s->variant != AIOT_SCHED_R2D_DATA)
      return AIOT_EINVAL;
  }
  if ((s->variant == AIOT_SCHED_PAGING_CFA || s->variant == AIOT_SCHED_R2D_DATA) && s->n_sfs != 1)
    return AIOT_EINVAL;
  if (s->r_block != 1 && s->r_block != 2)
    return AIOT_EINVAL;
  int ib = resolved_i_bit(s, row), ok = 0;
  for (int i = 0; i < 4; i++)
    ok |= ib == bit_dur[row].scale * i_bit_base[i];
  if (!ok)
    return AIOT_EINVAL;
  if (s->l_amble != 7 && s->l_amble != 31)
    return AIOT_EINVAL;
  if (!flag_ok(s->r_code) || !flag_ok(s->i_add))
    return AIOT_EINVAL;
  return 0;
}

int aiot_mac_sched_scale(const aiot_sched_info_t *s)
{
  int row = s ? bit_dur_index(s->t_bit_num, s->t_bit_den) : -1;
  return row < 0 ? AIOT_EINVAL : bit_dur[row].scale;
}

int aiot_mac_sched_i_bit_codepoint(const aiot_sched_info_t *s)
{
  int row = s ? bit_dur_index(s->t_bit_num, s->t_bit_den) : -1;
  if (row < 0)
    return AIOT_EINVAL;
  for (int i = 0; i < 4; i++)
    if (resolved_i_bit(s, row) == bit_dur[row].scale * i_bit_base[i])
      return i;
  return AIOT_EINVAL;
}

/* Child fields in the row order of Table 6.2.1.6-1. */
static void sched_put(wr_t *w, const aiot_sched_info_t *s)
{
  int row = bit_dur_index(s->t_bit_num, s->t_bit_den);
  if (s->variant == AIOT_SCHED_PAGING_CBRA)
    put(w, s->x - 1, 1); /* Time Resource Indication */
  put(w, row, 3); /* Bit Duration */
  if (s->variant == AIOT_SCHED_R2D_DATA) {
    put(w, r_sfs_index(s->r_sfs[0]), 3); /* Frequency Resource Indication (Unicast) */
  } else {
    int map = 0; /* Frequency Resource Indication (Broadcast): bit i set for R_SFS_VALUES[i] */
    for (int i = 0; i < s->n_sfs; i++)
      map |= 0x80 >> r_sfs_index(s->r_sfs[i]);
    put(w, map, 8);
  }
  put(w, s->r_block - 1, 1); /* Block Repetition number */
  put(w, s->r_code ? 0 : 1, 1); /* Channel Coding Indicator {FEC, no FEC} */
  put(w, aiot_mac_sched_i_bit_codepoint(s), 2); /* Interval Bits */
  put(w, s->l_amble == 31, 1); /* Sequence Length Indicator {short, long} */
  put(w, s->i_add ? 1 : 0, 1); /* Additional Midamble Indicator {absent, present} */
  if (s->variant != AIOT_SCHED_PAGING_CBRA)
    put(w, s->d2r_tbs - 1, 7); /* D2R TBS: first codepoint = 1 byte */
}

int aiot_mac_sched_pack(const aiot_sched_info_t *s, uint8_t *bits, int cap)
{
  if (aiot_mac_sched_validate(s) < 0)
    return AIOT_EINVAL;
  wr_t w = {bits, 0, cap, 0};
  sched_put(&w, s);
  return w.err ? w.err : w.n;
}

int aiot_mac_sched_parse(const uint8_t *bits, int n, aiot_sched_variant_t variant, aiot_sched_info_t *s)
{
  if (variant < 0 || variant > 3 || !s)
    return AIOT_EINVAL;
  if (!bits_clean(bits, n) || n != sched_width[variant])
    return AIOT_EINVAL;
  rd_t r = {bits, n, 0};
  memset(s, 0, sizeof(*s));
  s->variant = variant;
  if (variant == AIOT_SCHED_PAGING_CBRA)
    s->x = rd_take(&r, 1) + 1;
  int row = rd_take(&r, 3);
  s->t_bit_num = bit_dur[row].num;
  s->t_bit_den = bit_dur[row].den;
  if (variant == AIOT_SCHED_R2D_DATA) {
    s->n_sfs = 1;
    s->r_sfs[0] = r_sfs_values[rd_take(&r, 3)];
  } else {
    int map = rd_take(&r, 8);
    for (int i = 0; i < 8; i++)
      if (map & (0x80 >> i))
        s->r_sfs[s->n_sfs++] = r_sfs_values[i];
  }
  s->r_block = rd_take(&r, 1) + 1;
  s->r_code = rd_take(&r, 1) == 0;
  s->i_bit = bit_dur[row].scale * i_bit_base[rd_take(&r, 2)];
  s->l_amble = rd_take(&r, 1) ? 31 : 7;
  s->i_add = rd_take(&r, 1);
  if (variant != AIOT_SCHED_PAGING_CBRA) {
    s->d2r_tbs = rd_take(&r, 7) + 1;
    if (s->d2r_tbs > MAX_TBS) /* codepoints 125..127 are not used in this release */
      return AIOT_EINVAL;
  }
  return aiot_mac_sched_validate(s) < 0 ? AIOT_EINVAL : 0;
}

int aiot_mac_to_d2r_params(const aiot_sched_info_t *s, int r_sfs, aiot_mod_t modulation, aiot_d2r_params_t *out)
{
  if (aiot_mac_sched_validate(s) < 0 || !out)
    return AIOT_EINVAL;
  int found = 0;
  for (int i = 0; i < s->n_sfs; i++)
    found |= s->r_sfs[i] == r_sfs;
  if (!found)
    return AIOT_EINVAL;
  /* D2RParams.__post_init__: a Unicast R_SFS may be outside the set allowed for the bit duration */
  int row = bit_dur_index(s->t_bit_num, s->t_bit_den);
  if (r_sfs > bit_dur[row].max_r_sfs)
    return AIOT_EINVAL;
  if (modulation != AIOT_OOK && modulation != AIOT_BPSK)
    return AIOT_EINVAL;
  out->t_bit_num = bit_dur[row].num;
  out->t_bit_den = bit_dur[row].den;
  out->r_sfs = r_sfs;
  out->r_block = s->r_block;
  out->r_code = s->r_code;
  out->l_amble = s->l_amble;
  out->i_bit = resolved_i_bit(s, row);
  out->i_add = s->i_add;
  out->modulation = modulation;
  return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Field value helpers
 * ------------------------------------------------------------------------------------------- */

int aiot_mac_r2d_type_valid(int type3)
{
  return type3 >= AIOT_MAC_R2D_TYPE_PAGING && type3 <= AIOT_MAC_R2D_TYPE_NACK_FEEDBACK;
}

int aiot_mac_num_access_occasions(int code)
{
  return (code >= 0 && code <= 15) ? 1 << code : AIOT_EINVAL;
}

int aiot_mac_k_value(int k_bit)
{
  return flag_ok(k_bit) ? (k_bit ? 4 : 1) : AIOT_EINVAL;
}

int aiot_mac_freq_index_to_r_sfs(int freq_index)
{
  return (freq_index >= 0 && freq_index < 8) ? r_sfs_values[freq_index] : AIOT_EINVAL;
}

/* ---------------------------------------------------------------------------------------------
 * R2D messages (6.2.1)
 * ------------------------------------------------------------------------------------------- */

static int paging_id_ok(const aiot_mac_r2d_msg_t *m)
{
  if (m->paging_id_len < 0 || m->paging_id_len > AIOT_MAC_MAX_PAGING_ID_BITS)
    return 0;
  return bits_clean(m->paging_id, m->paging_id_len);
}

/* Validates the content of msg and writes its body with the TBS field set to tbs, without Fill
 * Bits (_check + _body). Returns the bit count or AIOT_EINVAL; out holds SCRATCH_BITS. */
static int r2d_body(const aiot_mac_r2d_msg_t *m, int tbs, uint8_t *out)
{
  wr_t w = {out, 0, SCRATCH_BITS, 0};
  switch (m->kind) {
    case AIOT_R2D_PAGING_CBRA: /* Figure 6.2.1.1-1 */
      if (m->sched.variant != AIOT_SCHED_PAGING_CBRA || !flag_ok(m->k_bit))
        return AIOT_EINVAL;
      if (m->has_paging_id && !paging_id_ok(m))
        return AIOT_EINVAL;
      put(&w, AIOT_MAC_R2D_TYPE_PAGING, 3);
      put(&w, tbs, 7);
      put(&w, 1, 1); /* AT = 1 */
      put(&w, m->transaction_id, 6);
      if (!m->has_paging_id) {
        put(&w, 0, 1); /* PIPI */
      } else {
        put(&w, 1, 1);
        put(&w, m->paging_id_len, 8);
        put_bits(&w, m->paging_id, m->paging_id_len);
      }
      put(&w, m->num_access_occasions_code, 4);
      if (aiot_mac_sched_validate(&m->sched) < 0)
        return AIOT_EINVAL;
      sched_put(&w, &m->sched);
      put(&w, m->k_bit, 1);
      break;
    case AIOT_R2D_PAGING_CFA: /* Figure 6.2.1.1-2 */
      if (!m->has_paging_id || !paging_id_ok(m) || m->sched.variant != AIOT_SCHED_PAGING_CFA)
        return AIOT_EINVAL;
      put(&w, AIOT_MAC_R2D_TYPE_PAGING, 3);
      put(&w, tbs, 7);
      put(&w, 0, 1); /* AT = 0 */
      put(&w, m->paging_id_len, 8);
      put_bits(&w, m->paging_id, m->paging_id_len);
      if (aiot_mac_sched_validate(&m->sched) < 0)
        return AIOT_EINVAL;
      sched_put(&w, &m->sched);
      break;
    case AIOT_R2D_RANDOM_ID_RESPONSE: { /* Figure 6.2.1.3-1 */
      if (m->sched.variant != AIOT_SCHED_RANDOM_ID_RESPONSE)
        return AIOT_EINVAL;
      if (m->n_entries < 1 || m->n_entries > AIOT_MAC_MAX_ID_ENTRIES)
        return AIOT_EINVAL;
      int fipi = m->entries[0].freq_index != -1;
      for (int i = 0; i < m->n_entries; i++) {
        const aiot_mac_rid_entry_t *e = &m->entries[i];
        if (e->echoed_random_id < 0 || e->echoed_random_id > 0xFFFF)
          return AIOT_EINVAL;
        if (e->assigned_as_id < -1 || e->assigned_as_id > 0xFFFF)
          return AIOT_EINVAL;
        if (e->freq_index < -1 || e->freq_index > 7)
          return AIOT_EINVAL;
        if ((e->freq_index != -1) != fipi) /* FIPI applies to all entries */
          return AIOT_EINVAL;
      }
      put(&w, AIOT_MAC_R2D_TYPE_RANDOM_ID_RESPONSE, 3);
      put(&w, tbs, 7);
      if (aiot_mac_sched_validate(&m->sched) < 0)
        return AIOT_EINVAL;
      sched_put(&w, &m->sched);
      put(&w, fipi, 1);
      for (int i = 0; i < m->n_entries; i++) { /* entries are packed back to back, not byte aligned */
        const aiot_mac_rid_entry_t *e = &m->entries[i];
        if (fipi)
          put(&w, e->freq_index, 3);
        put(&w, e->echoed_random_id, 16);
        if (e->assigned_as_id == -1) {
          put(&w, 0, 1);
        } else {
          put(&w, 1, 1);
          put(&w, e->assigned_as_id, 16);
        }
      }
      break;
    }
    case AIOT_R2D_UPPER_LAYER_DATA: /* Figures 6.2.1.4-1 and -2 */
      if (m->sched.variant != AIOT_SCHED_R2D_DATA)
        return AIOT_EINVAL;
      if (m->has_sdu ? (m->sdu_len < 0 || m->sdu_len > MAX_TBS) : (m->received_data_size < 0 || m->received_data_size > 127))
        return AIOT_EINVAL;
      put(&w, AIOT_MAC_R2D_TYPE_UPPER_LAYER_DATA, 3);
      put(&w, tbs, 7);
      put(&w, m->as_id, 16);
      if (aiot_mac_sched_validate(&m->sched) < 0)
        return AIOT_EINVAL;
      sched_put(&w, &m->sched);
      if (m->has_sdu) {
        put(&w, 1, 1); /* CI = 1, R (2) */
        put(&w, 0, 2);
        put_bytes(&w, m->sdu, m->sdu_len);
      } else {
        put(&w, 0, 1); /* CI = 0, R (3) */
        put(&w, 0, 3);
        put(&w, m->received_data_size, 7);
      }
      break;
    case AIOT_R2D_NACK_FEEDBACK: /* Figure 6.2.1.5-1 */
      if (m->n_as_ids < 1 || m->n_as_ids > AIOT_MAC_MAX_NACK_IDS)
        return AIOT_EINVAL;
      put(&w, AIOT_MAC_R2D_TYPE_NACK_FEEDBACK, 3);
      put(&w, tbs, 7);
      put(&w, 0, 6); /* R */
      for (int i = 0; i < m->n_as_ids; i++)
        put(&w, m->as_ids[i], 16);
      break;
    default:
      return AIOT_EINVAL;
  }
  return w.err ? AIOT_EINVAL : w.n;
}

/* The natural TBS of a message (ceil of the body), or AIOT_EINVAL (invalid, or more than 125 bytes). */
static int r2d_natural(const aiot_mac_r2d_msg_t *m)
{
  uint8_t scratch[SCRATCH_BITS];
  int nb = r2d_body(m, 0, scratch);
  if (nb < 0)
    return AIOT_EINVAL;
  int nat = (nb + 7) / 8;
  return nat > MAX_TBS ? AIOT_EINVAL : nat;
}

int aiot_mac_r2d_natural_bytes(const aiot_mac_r2d_msg_t *m)
{
  if (!m)
    return AIOT_EINVAL;
  if (m->kind == AIOT_R2D_ACCESS_TRIGGER)
    return 0;
  return r2d_natural(m);
}

int aiot_mac_r2d_body_bits(const aiot_mac_r2d_msg_t *m)
{
  if (!m)
    return AIOT_EINVAL;
  if (m->kind == AIOT_R2D_ACCESS_TRIGGER)
    return 3;
  uint8_t scratch[SCRATCH_BITS];
  return r2d_body(m, 0, scratch);
}

int aiot_mac_pack_r2d(const aiot_mac_r2d_msg_t *m, uint8_t *bits, int cap)
{
  if (!m || (cap > 0 && !bits))
    return AIOT_EINVAL;
  if (m->kind == AIOT_R2D_ACCESS_TRIGGER) { /* only the 3-bit type (6.1.1) */
    if (cap < 3)
      return AIOT_ESPACE;
    bits[0] = 0;
    bits[1] = 1;
    bits[2] = 0;
    return 3;
  }
  int nat = r2d_natural(m);
  if (nat < 0)
    return AIOT_EINVAL;
  int tbs = m->r2d_tbs == 0 ? nat : m->r2d_tbs;
  if (tbs < 1 || tbs > MAX_TBS || tbs < nat)
    return AIOT_EINVAL;
  /* the paging messages may carry extra zero Fill Bits ("future extensions"), the others may not */
  int fill_larger = m->kind == AIOT_R2D_PAGING_CBRA || m->kind == AIOT_R2D_PAGING_CFA;
  if (tbs > nat && !fill_larger)
    return AIOT_EINVAL;
  if (cap < 8 * tbs)
    return AIOT_ESPACE;
  uint8_t scratch[SCRATCH_BITS];
  int nb = r2d_body(m, tbs, scratch);
  if (nb < 0)
    return AIOT_EINVAL;
  memcpy(bits, scratch, nb);
  memset(bits + nb, 0, 8 * tbs - nb);
  return 8 * tbs;
}

int aiot_mac_parse_r2d(const uint8_t *bits, int n, aiot_mac_r2d_msg_t *m)
{
  if (!m)
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  if (!bits_clean(bits, n) || n < 3)
    return AIOT_EINVAL;
  int mtype = bits_to_int(bits, 3);
  if (!aiot_mac_r2d_type_valid(mtype)) /* 000, 110, 111 are reserved */
    return AIOT_EINVAL;
  if (mtype == AIOT_MAC_R2D_TYPE_ACCESS_TRIGGER) {
    if (n != 3)
      return AIOT_EINVAL;
    m->kind = AIOT_R2D_ACCESS_TRIGGER;
    return 0;
  }
  if (n % 8)
    return AIOT_EINVAL;
  rd_t r = {bits, n, 3};
  int tbs = rd_take(&r, 7);
  if (tbs < 1 || tbs > MAX_TBS || n != 8 * tbs)
    return AIOT_EINVAL;
  m->r2d_tbs = tbs;

  if (mtype == AIOT_MAC_R2D_TYPE_PAGING) {
    int at = rd_take(&r, 1);
    if (at < 0)
      return AIOT_EINVAL;
    const uint8_t *sb;
    if (at == 1) {
      m->kind = AIOT_R2D_PAGING_CBRA;
      m->transaction_id = rd_take(&r, 6);
      int pipi = rd_take(&r, 1);
      if (m->transaction_id < 0 || pipi < 0)
        return AIOT_EINVAL;
      if (pipi) {
        int len = rd_take(&r, 8);
        const uint8_t *p = len < 0 ? NULL : rd_take_bits(&r, len);
        if (!p)
          return AIOT_EINVAL;
        m->has_paging_id = 1;
        m->paging_id_len = len;
        memcpy(m->paging_id, p, len);
      }
      m->num_access_occasions_code = rd_take(&r, 4);
      if (m->num_access_occasions_code < 0)
        return AIOT_EINVAL;
      sb = rd_take_bits(&r, sched_width[AIOT_SCHED_PAGING_CBRA]);
      if (!sb || aiot_mac_sched_parse(sb, sched_width[AIOT_SCHED_PAGING_CBRA], AIOT_SCHED_PAGING_CBRA, &m->sched) < 0)
        return AIOT_EINVAL;
      m->k_bit = rd_take(&r, 1);
      return m->k_bit < 0 ? AIOT_EINVAL : 0;
    }
    m->kind = AIOT_R2D_PAGING_CFA;
    int len = rd_take(&r, 8);
    const uint8_t *p = len < 0 ? NULL : rd_take_bits(&r, len);
    if (!p)
      return AIOT_EINVAL;
    m->has_paging_id = 1;
    m->paging_id_len = len;
    memcpy(m->paging_id, p, len);
    sb = rd_take_bits(&r, sched_width[AIOT_SCHED_PAGING_CFA]);
    if (!sb || aiot_mac_sched_parse(sb, sched_width[AIOT_SCHED_PAGING_CFA], AIOT_SCHED_PAGING_CFA, &m->sched) < 0)
      return AIOT_EINVAL;
    return 0;
  }

  if (mtype == AIOT_MAC_R2D_TYPE_RANDOM_ID_RESPONSE) {
    m->kind = AIOT_R2D_RANDOM_ID_RESPONSE;
    const uint8_t *sb = rd_take_bits(&r, sched_width[AIOT_SCHED_RANDOM_ID_RESPONSE]);
    if (!sb || aiot_mac_sched_parse(sb, sched_width[AIOT_SCHED_RANDOM_ID_RESPONSE], AIOT_SCHED_RANDOM_ID_RESPONSE, &m->sched) < 0)
      return AIOT_EINVAL;
    int fipi = rd_take(&r, 1);
    if (fipi < 0)
      return AIOT_EINVAL;
    while (rd_left(&r) >= RIDR_ENTRY_MIN_BITS + 3 * fipi) {
      if (m->n_entries == AIOT_MAC_MAX_ID_ENTRIES) /* a ninth entry */
        return AIOT_EINVAL;
      aiot_mac_rid_entry_t *e = &m->entries[m->n_entries++];
      e->freq_index = fipi ? rd_take(&r, 3) : -1;
      e->echoed_random_id = rd_take(&r, 16);
      int aipi = rd_take(&r, 1);
      e->assigned_as_id = aipi ? rd_take(&r, 16) : -1;
      if (e->echoed_random_id < 0 || aipi < 0 || (aipi && e->assigned_as_id < 0)) /* truncated entry */
        return AIOT_EINVAL;
    }
    if (rd_left(&r) >= 8 || m->n_entries < 1) /* Fill Bits are 0..7 bits */
      return AIOT_EINVAL;
    return 0;
  }

  if (mtype == AIOT_MAC_R2D_TYPE_UPPER_LAYER_DATA) {
    m->kind = AIOT_R2D_UPPER_LAYER_DATA;
    m->as_id = rd_take(&r, 16);
    const uint8_t *sb = m->as_id < 0 ? NULL : rd_take_bits(&r, sched_width[AIOT_SCHED_R2D_DATA]);
    if (!sb || aiot_mac_sched_parse(sb, sched_width[AIOT_SCHED_R2D_DATA], AIOT_SCHED_R2D_DATA, &m->sched) < 0)
      return AIOT_EINVAL;
    int ci = rd_take(&r, 1);
    if (ci < 0)
      return AIOT_EINVAL;
    if (ci) {
      if (rd_take(&r, 2) < 0)
        return AIOT_EINVAL;
      m->has_sdu = 1;
      m->sdu_len = rd_left(&r) / 8;
      for (int i = 0; i < m->sdu_len; i++)
        m->sdu[i] = (uint8_t)rd_take(&r, 8);
      return 0;
    }
    if (rd_take(&r, 3) < 0)
      return AIOT_EINVAL;
    m->received_data_size = rd_take(&r, 7);
    if (m->received_data_size < 0 || rd_left(&r)) /* CI = 0 is 7 bytes */
      return AIOT_EINVAL;
    return 0;
  }

  /* NACK Feedback */
  m->kind = AIOT_R2D_NACK_FEEDBACK;
  if (rd_take(&r, 6) < 0)
    return AIOT_EINVAL;
  while (rd_left(&r)) {
    int id = rd_take(&r, 16);
    if (id < 0 || m->n_as_ids == AIOT_MAC_MAX_NACK_IDS)
      return AIOT_EINVAL;
    m->as_ids[m->n_as_ids++] = id;
  }
  return m->n_as_ids < 1 ? AIOT_EINVAL : 0;
}

/* ---------------------------------------------------------------------------------------------
 * D2R messages (6.2.2)
 * ------------------------------------------------------------------------------------------- */

int aiot_mac_pack_d2r(const aiot_mac_d2r_msg_t *m, uint8_t *bits, int cap)
{
  if (!m || (cap > 0 && !bits))
    return AIOT_EINVAL;
  wr_t w = {bits, 0, cap, 0};
  if (m->kind == AIOT_D2R_ACCESS_RANDOM_ID) { /* Figure 6.2.2.1-1: just the 16-bit Random ID */
    put(&w, m->random_id, 16);
    return w.err ? w.err : w.n;
  }
  if (m->kind != AIOT_D2R_UPPER_LAYER_DATA) /* Figure 6.2.2.2-1 */
    return AIOT_EINVAL;
  if (!flag_ok(m->mdi) || m->sdu_len < 0 || m->sdu_len > MAX_TBS || m->padding_bytes < 0 || m->padding_bytes > MAX_TBS)
    return AIOT_EINVAL;
  if (2 + m->sdu_len + m->padding_bytes > MAX_TBS)
    return AIOT_EINVAL;
  if (cap < 8 * (2 + m->sdu_len + m->padding_bytes))
    return AIOT_ESPACE;
  put(&w, 0, 2); /* type 00 */
  put(&w, 0, 6); /* R */
  put(&w, m->mdi, 1);
  put(&w, m->sdu_len, 7);
  put_bytes(&w, m->sdu, m->sdu_len);
  for (int i = 0; i < 8 * m->padding_bytes; i++)
    put(&w, 0, 1); /* MAC Padding */
  return w.err ? w.err : w.n;
}

int aiot_mac_parse_d2r(const uint8_t *bits, int n, aiot_d2r_kind_t kind, int d2r_tbs, aiot_mac_d2r_msg_t *m)
{
  if (!m)
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  if (!bits_clean(bits, n))
    return AIOT_EINVAL;
  if (kind == AIOT_D2R_ACCESS_RANDOM_ID) {
    if (n != 16)
      return AIOT_EINVAL;
    m->kind = kind;
    m->random_id = bits_to_int(bits, 16);
    return 0;
  }
  if (kind != AIOT_D2R_UPPER_LAYER_DATA)
    return AIOT_EINVAL;
  if (n % 8 || n < 16 || n > 8 * MAX_TBS)
    return AIOT_EINVAL;
  if (d2r_tbs != 0 && n != 8 * d2r_tbs)
    return AIOT_EINVAL;
  rd_t r = {bits, n, 0};
  if (rd_take(&r, 2) != 0) /* 01, 10, 11 are reserved */
    return AIOT_EINVAL;
  rd_take(&r, 6);
  m->kind = kind;
  m->mdi = rd_take(&r, 1);
  int len = rd_take(&r, 7);
  if (rd_left(&r) < 8 * len)
    return AIOT_EINVAL;
  m->sdu_len = len;
  for (int i = 0; i < len; i++)
    m->sdu[i] = (uint8_t)rd_take(&r, 8);
  m->padding_bytes = rd_left(&r) / 8;
  return 0;
}

int aiot_mac_d2r_for_tbs(const uint8_t *sdu, int sdu_len, int tbs, int mdi, aiot_mac_d2r_msg_t *m)
{
  if (!m || sdu_len < 0 || (sdu_len > 0 && !sdu) || tbs < 1 || tbs > MAX_TBS)
    return AIOT_EINVAL;
  int pad = tbs - 2 - sdu_len;
  if (pad < 0 || !flag_ok(mdi))
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  m->kind = AIOT_D2R_UPPER_LAYER_DATA;
  m->mdi = mdi;
  m->sdu_len = sdu_len;
  if (sdu_len)
    memcpy(m->sdu, sdu, sdu_len);
  m->padding_bytes = pad;
  return 0;
}
