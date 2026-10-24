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
  return buf[1] == AIOT_NODE_MSG_TAG_DATA ? AIOT_NODE_MSG_TAG_DATA : AIOT_EINVAL;
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
  buf[0] = AIOT_NODE_MSG_VERSION;
  buf[1] = AIOT_NODE_MSG_TAG_DATA;
  put_u16(buf + 2, len);
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
