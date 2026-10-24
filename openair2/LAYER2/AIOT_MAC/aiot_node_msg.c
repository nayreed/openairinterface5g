/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_node_msg.c
 * \brief The messages between the nodes of an Ambient IoT network: see aiot_node_msg.h for the layout.
 */

#include "aiot_node_msg.h"

#include <string.h>

#define HEADER_BYTES 4

static void put_u16(uint8_t *p, uint16_t v)
{
  p[0] = v >> 8;
  p[1] = v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
  put_u16(p, v >> 16);
  put_u16(p + 2, v);
}

static void put_u64(uint8_t *p, uint64_t v)
{
  put_u32(p, v >> 32);
  put_u32(p + 4, v);
}

static uint16_t get_u16(const uint8_t *p)
{
  return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
  return (uint32_t)get_u16(p) << 16 | get_u16(p + 2);
}

static uint64_t get_u64(const uint8_t *p)
{
  return (uint64_t)get_u32(p) << 32 | get_u32(p + 4);
}

static int fits(int v, int lo, int hi)
{
  return v >= lo && v <= hi;
}

static aiot_d2r_kind_t d2r_kind(aiot_answer_kind_t kind)
{
  return kind == AIOT_ANSWER_MSG1 ? AIOT_D2R_ACCESS_RANDOM_ID : AIOT_D2R_UPPER_LAYER_DATA;
}

int aiot_node_msg_type(const uint8_t *buf, int len)
{
  if (!buf || len < HEADER_BYTES || buf[0] != AIOT_NODE_MSG_VERSION || get_u16(buf + 2) != len)
    return AIOT_EINVAL;
  return buf[1] >= AIOT_NODE_MSG_TAG_DATA && buf[1] <= AIOT_NODE_MSG_OUTCOME ? buf[1] : AIOT_EINVAL;
}

static void put_header(uint8_t *buf, aiot_node_msg_type_t type, int len)
{
  buf[0] = AIOT_NODE_MSG_VERSION;
  buf[1] = type;
  put_u16(buf + 2, len);
}

int aiot_node_msg_pack_tag_data(const aiot_tag_data_t *m, uint8_t *buf, int cap)
{
  if (!m || !buf)
    return AIOT_EINVAL;
  const aiot_reader_answer_t *a = &m->answer;
  const aiot_mac_d2r_msg_t *d = &a->message;
  if (!fits(m->after, AIOT_R2D_PAGING_CBRA, AIOT_R2D_NACK_FEEDBACK) || !fits(a->kind, AIOT_ANSWER_MSG1, AIOT_ANSWER_DATA)
      || d->kind != d2r_kind(a->kind) || !fits(a->r_sfs, 0, UINT8_MAX) || !fits(d->mdi, 0, 1)
      || !fits(a->slot, INT16_MIN, INT16_MAX) || !fits(a->entry, -1, INT16_MAX) || !fits(d->random_id, 0, UINT16_MAX)
      || !fits(d->padding_bytes, 0, AIOT_MAC_MAX_TBS) || !fits(d->sdu_len, 0, AIOT_MAC_MAX_TBS))
    return AIOT_EINVAL;
  const int len = AIOT_TAG_DATA_FIXED_BYTES + d->sdu_len;
  if (cap < len)
    return AIOT_ESPACE;
  uint64_t rssi;
  memcpy(&rssi, &a->rssi_w, sizeof(rssi));
  put_header(buf, AIOT_NODE_MSG_TAG_DATA, len);
  put_u32(buf + 4, m->seq);
  put_u64(buf + 8, (uint64_t)a->start);
  put_u64(buf + 16, rssi);
  buf[24] = m->after;
  buf[25] = a->kind;
  buf[26] = a->r_sfs;
  buf[27] = d->mdi;
  put_u16(buf + 28, (uint16_t)a->slot);
  put_u16(buf + 30, (uint16_t)a->entry);
  put_u32(buf + 32, (uint32_t)a->as_id);
  put_u16(buf + 36, d->random_id);
  buf[38] = d->padding_bytes;
  buf[39] = d->sdu_len;
  memcpy(buf + AIOT_TAG_DATA_FIXED_BYTES, d->sdu, d->sdu_len);
  return len;
}

int aiot_node_msg_parse_tag_data(const uint8_t *buf, int len, aiot_tag_data_t *m)
{
  if (!m || aiot_node_msg_type(buf, len) != AIOT_NODE_MSG_TAG_DATA || len < AIOT_TAG_DATA_FIXED_BYTES
      || len != AIOT_TAG_DATA_FIXED_BYTES + buf[39] || buf[39] > AIOT_MAC_MAX_TBS || buf[38] > AIOT_MAC_MAX_TBS
      || buf[24] > AIOT_R2D_NACK_FEEDBACK || buf[25] > AIOT_ANSWER_DATA || buf[27] > 1)
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  aiot_reader_answer_t *a = &m->answer;
  aiot_mac_d2r_msg_t *d = &a->message;
  m->seq = get_u32(buf + 4);
  a->start = (int64_t)get_u64(buf + 8);
  const uint64_t rssi = get_u64(buf + 16);
  memcpy(&a->rssi_w, &rssi, sizeof(rssi));
  m->after = (aiot_r2d_kind_t)buf[24];
  a->kind = (aiot_answer_kind_t)buf[25];
  a->r_sfs = buf[26];
  d->mdi = buf[27];
  a->slot = (int16_t)get_u16(buf + 28);
  a->entry = (int16_t)get_u16(buf + 30);
  a->as_id = (int32_t)get_u32(buf + 32);
  d->random_id = get_u16(buf + 36);
  d->kind = d2r_kind(a->kind);
  d->padding_bytes = buf[38];
  d->sdu_len = buf[39];
  memcpy(d->sdu, buf + AIOT_TAG_DATA_FIXED_BYTES, d->sdu_len);
  return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The split Reader's messages
 * ------------------------------------------------------------------------------------------- */

#define CELL_FRAMES 1024
#define CELL_SLOTS 10 /* a frame's, at 15 kHz */
#define MAX_DECODED (AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS)
#define MAX_D2R_BITS AIOT_MAX_TB_BITS /* aiot_d2r_result_t.bits */

static int packed_bytes(int n_bits)
{
  return (n_bits + 7) / 8;
}

/* n bits, one a byte, into (n + 7) / 8 bytes, the first in the top bit */
static void put_bits(uint8_t *p, const uint8_t *bits, int n)
{
  memset(p, 0, packed_bytes(n));
  for (int i = 0; i < n; i++)
    if (bits[i])
      p[i / 8] |= 0x80 >> (i % 8);
}

static void get_bits(const uint8_t *p, uint8_t *bits, int n)
{
  for (int i = 0; i < n; i++)
    bits[i] = (p[i / 8] >> (7 - i % 8)) & 1;
}

static int is_type(const uint8_t *buf, int len, aiot_node_msg_type_t type, int fixed)
{
  return aiot_node_msg_type(buf, len) == (int)type && len >= fixed;
}

int aiot_node_msg_pack_r2d(const aiot_node_r2d_t *m, uint8_t *buf, int cap)
{
  if (!m || !buf || !fits(m->frame, 0, CELL_FRAMES - 1) || !fits(m->slot, 0, CELL_SLOTS - 1) || (m->symbol != 0 && m->symbol != 7)
      || !fits(m->m_chip_symb, 0, UINT8_MAX) || !fits(m->n_rb, 0, UINT8_MAX) || !fits(m->a_bits, 1, AIOT_MAX_TB_BITS))
    return AIOT_EINVAL;
  const int len = AIOT_NODE_R2D_FIXED_BYTES + packed_bytes(m->a_bits);
  if (cap < len)
    return AIOT_ESPACE;
  put_header(buf, AIOT_NODE_MSG_R2D, len);
  put_u32(buf + 4, m->seq);
  put_u16(buf + 8, m->frame);
  buf[10] = m->slot;
  buf[11] = m->symbol;
  buf[12] = m->m_chip_symb;
  buf[13] = m->n_rb;
  put_u16(buf + 14, m->a_bits);
  put_bits(buf + AIOT_NODE_R2D_FIXED_BYTES, m->bits, m->a_bits);
  return len;
}

int aiot_node_msg_parse_r2d(const uint8_t *buf, int len, aiot_node_r2d_t *m)
{
  if (!m || !is_type(buf, len, AIOT_NODE_MSG_R2D, AIOT_NODE_R2D_FIXED_BYTES))
    return AIOT_EINVAL;
  const int a_bits = get_u16(buf + 14);
  if (!fits(a_bits, 1, AIOT_MAX_TB_BITS) || len != AIOT_NODE_R2D_FIXED_BYTES + packed_bytes(a_bits)
      || get_u16(buf + 8) >= CELL_FRAMES || buf[10] >= CELL_SLOTS || (buf[11] != 0 && buf[11] != 7))
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  m->seq = get_u32(buf + 4);
  m->frame = get_u16(buf + 8);
  m->slot = buf[10];
  m->symbol = buf[11];
  m->m_chip_symb = buf[12];
  m->n_rb = buf[13];
  m->a_bits = a_bits;
  get_bits(buf + AIOT_NODE_R2D_FIXED_BYTES, m->bits, a_bits);
  return 0;
}

int aiot_node_msg_pack_r2d_done(const aiot_node_r2d_done_t *m, uint8_t *buf, int cap)
{
  if (!m || !buf || !fits(m->result, INT16_MIN, INT16_MAX) || !fits(m->ahead, INT16_MIN, INT16_MAX))
    return AIOT_EINVAL;
  if (cap < AIOT_NODE_R2D_DONE_BYTES)
    return AIOT_ESPACE;
  put_header(buf, AIOT_NODE_MSG_R2D_DONE, AIOT_NODE_R2D_DONE_BYTES);
  put_u32(buf + 4, m->seq);
  put_u16(buf + 8, (uint16_t)m->result);
  put_u16(buf + 10, (uint16_t)m->ahead);
  return AIOT_NODE_R2D_DONE_BYTES;
}

int aiot_node_msg_parse_r2d_done(const uint8_t *buf, int len, aiot_node_r2d_done_t *m)
{
  if (!m || !is_type(buf, len, AIOT_NODE_MSG_R2D_DONE, AIOT_NODE_R2D_DONE_BYTES) || len != AIOT_NODE_R2D_DONE_BYTES)
    return AIOT_EINVAL;
  m->seq = get_u32(buf + 4);
  m->result = (int16_t)get_u16(buf + 8);
  m->ahead = (int16_t)get_u16(buf + 10);
  return 0;
}

int aiot_node_msg_pack_window(const aiot_node_window_t *m, uint8_t *buf, int cap)
{
  if (!m || !buf)
    return AIOT_EINVAL;
  const aiot_d2r_expect_t *e = &m->expect;
  const aiot_d2r_params_t *p = &e->params;
  if (!fits(m->at.frame, 0, CELL_FRAMES - 1) || !fits(m->at.slot, 0, CELL_SLOTS - 1)
      || !fits(m->at.sample, 0, AIOT_SAMPLES_PER_MS - 1) || e->te < 0 || !fits(e->n_r_sfs, 0, AIOT_MAX_SHIFTS)
      || !fits(e->a_bits, 0, UINT16_MAX) || !fits(p->i_bit, 0, UINT16_MAX) || !fits(p->t_bit_num, 0, UINT8_MAX)
      || !fits(p->t_bit_den, 0, UINT8_MAX) || !fits(p->r_sfs, 0, UINT8_MAX) || !fits(p->r_block, 0, UINT8_MAX)
      || !fits(p->r_code, 0, UINT8_MAX) || !fits(p->l_amble, 0, UINT8_MAX) || !fits(p->i_add, 0, UINT8_MAX)
      || !fits(p->modulation, AIOT_OOK, AIOT_BPSK))
    return AIOT_EINVAL;
  for (int i = 0; i < e->n_r_sfs; i++)
    if (!fits(e->r_sfs[i], 0, UINT8_MAX))
      return AIOT_EINVAL;
  const int len = AIOT_NODE_WINDOW_FIXED_BYTES + e->n_r_sfs;
  if (cap < len)
    return AIOT_ESPACE;
  put_header(buf, AIOT_NODE_MSG_WINDOW, len);
  put_u64(buf + 4, (uint64_t)e->tag);
  put_u32(buf + 12, (uint32_t)e->te);
  put_u16(buf + 16, m->at.frame);
  put_u16(buf + 18, m->at.sample);
  buf[20] = m->at.slot;
  buf[21] = e->n_r_sfs;
  put_u16(buf + 22, e->a_bits);
  put_u16(buf + 24, p->i_bit);
  const int bytes[] = {p->t_bit_num, p->t_bit_den, p->r_sfs, p->r_block, p->r_code, p->l_amble, p->i_add, p->modulation};
  for (int i = 0; i < 8; i++)
    buf[26 + i] = bytes[i];
  for (int i = 0; i < e->n_r_sfs; i++)
    buf[AIOT_NODE_WINDOW_FIXED_BYTES + i] = e->r_sfs[i];
  return len;
}

int aiot_node_msg_parse_window(const uint8_t *buf, int len, aiot_node_window_t *m)
{
  if (!m || !is_type(buf, len, AIOT_NODE_MSG_WINDOW, AIOT_NODE_WINDOW_FIXED_BYTES))
    return AIOT_EINVAL;
  const int n = buf[21];
  if (n > AIOT_MAX_SHIFTS || len != AIOT_NODE_WINDOW_FIXED_BYTES + n || get_u16(buf + 16) >= CELL_FRAMES
      || get_u16(buf + 18) >= AIOT_SAMPLES_PER_MS || buf[20] >= CELL_SLOTS || (int32_t)get_u32(buf + 12) < 0 || buf[33] > AIOT_BPSK)
    return AIOT_EINVAL;
  memset(m, 0, sizeof(*m));
  aiot_d2r_expect_t *e = &m->expect;
  aiot_d2r_params_t *p = &e->params;
  e->tag = (int64_t)get_u64(buf + 4);
  e->te = (int32_t)get_u32(buf + 12);
  m->at.frame = get_u16(buf + 16);
  m->at.sample = get_u16(buf + 18);
  m->at.slot = buf[20];
  e->n_r_sfs = n;
  e->a_bits = get_u16(buf + 22);
  p->i_bit = get_u16(buf + 24);
  p->t_bit_num = buf[26];
  p->t_bit_den = buf[27];
  p->r_sfs = buf[28];
  p->r_block = buf[29];
  p->r_code = buf[30];
  p->l_amble = buf[31];
  p->i_add = buf[32];
  p->modulation = (aiot_mod_t)buf[33];
  for (int i = 0; i < n; i++)
    e->r_sfs[i] = buf[AIOT_NODE_WINDOW_FIXED_BYTES + i];
  return 0;
}

int aiot_node_msg_pack_outcome(const aiot_node_outcome_t *m, uint8_t *buf, int cap)
{
  if (!m || !buf)
    return AIOT_EINVAL;
  const aiot_d2r_outcome_t *o = &m->outcome;
  if (!fits(o->n_decoded, 0, MAX_DECODED) || !fits(o->collided, 0, UINT16_MAX))
    return AIOT_EINVAL;
  int len = AIOT_NODE_OUTCOME_FIXED_BYTES;
  for (int i = 0; i < o->n_decoded; i++) {
    const aiot_d2r_result_t *d = &o->decoded[i];
    if (d->start < INT32_MIN || d->start > INT32_MAX || !fits(d->n_bits, 0, MAX_D2R_BITS) || !fits(d->r_sfs, 0, UINT8_MAX))
      return AIOT_EINVAL;
    len += AIOT_NODE_D2R_FIXED_BYTES + packed_bytes(d->n_bits);
  }
  if (len > UINT16_MAX)
    return AIOT_EINVAL;
  if (cap < len)
    return AIOT_ESPACE;
  put_header(buf, AIOT_NODE_MSG_OUTCOME, len);
  put_u64(buf + 4, (uint64_t)o->expect.tag);
  put_u32(buf + 12, (uint32_t)m->ahead);
  put_u16(buf + 16, o->collided);
  put_u16(buf + 18, o->n_decoded);
  uint8_t *p = buf + AIOT_NODE_OUTCOME_FIXED_BYTES;
  for (int i = 0; i < o->n_decoded; i++) {
    const aiot_d2r_result_t *d = &o->decoded[i];
    uint64_t rssi;
    memcpy(&rssi, &d->rssi_w, sizeof(rssi));
    put_u32(p, (uint32_t)(int32_t)d->start);
    put_u64(p + 4, rssi);
    put_u16(p + 12, d->n_bits);
    p[14] = d->r_sfs;
    put_bits(p + AIOT_NODE_D2R_FIXED_BYTES, d->bits, d->n_bits);
    p += AIOT_NODE_D2R_FIXED_BYTES + packed_bytes(d->n_bits);
  }
  return len;
}

int aiot_node_msg_parse_outcome(const uint8_t *buf, int len, aiot_node_outcome_t *m)
{
  if (!m || !is_type(buf, len, AIOT_NODE_MSG_OUTCOME, AIOT_NODE_OUTCOME_FIXED_BYTES))
    return AIOT_EINVAL;
  const int n = get_u16(buf + 18);
  if (n > MAX_DECODED)
    return AIOT_EINVAL;
  /* the D2Rs must fill the message exactly */
  int at = AIOT_NODE_OUTCOME_FIXED_BYTES;
  for (int i = 0; i < n; i++) {
    if (len - at < AIOT_NODE_D2R_FIXED_BYTES)
      return AIOT_EINVAL;
    const int n_bits = get_u16(buf + at + 12);
    if (n_bits > MAX_D2R_BITS)
      return AIOT_EINVAL;
    at += AIOT_NODE_D2R_FIXED_BYTES + packed_bytes(n_bits);
  }
  if (at != len)
    return AIOT_EINVAL;
  aiot_d2r_outcome_t *o = &m->outcome;
  memset(&o->expect, 0, sizeof(o->expect));
  o->expect.tag = (int64_t)get_u64(buf + 4);
  m->ahead = (int32_t)get_u32(buf + 12);
  o->collided = get_u16(buf + 16);
  o->n_decoded = n;
  const uint8_t *p = buf + AIOT_NODE_OUTCOME_FIXED_BYTES;
  for (int i = 0; i < n; i++) {
    aiot_d2r_result_t *d = &o->decoded[i];
    const uint64_t rssi = get_u64(p + 4);
    d->start = (int32_t)get_u32(p);
    memcpy(&d->rssi_w, &rssi, sizeof(rssi));
    d->n_bits = get_u16(p + 12);
    d->r_sfs = p[14];
    d->tag = o->expect.tag;
    get_bits(p + AIOT_NODE_D2R_FIXED_BYTES, d->bits, d->n_bits);
    p += AIOT_NODE_D2R_FIXED_BYTES + packed_bytes(d->n_bits);
  }
  return 0;
}
