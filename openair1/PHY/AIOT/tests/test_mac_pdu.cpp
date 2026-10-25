/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// aiot_mac_pdu.c: the field layouts of 38.391 clause 6 on hand-built PDUs, and pack and parse of
// random valid messages of every type.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "aiot_mac_pdu.h"

namespace {

using Bits = std::vector<uint8_t>;
using Bytes = std::vector<uint8_t>;

// Appends value as width bits, most significant first.
void Put(Bits &b, int value, int width)
{
  for (int i = width - 1; i >= 0; i--)
    b.push_back((value >> i) & 1);
}

Bits Pack(const aiot_sched_info_t &s)
{
  Bits b(32);
  int n = aiot_mac_sched_pack(&s, b.data(), (int)b.size());
  EXPECT_GT(n, 0);
  b.resize(std::max(n, 0));
  return b;
}

Bits Pack(const aiot_mac_r2d_msg_t &m)
{
  Bits b(AIOT_MAC_MAX_PDU_BITS);
  int n = aiot_mac_pack_r2d(&m, b.data(), (int)b.size());
  EXPECT_GT(n, 0);
  b.resize(std::max(n, 0));
  return b;
}

aiot_sched_info_t Sched(aiot_sched_variant_t v)
{
  aiot_sched_info_t s;
  memset(&s, 0, sizeof(s));
  s.variant = v;
  s.t_bit_num = 1;
  s.t_bit_den = 8;
  s.n_sfs = 1;
  s.r_sfs[0] = 4;
  s.x = v == AIOT_SCHED_PAGING_CBRA ? 2 : 0;
  s.r_block = 2;
  s.r_code = 1;
  s.i_bit = 16 * 168;
  s.l_amble = 31;
  s.i_add = 1;
  s.d2r_tbs = v == AIOT_SCHED_PAGING_CBRA ? 0 : 20;
  return s;
}

// A random valid Scheduling Info of variant v.
aiot_sched_info_t RandomSched(std::mt19937 &gen, aiot_sched_variant_t v)
{
  const int dur[8][3] = {{2, 1, 128}, {1, 1, 64}, {1, 2, 32}, {1, 4, 16}, {1, 8, 8}, {1, 16, 4}, {1, 32, 2}, {1, 96, 1}};
  const int scale[8] = {1, 2, 4, 8, 16, 32, 64, 192}, base[4] = {48, 96, 168, 240};
  aiot_sched_info_t s;
  memset(&s, 0, sizeof(s));
  s.variant = v;
  const int row = gen() % 8;
  s.t_bit_num = dur[row][0];
  s.t_bit_den = dur[row][1];
  if (v == AIOT_SCHED_R2D_DATA || v == AIOT_SCHED_PAGING_CFA) {
    s.n_sfs = 1;
    const int max = dur[row][2]; // Table 6.2.1.6-1 (CR 0009): the Unicast value is limited too
    for (s.r_sfs[0] = 1; s.r_sfs[0] < max && gen() % 2;)
      s.r_sfs[0] *= 2;
  } else {
    for (int r = 1; r <= dur[row][2]; r *= 2)
      if (gen() % 2)
        s.r_sfs[s.n_sfs++] = r;
    if (s.n_sfs == 0)
      s.r_sfs[s.n_sfs++] = 1;
  }
  s.x = v == AIOT_SCHED_PAGING_CBRA ? 1 + gen() % 2 : 0;
  s.r_block = 1 + gen() % 2;
  s.r_code = gen() % 2;
  s.i_bit = scale[row] * base[gen() % 4];
  s.l_amble = gen() % 2 ? 31 : 7;
  s.i_add = gen() % 2;
  s.d2r_tbs = v == AIOT_SCHED_PAGING_CBRA ? 0 : 1 + gen() % AIOT_MAC_MAX_TBS;
  EXPECT_EQ(aiot_mac_sched_validate(&s), 0);
  return s;
}

// The Security Parameter of the hand-built Paging PDUs: bit i is 1 when i is a multiple of 3.
void SetSecurity(aiot_mac_r2d_msg_t &m)
{
  m.has_security_param = 1;
  for (int i = 0; i < AIOT_MAC_SECURITY_PARAM_BITS; i++)
    m.security_param[i] = i % 3 == 0;
}

void PutSecurity(Bits &b)
{
  for (int i = 0; i < AIOT_MAC_SECURITY_PARAM_BITS; i++)
    b.push_back(i % 3 == 0);
}

// A Paging message of the given kind: no Security Parameter unless sppi, id_len Paging ID bits
// (alternating 1 0 1 0 ...) when has_id.
aiot_mac_r2d_msg_t Paging(aiot_r2d_kind_t kind, bool sppi, int id_len, bool has_id = true)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = kind;
  m.sched = Sched(kind == AIOT_R2D_PAGING_CBRA ? AIOT_SCHED_PAGING_CBRA : AIOT_SCHED_PAGING_CFA);
  if (sppi)
    SetSecurity(m);
  m.has_paging_id = has_id;
  m.paging_id_len = has_id ? id_len : 0;
  for (int i = 0; i < m.paging_id_len; i++)
    m.paging_id[i] = (i & 1) ^ 1;
  return m;
}

void PutPagingId(Bits &b, int len)
{
  for (int i = 0; i < len; i++)
    b.push_back((i & 1) ^ 1);
}

void ExpectSameSched(const aiot_sched_info_t &a, const aiot_sched_info_t &b)
{
  EXPECT_EQ(a.variant, b.variant);
  EXPECT_EQ(a.t_bit_num * b.t_bit_den, b.t_bit_num * a.t_bit_den);
  ASSERT_EQ(a.n_sfs, b.n_sfs);
  for (int i = 0; i < a.n_sfs; i++)
    EXPECT_EQ(a.r_sfs[i], b.r_sfs[i]);
  EXPECT_EQ(a.x, b.x);
  EXPECT_EQ(a.r_block, b.r_block);
  EXPECT_EQ(a.r_code, b.r_code);
  EXPECT_EQ(a.i_bit, b.i_bit);
  EXPECT_EQ(a.l_amble, b.l_amble);
  EXPECT_EQ(a.i_add, b.i_add);
  EXPECT_EQ(a.d2r_tbs, b.d2r_tbs);
}

aiot_mac_r2d_msg_t RandomR2d(std::mt19937 &gen, aiot_r2d_kind_t kind)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = kind;
  auto security = [&]() {
    m.has_security_param = gen() % 2;
    if (m.has_security_param)
      for (auto &x : m.security_param)
        x = gen() & 1;
  };
  auto paging_id = [&](int max_len) {
    m.has_paging_id = 1;
    m.paging_id_len = gen() % (max_len + 1);
    for (int i = 0; i < m.paging_id_len; i++)
      m.paging_id[i] = gen() & 1;
  };
  switch (kind) {
    case AIOT_R2D_PAGING_CBRA:
      m.sched = RandomSched(gen, AIOT_SCHED_PAGING_CBRA);
      m.transaction_id = gen() % 64;
      m.num_access_occasions_code = gen() % 16;
      m.k_bit = gen() % 2;
      security();
      if (gen() % 2)
        paging_id(AIOT_MAC_MAX_PAGING_ID_BITS);
      break;
    case AIOT_R2D_PAGING_CFA:
      m.sched = RandomSched(gen, AIOT_SCHED_PAGING_CFA);
      security();
      paging_id(AIOT_MAC_MAX_PAGING_ID_BITS);
      break;
    case AIOT_R2D_RANDOM_ID_RESPONSE: {
      m.sched = RandomSched(gen, AIOT_SCHED_RANDOM_ID_RESPONSE);
      m.n_entries = 1 + gen() % AIOT_MAC_MAX_ID_ENTRIES;
      const bool fipi = gen() % 2;
      for (int i = 0; i < m.n_entries; i++)
        m.entries[i] = {(int)(gen() % 65536), gen() % 2 ? (int)(gen() % 65536) : -1, fipi ? (int)(gen() % 8) : -1};
      break;
    }
    case AIOT_R2D_UPPER_LAYER_DATA:
      m.sched = RandomSched(gen, AIOT_SCHED_R2D_DATA);
      m.as_id = gen() % 65536;
      m.has_sdu = gen() % 2;
      if (m.has_sdu) {
        m.sdu_len = gen() % 100;
        for (int i = 0; i < m.sdu_len; i++)
          m.sdu[i] = gen() & 0xff;
      } else {
        m.received_data_size = gen() % (AIOT_MAC_MAX_RECEIVED_DATA_SIZE + 1);
      }
      break;
    case AIOT_R2D_NACK_FEEDBACK:
      m.n_as_ids = 1 + gen() % AIOT_MAC_MAX_NACK_IDS;
      for (int i = 0; i < m.n_as_ids; i++)
        m.as_ids[i] = gen() % 65536;
      break;
    default:
      break;
  }
  return m;
}

void ExpectSameR2d(const aiot_mac_r2d_msg_t &a, const aiot_mac_r2d_msg_t &b)
{
  ASSERT_EQ(a.kind, b.kind);
  if (a.kind == AIOT_R2D_ACCESS_TRIGGER)
    return;
  if (a.kind != AIOT_R2D_NACK_FEEDBACK)
    ExpectSameSched(a.sched, b.sched);
  if (a.kind == AIOT_R2D_PAGING_CBRA || a.kind == AIOT_R2D_PAGING_CFA) {
    ASSERT_EQ(a.has_security_param, b.has_security_param);
    if (a.has_security_param) {
      EXPECT_EQ(memcmp(a.security_param, b.security_param, AIOT_MAC_SECURITY_PARAM_BITS), 0);
    }
  }
  switch (a.kind) {
    case AIOT_R2D_PAGING_CBRA:
      EXPECT_EQ(a.transaction_id, b.transaction_id);
      EXPECT_EQ(a.num_access_occasions_code, b.num_access_occasions_code);
      EXPECT_EQ(a.k_bit, b.k_bit);
      // fall through: the Paging ID
    case AIOT_R2D_PAGING_CFA:
      ASSERT_EQ(a.has_paging_id, b.has_paging_id);
      ASSERT_EQ(a.paging_id_len, b.paging_id_len);
      EXPECT_EQ(memcmp(a.paging_id, b.paging_id, a.paging_id_len), 0);
      break;
    case AIOT_R2D_RANDOM_ID_RESPONSE:
      ASSERT_EQ(a.n_entries, b.n_entries);
      for (int i = 0; i < a.n_entries; i++) {
        EXPECT_EQ(a.entries[i].echoed_random_id, b.entries[i].echoed_random_id);
        EXPECT_EQ(a.entries[i].assigned_as_id, b.entries[i].assigned_as_id);
        EXPECT_EQ(a.entries[i].freq_index, b.entries[i].freq_index);
      }
      break;
    case AIOT_R2D_UPPER_LAYER_DATA:
      EXPECT_EQ(a.as_id, b.as_id);
      ASSERT_EQ(a.has_sdu, b.has_sdu);
      if (a.has_sdu) {
        ASSERT_EQ(a.sdu_len, b.sdu_len);
        EXPECT_EQ(memcmp(a.sdu, b.sdu, a.sdu_len), 0);
      } else {
        EXPECT_EQ(a.received_data_size, b.received_data_size);
      }
      break;
    case AIOT_R2D_NACK_FEEDBACK:
      ASSERT_EQ(a.n_as_ids, b.n_as_ids);
      for (int i = 0; i < a.n_as_ids; i++)
        EXPECT_EQ(a.as_ids[i], b.as_ids[i]);
      break;
    default:
      break;
  }
}

} // namespace

TEST(MacPdu, FieldValues)
{
  for (int t = 0; t < 8; t++)
    EXPECT_EQ(aiot_mac_r2d_type_valid(t), t >= 1 && t <= 5) << t;
  for (int c = 0; c < 16; c++)
    EXPECT_EQ(aiot_mac_num_access_occasions(c), 1 << c);
  EXPECT_EQ(aiot_mac_num_access_occasions(16), AIOT_EINVAL);
  EXPECT_EQ(aiot_mac_k_value(0), 1);
  EXPECT_EQ(aiot_mac_k_value(1), 4);
  EXPECT_EQ(aiot_mac_k_value(2), AIOT_EINVAL);
  for (int i = 0; i < 8; i++)
    EXPECT_EQ(aiot_mac_freq_index_to_r_sfs(i), 1 << i);
  EXPECT_EQ(aiot_mac_freq_index_to_r_sfs(8), AIOT_EINVAL);
  const int width[4] = {18, 24, 24, 19};
  for (int v = 0; v < 4; v++)
    EXPECT_EQ(aiot_mac_sched_width(v), width[v]);
  EXPECT_EQ(aiot_mac_sched_width(4), AIOT_EINVAL);
}

// Table 6.2.1.6-1, field by field
TEST(MacPdu, SchedulingInfoLayout)
{
  Bits want;
  // Paging for CBRA: TRI, Bit Duration, Frequency (Broadcast bitmap), Block, Coding, Interval,
  // Sequence, Additional Midamble
  Put(want, 1, 1);
  Put(want, 4, 3); // 1/8 tau is the fifth bit duration
  Put(want, 0x20, 8); // R_SFS 4 is the third bit of the bitmap
  Put(want, 1, 1); // R_block = 2
  Put(want, 0, 1); // FEC
  Put(want, 2, 2); // I_bit = 168 S
  Put(want, 1, 1); // long sequence
  Put(want, 1, 1); // additional midamble
  EXPECT_EQ(Pack(Sched(AIOT_SCHED_PAGING_CBRA)), want);

  // R2D data: no TRI, the Unicast frequency index, then the D2R TBS minus one
  want.clear();
  Put(want, 4, 3);
  Put(want, 2, 3);
  Put(want, 1, 1);
  Put(want, 0, 1);
  Put(want, 2, 2);
  Put(want, 1, 1);
  Put(want, 1, 1);
  Put(want, 19, 7);
  EXPECT_EQ(Pack(Sched(AIOT_SCHED_R2D_DATA)), want);

  // D2R TBS codepoints 125 to 127 are not used
  for (int cp = 0; cp < 128; cp++) {
    Bits b = Pack(Sched(AIOT_SCHED_PAGING_CFA));
    for (int j = 0; j < 7; j++)
      b[17 + j] = (cp >> (6 - j)) & 1;
    aiot_sched_info_t s;
    EXPECT_EQ(aiot_mac_sched_parse(b.data(), 24, AIOT_SCHED_PAGING_CFA, &s), cp <= 124 ? 0 : AIOT_EINVAL) << cp;
  }
}

TEST(MacPdu, SchedulingInfoRoundTrip)
{
  std::mt19937 gen(1);
  for (int t = 0; t < 2000; t++) {
    const aiot_sched_variant_t v = (aiot_sched_variant_t)(t % 4);
    aiot_sched_info_t s = RandomSched(gen, v);
    Bits b = Pack(s);
    ASSERT_EQ((int)b.size(), aiot_mac_sched_width(v));
    aiot_sched_info_t back;
    ASSERT_EQ(aiot_mac_sched_parse(b.data(), (int)b.size(), v, &back), 0);
    ExpectSameSched(s, back);
  }
  aiot_sched_info_t s = Sched(AIOT_SCHED_PAGING_CFA);
  s.n_sfs = 2;
  s.r_sfs[1] = 8; // CFA paging has exactly one shift
  EXPECT_EQ(aiot_mac_sched_validate(&s), AIOT_EINVAL);
  s = Sched(AIOT_SCHED_PAGING_CBRA);
  s.r_sfs[0] = 16; // larger than 1/8 tau allows
  EXPECT_EQ(aiot_mac_sched_validate(&s), AIOT_EINVAL);
  s = Sched(AIOT_SCHED_R2D_DATA);
  s.r_sfs[0] = 16; // the Unicast field is limited by the bit duration as the Broadcast one (CR 0009)
  EXPECT_EQ(aiot_mac_sched_validate(&s), AIOT_EINVAL);
  aiot_d2r_params_t p;
  EXPECT_EQ(aiot_mac_to_d2r_params(&s, 16, AIOT_BPSK, &p), AIOT_EINVAL);
  s.r_sfs[0] = 8;
  EXPECT_EQ(aiot_mac_sched_validate(&s), 0);
  EXPECT_EQ(aiot_mac_to_d2r_params(&s, 8, AIOT_BPSK, &p), 0);
  s.r_sfs[0] = 4;
  ASSERT_EQ(aiot_mac_to_d2r_params(&s, 4, AIOT_OOK, &p), 0);
  EXPECT_EQ(p.t_bit_num * 8, p.t_bit_den);
  EXPECT_EQ(p.r_sfs, 4);
  EXPECT_EQ(p.r_block, 2);
  EXPECT_EQ(p.r_code, 1);
  EXPECT_EQ(p.l_amble, 31);
  EXPECT_EQ(p.i_bit, 16 * 168);
  EXPECT_EQ(p.i_add, 1);
  EXPECT_EQ(p.modulation, AIOT_OOK);
  EXPECT_EQ(aiot_d2r_params_check(&p), 0);
  EXPECT_EQ(aiot_mac_to_d2r_params(&s, 2, AIOT_OOK, &p), AIOT_EINVAL); // not one of its shifts
}

// Figures 6.2.1.2-1 (Access Trigger) and 6.2.1.5-1 (NACK Feedback); 6.2.2.1-1 and 6.2.2.2-1 (D2R)
TEST(MacPdu, MessageLayouts)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_ACCESS_TRIGGER;
  EXPECT_EQ(Pack(m), (Bits{0, 1, 0}));

  m.kind = AIOT_R2D_NACK_FEEDBACK;
  m.n_as_ids = 2;
  m.as_ids[0] = 0x1234;
  m.as_ids[1] = 0xbeef;
  Bits want;
  Put(want, 5, 3);
  Put(want, 6, 7); // TBS 6 bytes
  Put(want, 0, 6);
  Put(want, 0x1234, 16);
  Put(want, 0xbeef, 16);
  EXPECT_EQ(Pack(m), want);

  aiot_mac_d2r_msg_t d;
  memset(&d, 0, sizeof(d));
  d.kind = AIOT_D2R_ACCESS_RANDOM_ID;
  d.random_id = 0xa5c3;
  Bits b(64);
  ASSERT_EQ(aiot_mac_pack_d2r(&d, b.data(), 64), 16);
  want.clear();
  Put(want, 0xa5c3, 16);
  EXPECT_TRUE(std::equal(want.begin(), want.end(), b.begin()));

  const uint8_t sdu[2] = {0x81, 0x7e};
  ASSERT_EQ(aiot_mac_d2r_for_tbs(sdu, 2, 6, 1, &d), 0);
  EXPECT_EQ(d.padding_bytes, 2);
  ASSERT_EQ(aiot_mac_pack_d2r(&d, b.data(), 64), 48);
  want.clear();
  Put(want, 0, 2); // Upper Layer Data
  Put(want, 0, 6);
  Put(want, 1, 1); // MDI
  Put(want, 2, 7); // SDU length
  Put(want, 0x81, 8);
  Put(want, 0x7e, 8);
  Put(want, 0, 16); // padding
  EXPECT_TRUE(std::equal(want.begin(), want.end(), b.begin()));
  aiot_mac_d2r_msg_t back;
  ASSERT_EQ(aiot_mac_parse_d2r(b.data(), 48, AIOT_D2R_UPPER_LAYER_DATA, 6, &back), 0);
  EXPECT_EQ(back.mdi, 1);
  EXPECT_EQ(back.sdu_len, 2);
  EXPECT_EQ(back.padding_bytes, 2);
  EXPECT_EQ(aiot_mac_parse_d2r(b.data(), 48, AIOT_D2R_UPPER_LAYER_DATA, 5, &back), AIOT_EINVAL);
  b[0] = 1; // reserved type
  EXPECT_EQ(aiot_mac_parse_d2r(b.data(), 48, AIOT_D2R_UPPER_LAYER_DATA, 0, &back), AIOT_EINVAL);
}

TEST(MacPdu, R2DRoundTrip)
{
  std::mt19937 gen(2);
  for (int t = 0; t < 3000; t++) {
    const aiot_r2d_kind_t kind = (aiot_r2d_kind_t)(t % 6);
    aiot_mac_r2d_msg_t m = RandomR2d(gen, kind);
    const int natural = aiot_mac_r2d_natural_bytes(&m);
    if (natural < 0) // a Paging ID too long for 125 bytes
      continue;
    // the paging messages may carry extra Fill Bits up to 125 bytes
    if ((kind == AIOT_R2D_PAGING_CBRA || kind == AIOT_R2D_PAGING_CFA) && gen() % 2)
      m.r2d_tbs = natural + gen() % (AIOT_MAC_MAX_TBS - natural + 1);
    Bits b = Pack(m);
    ASSERT_EQ((int)b.size(), kind == AIOT_R2D_ACCESS_TRIGGER ? 3 : 8 * (m.r2d_tbs ? m.r2d_tbs : natural));
    aiot_mac_r2d_msg_t back;
    ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0) << "kind " << kind;
    ExpectSameR2d(m, back);
  }
}

TEST(MacPdu, D2RRoundTrip)
{
  std::mt19937 gen(3);
  for (int t = 0; t < 500; t++) {
    std::vector<uint8_t> sdu(gen() % 124);
    for (auto &x : sdu)
      x = gen() & 0xff;
    const int tbs = 2 + (int)sdu.size() + gen() % (AIOT_MAC_MAX_TBS - 1 - (int)sdu.size());
    aiot_mac_d2r_msg_t m, back;
    ASSERT_EQ(aiot_mac_d2r_for_tbs(sdu.data(), (int)sdu.size(), tbs, gen() % 2, &m), 0);
    Bits b(AIOT_MAC_MAX_PDU_BITS);
    ASSERT_EQ(aiot_mac_pack_d2r(&m, b.data(), (int)b.size()), 8 * tbs);
    ASSERT_EQ(aiot_mac_parse_d2r(b.data(), 8 * tbs, AIOT_D2R_UPPER_LAYER_DATA, tbs, &back), 0);
    EXPECT_EQ(back.mdi, m.mdi);
    ASSERT_EQ(back.sdu_len, (int)sdu.size());
    EXPECT_TRUE(std::equal(sdu.begin(), sdu.end(), back.sdu));
    EXPECT_EQ(back.padding_bytes, tbs - 2 - (int)sdu.size());
  }
}

TEST(MacPdu, Boundaries)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  m.kind = AIOT_R2D_NACK_FEEDBACK;
  m.n_as_ids = 61; // the longest: 124 bytes
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), 8 * 124);
  m.n_as_ids = 62;
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);
  m.n_as_ids = 0;
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);
  m.n_as_ids = 1;
  m.r2d_tbs = 5; // only paging may carry extra Fill Bits
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);
  m.r2d_tbs = 0;
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, 31), AIOT_ESPACE);
  // a one-byte PDU can hold no message, and is not an Access Trigger either
  const uint8_t one[8] = {0, 0, 1, 0, 0, 0, 0, 1};
  EXPECT_EQ(aiot_mac_parse_r2d(one, 8, &m), AIOT_EINVAL);
  // reserved message types
  for (int t : {0, 6, 7}) {
    uint8_t b[16] = {};
    b[0] = (t >> 2) & 1, b[1] = (t >> 1) & 1, b[2] = t & 1;
    EXPECT_EQ(aiot_mac_parse_r2d(b, 16, &m), AIOT_EINVAL) << t;
  }
  // SPPI is a flag, and the Security Parameter holds bits
  aiot_mac_r2d_msg_t pg = Paging(AIOT_R2D_PAGING_CBRA, true, 8);
  EXPECT_GT(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), 0);
  pg.has_security_param = 2;
  EXPECT_EQ(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), AIOT_EINVAL);
  pg.kind = AIOT_R2D_PAGING_CFA;
  pg.sched = Sched(AIOT_SCHED_PAGING_CFA);
  EXPECT_EQ(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), AIOT_EINVAL);
  pg.has_security_param = 1;
  EXPECT_GT(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), 0);
  pg.security_param[77] = 2;
  EXPECT_EQ(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), AIOT_EINVAL);
  pg.has_security_param = 0; // not looked at without SPPI
  EXPECT_GT(aiot_mac_pack_r2d(&pg, bits, (int)sizeof(bits)), 0);
  const uint8_t trigger[3] = {0, 1, 0};
  EXPECT_EQ(aiot_mac_parse_r2d(trigger, 3, &m), 0);
  EXPECT_EQ(m.kind, AIOT_R2D_ACCESS_TRIGGER);
  EXPECT_EQ(aiot_mac_parse_r2d(trigger, 2, &m), AIOT_EINVAL);
  EXPECT_EQ(aiot_mac_parse_r2d(NULL, 0, &m), AIOT_EINVAL);
}

// Figure 6.2.1.1-1 (V19.4.0): SPPI and the Security Parameter follow the R2D TBS, the Paging ID
// Length has 10 bits. Body without Paging ID: 3 + 7 + 1 + 128 + 1 + 6 + 1 + 4 + 18 + 1 = 170 bits,
// 22 bytes with 6 Fill Bits.
TEST(MacPdu, PagingCbraLayout)
{
  aiot_mac_r2d_msg_t m = Paging(AIOT_R2D_PAGING_CBRA, true, 0, false);
  m.transaction_id = 37;
  m.num_access_occasions_code = 5;
  m.k_bit = 1;
  Bits want;
  Put(want, AIOT_MAC_R2D_TYPE_PAGING, 3);
  Put(want, 22, 7);
  Put(want, 1, 1); // SPPI
  PutSecurity(want);
  Put(want, 1, 1); // AT = CBRA
  Put(want, 37, 6);
  Put(want, 0, 1); // PIPI
  Put(want, 5, 4);
  Bits sched = Pack(m.sched);
  ASSERT_EQ((int)sched.size(), 18);
  want.insert(want.end(), sched.begin(), sched.end());
  Put(want, 1, 1); // K
  ASSERT_EQ((int)want.size(), 170);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 170);
  EXPECT_EQ(aiot_mac_r2d_natural_bytes(&m), 22);
  Put(want, 0, 6); // Fill Bits
  ASSERT_EQ((int)want.size(), 176);
  EXPECT_EQ(Pack(m), want);

  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(want.data(), (int)want.size(), &back), 0);
  EXPECT_EQ(back.kind, AIOT_R2D_PAGING_CBRA);
  EXPECT_EQ(back.r2d_tbs, 22);
  EXPECT_EQ(back.has_security_param, 1);
  EXPECT_EQ(memcmp(back.security_param, m.security_param, AIOT_MAC_SECURITY_PARAM_BITS), 0);
  EXPECT_EQ(back.transaction_id, 37);
  EXPECT_EQ(back.has_paging_id, 0);
  EXPECT_EQ(back.num_access_occasions_code, 5);
  EXPECT_EQ(back.k_bit, 1);
  ExpectSameR2d(m, back);

  // a 20-bit Paging ID, its length on 10 bits: 170 + 10 + 20 = 200 bits, 25 bytes, no Fill Bits
  m = Paging(AIOT_R2D_PAGING_CBRA, true, 20);
  m.transaction_id = 37;
  m.num_access_occasions_code = 5;
  m.k_bit = 1;
  want.clear();
  Put(want, AIOT_MAC_R2D_TYPE_PAGING, 3);
  Put(want, 25, 7);
  Put(want, 1, 1);
  PutSecurity(want);
  Put(want, 1, 1);
  Put(want, 37, 6);
  Put(want, 1, 1); // PIPI
  Put(want, 20, 10); // Paging ID Length
  PutPagingId(want, 20);
  Put(want, 5, 4);
  want.insert(want.end(), sched.begin(), sched.end());
  Put(want, 1, 1);
  ASSERT_EQ((int)want.size(), 200);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 200);
  EXPECT_EQ(Pack(m), want);
  ASSERT_EQ(aiot_mac_parse_r2d(want.data(), (int)want.size(), &back), 0);
  EXPECT_EQ(back.r2d_tbs, 25);
  EXPECT_EQ(back.paging_id_len, 20);
  ExpectSameR2d(m, back);

  // SPPI = 1 but the PDU ends inside the Security Parameter
  Bits cut;
  Put(cut, AIOT_MAC_R2D_TYPE_PAGING, 3);
  Put(cut, 4, 7);
  Put(cut, 1, 1);
  Put(cut, 0, 21);
  EXPECT_EQ(aiot_mac_parse_r2d(cut.data(), (int)cut.size(), &back), AIOT_EINVAL);
}

// Figure 6.2.1.1-2: 3 + 7 + 1 + 128 + 1 + 10 + 16 + 24 = 190 bits, 24 bytes with 2 Fill Bits
TEST(MacPdu, PagingCfaLayout)
{
  aiot_mac_r2d_msg_t m = Paging(AIOT_R2D_PAGING_CFA, true, 16);
  Bits want;
  Put(want, AIOT_MAC_R2D_TYPE_PAGING, 3);
  Put(want, 24, 7);
  Put(want, 1, 1); // SPPI
  PutSecurity(want);
  Put(want, 0, 1); // AT = CFA
  Put(want, 16, 10); // Paging ID Length
  PutPagingId(want, 16);
  Bits sched = Pack(m.sched);
  ASSERT_EQ((int)sched.size(), 24);
  want.insert(want.end(), sched.begin(), sched.end());
  ASSERT_EQ((int)want.size(), 190);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 190);
  EXPECT_EQ(aiot_mac_r2d_natural_bytes(&m), 24);
  Put(want, 0, 2); // Fill Bits
  EXPECT_EQ(Pack(m), want);

  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(want.data(), (int)want.size(), &back), 0);
  EXPECT_EQ(back.kind, AIOT_R2D_PAGING_CFA);
  EXPECT_EQ(back.r2d_tbs, 24);
  EXPECT_EQ(back.has_security_param, 1);
  EXPECT_EQ(back.paging_id_len, 16);
  ExpectSameR2d(m, back);
}

// SPPI = 0 is a valid format: no Security Parameter, 128 bits shorter
TEST(MacPdu, PagingWithoutSecurityParameter)
{
  // CBRA, no Paging ID: 3 + 7 + 1 + 1 + 6 + 1 + 4 + 18 + 1 = 42 bits, 6 bytes
  aiot_mac_r2d_msg_t m = Paging(AIOT_R2D_PAGING_CBRA, false, 0, false);
  m.transaction_id = 9;
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 42);
  Bits b = Pack(m);
  ASSERT_EQ((int)b.size(), 48);
  EXPECT_EQ(b[10], 0); // SPPI
  EXPECT_EQ(b[11], 1); // AT directly after it
  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  EXPECT_EQ(back.has_security_param, 0);
  EXPECT_EQ(back.r2d_tbs, 6);
  EXPECT_EQ(back.transaction_id, 9);
  ExpectSameR2d(m, back);

  // CFA, 12-bit Paging ID: 3 + 7 + 1 + 1 + 10 + 12 + 24 = 58 bits, 8 bytes
  m = Paging(AIOT_R2D_PAGING_CFA, false, 12);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 58);
  b = Pack(m);
  ASSERT_EQ((int)b.size(), 64);
  EXPECT_EQ(b[10], 0);
  EXPECT_EQ(b[11], 0);
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  EXPECT_EQ(back.has_security_param, 0);
  EXPECT_EQ(back.r2d_tbs, 8);
  ExpectSameR2d(m, back);
}

// The Paging ID Length has 10 bits (up to 1023); what fits in 125 bytes (1000 bits) is limited by
// the other fields: CFA 46 + L (SPPI 0) or 174 + L (SPPI 1), CBRA 170 + 10 + L (SPPI 1)
TEST(MacPdu, PagingIdLengthTenBits)
{
  aiot_mac_r2d_msg_t m = Paging(AIOT_R2D_PAGING_CFA, false, 300);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 346);
  Bits b = Pack(m);
  ASSERT_EQ((int)b.size(), 8 * 44); // 346 bits
  Bits len;
  Put(len, 300, 10);
  EXPECT_TRUE(std::equal(len.begin(), len.end(), b.begin() + 12));
  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  EXPECT_EQ(back.paging_id_len, 300);
  ExpectSameR2d(m, back);

  m = Paging(AIOT_R2D_PAGING_CFA, false, 954);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 1000);
  b = Pack(m);
  ASSERT_EQ((int)b.size(), 8 * AIOT_MAC_MAX_TBS);
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  EXPECT_EQ(back.r2d_tbs, AIOT_MAC_MAX_TBS);
  ExpectSameR2d(m, back);
  m = Paging(AIOT_R2D_PAGING_CFA, false, 955);
  EXPECT_EQ(aiot_mac_r2d_natural_bytes(&m), AIOT_EINVAL);
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);

  m = Paging(AIOT_R2D_PAGING_CFA, true, 826);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 1000);
  b = Pack(m);
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  ExpectSameR2d(m, back);
  m = Paging(AIOT_R2D_PAGING_CFA, true, 827);
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);

  m = Paging(AIOT_R2D_PAGING_CBRA, true, 820);
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 1000);
  b = Pack(m);
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  ExpectSameR2d(m, back);
  m = Paging(AIOT_R2D_PAGING_CBRA, true, 821);
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);

  // the field itself holds 1023 at most
  m = Paging(AIOT_R2D_PAGING_CFA, false, AIOT_MAC_MAX_PAGING_ID_BITS);
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL); // 1069 bits
  m.paging_id_len = AIOT_MAC_MAX_PAGING_ID_BITS + 1;
  EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL);
}

// 6.2.1.1: the MAC entity ignores the values of the Fill Bits (CR 0016)
TEST(MacPdu, PagingFillBitsIgnored)
{
  for (aiot_r2d_kind_t kind : {AIOT_R2D_PAGING_CBRA, AIOT_R2D_PAGING_CFA}) {
    for (bool sppi : {false, true}) {
      aiot_mac_r2d_msg_t m = Paging(kind, sppi, 20);
      m.transaction_id = 5;
      const int body = aiot_mac_r2d_body_bits(&m);
      const int nat = aiot_mac_r2d_natural_bytes(&m);
      ASSERT_GT(body, 0);
      m.r2d_tbs = nat + 2;
      Bits b = Pack(m);
      ASSERT_EQ((int)b.size(), 8 * (nat + 2));
      for (int i = body; i < (int)b.size(); i++)
        EXPECT_EQ(b[i], 0) << i; // packed as zeros
      Bits ones = b;
      for (int i = body; i < (int)ones.size(); i++)
        ones[i] = 1;
      aiot_mac_r2d_msg_t back, back1;
      ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
      ASSERT_EQ(aiot_mac_parse_r2d(ones.data(), (int)ones.size(), &back1), 0);
      ExpectSameR2d(m, back);
      ExpectSameR2d(back, back1);
      EXPECT_EQ(back1.r2d_tbs, nat + 2);
    }
  }
  // Random ID Response: the 4 Fill Bits of 52 bits in 7 bytes
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_RANDOM_ID_RESPONSE;
  m.sched = Sched(AIOT_SCHED_RANDOM_ID_RESPONSE);
  m.n_entries = 1;
  m.entries[0] = {0x1234, -1, -1};
  const int body = aiot_mac_r2d_body_bits(&m);
  ASSERT_EQ(body, 52); // 3 + 7 + 24 + 1 + 16 + 1
  Bits b = Pack(m);
  ASSERT_EQ((int)b.size(), 56);
  for (int i = body; i < 56; i++)
    b[i] = 1;
  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), 56, &back), 0);
  ExpectSameR2d(m, back);
}

// 6.2.1.4: Received Data Size is {0..125}
TEST(MacPdu, ReceivedDataSizeRange)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_UPPER_LAYER_DATA;
  m.sched = Sched(AIOT_SCHED_R2D_DATA);
  m.as_id = 7;
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  for (int rds : {0, 1, AIOT_MAC_MAX_RECEIVED_DATA_SIZE}) {
    m.received_data_size = rds;
    Bits b = Pack(m);
    ASSERT_EQ((int)b.size(), 56);
    aiot_mac_r2d_msg_t back;
    ASSERT_EQ(aiot_mac_parse_r2d(b.data(), 56, &back), 0);
    EXPECT_EQ(back.received_data_size, rds);
  }
  for (int rds : {126, 127, 128, -1}) {
    m.received_data_size = rds;
    EXPECT_EQ(aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits)), AIOT_EINVAL) << rds;
  }
  // hand-built CI = 0 PDUs: 3 + 7 + 16 + 19 + 1 + 3 + 7 = 56 bits
  Bits sched = Pack(m.sched);
  ASSERT_EQ((int)sched.size(), 19);
  for (int rds : {125, 126, 127}) {
    Bits b;
    Put(b, AIOT_MAC_R2D_TYPE_UPPER_LAYER_DATA, 3);
    Put(b, 7, 7);
    Put(b, 7, 16);
    b.insert(b.end(), sched.begin(), sched.end());
    Put(b, 0, 1); // CI
    Put(b, 0, 3); // R
    Put(b, rds, 7);
    ASSERT_EQ((int)b.size(), 56);
    aiot_mac_r2d_msg_t back;
    EXPECT_EQ(aiot_mac_parse_r2d(b.data(), 56, &back), rds <= 125 ? 0 : AIOT_EINVAL) << rds;
    if (rds <= 125) {
      EXPECT_EQ(back.received_data_size, rds);
    }
  }
}

// Table 6.2.1.6-1 (CR 0009): the Unicast R_SFS is limited by the Bit Duration like the Broadcast ones
TEST(MacPdu, UnicastFrequencyLimitedByBitDuration)
{
  const int dur[8][3] = {{2, 1, 128}, {1, 1, 64}, {1, 2, 32}, {1, 4, 16}, {1, 8, 8}, {1, 16, 4}, {1, 32, 2}, {1, 96, 1}};
  for (int row = 0; row < 8; row++) {
    aiot_sched_info_t s = Sched(AIOT_SCHED_R2D_DATA);
    s.t_bit_num = dur[row][0];
    s.t_bit_den = dur[row][1];
    s.i_bit = 0;
    s.r_sfs[0] = dur[row][2];
    EXPECT_EQ(aiot_mac_sched_validate(&s), 0) << row;
    Bits b(32);
    EXPECT_EQ(aiot_mac_sched_pack(&s, b.data(), 32), 19) << row;
    s.r_sfs[0] = 2 * dur[row][2];
    EXPECT_EQ(aiot_mac_sched_validate(&s), AIOT_EINVAL) << row;
    EXPECT_EQ(aiot_mac_sched_pack(&s, b.data(), 32), AIOT_EINVAL) << row;

    // a hand-built field: Bit Duration row, Frequency codepoint log2(max) (valid) or one above
    int log2max = 0;
    while ((1 << log2max) < dur[row][2])
      log2max++;
    for (int cp : {log2max, log2max + 1}) {
      if (cp > 7)
        continue; // not a 3-bit codepoint
      Bits h;
      Put(h, row, 3);
      Put(h, cp, 3);
      Put(h, 0, 1); // Block
      Put(h, 0, 1); // FEC
      Put(h, 0, 2); // Interval
      Put(h, 0, 1);
      Put(h, 0, 1);
      Put(h, 0, 7); // D2R TBS 1
      ASSERT_EQ((int)h.size(), 19);
      aiot_sched_info_t back;
      EXPECT_EQ(aiot_mac_sched_parse(h.data(), 19, AIOT_SCHED_R2D_DATA, &back), cp == log2max ? 0 : AIOT_EINVAL)
          << row << " cp " << cp;
    }
  }
}

namespace {

aiot_mac_r2d_msg_t Valid(aiot_r2d_kind_t kind)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = kind;
  switch (kind) {
    case AIOT_R2D_PAGING_CBRA:
    case AIOT_R2D_PAGING_CFA:
      m = Paging(kind, true, 16);
      break;
    case AIOT_R2D_RANDOM_ID_RESPONSE:
      m.sched = Sched(AIOT_SCHED_RANDOM_ID_RESPONSE);
      m.n_entries = 1;
      m.entries[0] = {1, 2, 3};
      break;
    case AIOT_R2D_UPPER_LAYER_DATA:
      m.sched = Sched(AIOT_SCHED_R2D_DATA);
      m.as_id = 1;
      break;
    case AIOT_R2D_NACK_FEEDBACK:
      m.n_as_ids = 1;
      break;
    default:
      break;
  }
  return m;
}

int PackInto(const aiot_mac_r2d_msg_t &m)
{
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  return aiot_mac_pack_r2d(&m, bits, (int)sizeof(bits));
}

} // namespace

// Every bounded field at the top of its value range packs, one past it is refused rather than
// cut to its width (6.2.1, 6.2.2).
TEST(MacPdu, FieldsBeyondTheirRangeAreRefused)
{
  struct Case {
    const char *field;
    aiot_r2d_kind_t kind;
    void (*set)(aiot_mac_r2d_msg_t &, int);
    int top;
  };
  const Case cases[] = {
      {"Transaction ID", AIOT_R2D_PAGING_CBRA, [](aiot_mac_r2d_msg_t &m, int v) { m.transaction_id = v; }, 63},
      {"Number of Access Occasions",
       AIOT_R2D_PAGING_CBRA,
       [](aiot_mac_r2d_msg_t &m, int v) { m.num_access_occasions_code = v; },
       15},
      {"K", AIOT_R2D_PAGING_CBRA, [](aiot_mac_r2d_msg_t &m, int v) { m.k_bit = v; }, 1},
      {"Echoed Random ID",
       AIOT_R2D_RANDOM_ID_RESPONSE,
       [](aiot_mac_r2d_msg_t &m, int v) { m.entries[0].echoed_random_id = v; },
       0xFFFF},
      {"Assigned AS ID",
       AIOT_R2D_RANDOM_ID_RESPONSE,
       [](aiot_mac_r2d_msg_t &m, int v) { m.entries[0].assigned_as_id = v; },
       0xFFFF},
      {"Frequency Index", AIOT_R2D_RANDOM_ID_RESPONSE, [](aiot_mac_r2d_msg_t &m, int v) { m.entries[0].freq_index = v; }, 7},
      {"AS ID (R2D data)", AIOT_R2D_UPPER_LAYER_DATA, [](aiot_mac_r2d_msg_t &m, int v) { m.as_id = v; }, 0xFFFF},
      {"Received Data Size", AIOT_R2D_UPPER_LAYER_DATA, [](aiot_mac_r2d_msg_t &m, int v) { m.received_data_size = v; }, 125},
      {"AS ID (NACK)", AIOT_R2D_NACK_FEEDBACK, [](aiot_mac_r2d_msg_t &m, int v) { m.as_ids[0] = v; }, 0xFFFF},
  };
  for (const Case &c : cases) {
    aiot_mac_r2d_msg_t m = Valid(c.kind);
    c.set(m, c.top);
    EXPECT_GT(PackInto(m), 0) << c.field << " " << c.top;
    c.set(m, c.top + 1);
    EXPECT_EQ(PackInto(m), AIOT_EINVAL) << c.field << " " << c.top + 1;
    c.set(m, -2);
    EXPECT_EQ(PackInto(m), AIOT_EINVAL) << c.field << " -2";
  }
  // the R2D TBS given by the caller: 1..125, and not below the message
  aiot_mac_r2d_msg_t m = Valid(AIOT_R2D_PAGING_CFA);
  m.r2d_tbs = AIOT_MAC_MAX_TBS;
  EXPECT_EQ(PackInto(m), 8 * AIOT_MAC_MAX_TBS);
  m.r2d_tbs = AIOT_MAC_MAX_TBS + 1;
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);
  m.r2d_tbs = aiot_mac_r2d_natural_bytes(&m) - 1;
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);
  // the Paging ID Length has 10 bits; the longest CFA Paging ID fills 125 bytes exactly
  m = Valid(AIOT_R2D_PAGING_CFA);
  m.paging_id_len = AIOT_MAC_MAX_PAGING_ID_BITS + 1;
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);
  m.has_security_param = 0;
  m.paging_id_len = 8 * AIOT_MAC_MAX_TBS - (3 + 7 + 1 + 1 + 10 + 24);
  EXPECT_EQ(PackInto(m), 8 * AIOT_MAC_MAX_TBS);
  m.paging_id_len++;
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);
  // D2R: a 16-bit Random ID, an SDU of at most 123 bytes (6.2.2.2)
  aiot_mac_d2r_msg_t d;
  memset(&d, 0, sizeof(d));
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  d.kind = AIOT_D2R_ACCESS_RANDOM_ID;
  d.random_id = 0xFFFF;
  EXPECT_EQ(aiot_mac_pack_d2r(&d, bits, (int)sizeof(bits)), 16);
  d.random_id = 0x10000;
  EXPECT_EQ(aiot_mac_pack_d2r(&d, bits, (int)sizeof(bits)), AIOT_EINVAL);
  d.random_id = 1;
  EXPECT_EQ(aiot_mac_pack_d2r(&d, bits, 15), AIOT_ESPACE);
  uint8_t sdu[AIOT_MAC_MAX_TBS] = {};
  EXPECT_EQ(aiot_mac_d2r_for_tbs(sdu, AIOT_MAC_MAX_TBS - 2, AIOT_MAC_MAX_TBS, 0, &d), 0);
  EXPECT_EQ(aiot_mac_pack_d2r(&d, bits, (int)sizeof(bits)), 8 * AIOT_MAC_MAX_TBS);
  EXPECT_EQ(aiot_mac_d2r_for_tbs(sdu, AIOT_MAC_MAX_TBS - 1, AIOT_MAC_MAX_TBS, 0, &d), AIOT_EINVAL);
  d.sdu_len = AIOT_MAC_MAX_TBS - 1;
  d.padding_bytes = 0;
  EXPECT_EQ(aiot_mac_pack_d2r(&d, bits, (int)sizeof(bits)), AIOT_EINVAL);
  EXPECT_EQ(aiot_mac_d2r_for_tbs(sdu, 0, AIOT_MAC_MAX_TBS + 1, 0, &d), AIOT_EINVAL);
  EXPECT_EQ(aiot_mac_d2r_for_tbs(sdu, 0, 1, 0, &d), AIOT_EINVAL) << "no room for the 2-byte header";
}

namespace {

// The bits pack() gives for what parse() read, or an empty vector when parse() refuses them.
Bits Reparse(const Bits &y, aiot_mac_r2d_msg_t *parsed)
{
  if (aiot_mac_parse_r2d(y.data(), (int)y.size(), parsed) != 0)
    return {};
  Bits z(AIOT_MAC_MAX_PDU_BITS);
  const int n = aiot_mac_pack_r2d(parsed, z.data(), (int)z.size());
  EXPECT_EQ(n, (int)y.size()) << "a parsed message must pack to the same TBS";
  z.resize(std::max(n, 0));
  return z;
}

// The bits of a parsed message the receiver ignores: the R bits (6.2.1.4, 6.2.1.5) and the Fill
// Bits (6.2.1.1, 6.2.1.3).
std::vector<bool> Ignored(const aiot_mac_r2d_msg_t &p, int n)
{
  std::vector<bool> ign(n);
  auto mark = [&](int from, int to) {
    for (int i = from; i < to && i < n; i++)
      ign[i] = true;
  };
  const int r_at = 3 + 7 + 16 + aiot_mac_sched_width(AIOT_SCHED_R2D_DATA) + 1; // after CI
  switch (p.kind) {
    case AIOT_R2D_NACK_FEEDBACK:
      mark(10, 16);
      break;
    case AIOT_R2D_UPPER_LAYER_DATA:
      mark(r_at, r_at + (p.has_sdu ? 2 : 3));
      break;
    default:
      mark(aiot_mac_r2d_body_bits(&p), n);
      break;
  }
  return ign;
}

} // namespace

// What parse() accepts is exactly what pack() writes: a valid PDU with any one bit flipped is
// either refused, or read as a message that packs back to the flipped bits, but for the bits the
// receiver ignores (packed as zeros). A PDU cut by any number of bytes, or one byte too long, is
// refused.
TEST(MacPdu, ParseAcceptsOnlyWhatPackWrites)
{
  std::mt19937 gen(11);
  int refused = 0, accepted = 0;
  for (int t = 0; t < 120; t++) {
    const aiot_r2d_kind_t kind = (aiot_r2d_kind_t)(t % 6);
    if (kind == AIOT_R2D_ACCESS_TRIGGER)
      continue;
    aiot_mac_r2d_msg_t m = RandomR2d(gen, kind);
    if (aiot_mac_r2d_natural_bytes(&m) < 0)
      continue;
    const Bits x = Pack(m);
    for (size_t i = 0; i < x.size(); i++) {
      Bits y = x;
      y[i] ^= 1;
      aiot_mac_r2d_msg_t p;
      const Bits z = Reparse(y, &p);
      if (z.empty()) {
        refused++;
        continue;
      }
      accepted++;
      const std::vector<bool> ign = Ignored(p, (int)z.size());
      for (size_t j = 0; j < z.size(); j++)
        ASSERT_EQ(z[j], ign[j] ? 0 : y[j]) << "kind " << kind << " read as " << p.kind << ", bit " << i << " flipped, bit " << j;
      if (!ign[i] && p.kind == kind) {
        aiot_mac_r2d_msg_t again;
        ASSERT_EQ(aiot_mac_parse_r2d(z.data(), (int)z.size(), &again), 0);
        ExpectSameR2d(p, again);
      }
    }
    aiot_mac_r2d_msg_t p;
    for (size_t cut = 8; cut < x.size(); cut += 8)
      EXPECT_TRUE(Reparse(Bits(x.begin(), x.end() - cut), &p).empty()) << "kind " << kind << " cut " << cut;
    Bits longer = x;
    longer.insert(longer.end(), 8, 0);
    EXPECT_TRUE(Reparse(longer, &p).empty()) << "kind " << kind;
  }
  EXPECT_GT(refused, 0);
  EXPECT_GT(accepted, 0);
}

// The same for the D2R Upper Layer Data Transfer, whose ignored bits are the R bits and the MAC
// Padding.
TEST(MacPdu, D2RParseAcceptsOnlyWhatPackWrites)
{
  std::mt19937 gen(12);
  for (int t = 0; t < 80; t++) {
    const int tbs = 2 + gen() % (AIOT_MAC_MAX_TBS - 1);
    Bytes sdu(gen() % (tbs - 1));
    for (auto &c : sdu)
      c = gen() & 0xff;
    aiot_mac_d2r_msg_t m, p;
    ASSERT_EQ(aiot_mac_d2r_for_tbs(sdu.data(), (int)sdu.size(), tbs, gen() % 2, &m), 0);
    Bits x(8 * tbs);
    ASSERT_EQ(aiot_mac_pack_d2r(&m, x.data(), (int)x.size()), 8 * tbs);
    for (size_t i = 0; i < x.size(); i++) {
      Bits y = x;
      y[i] ^= 1;
      if (aiot_mac_parse_d2r(y.data(), (int)y.size(), AIOT_D2R_UPPER_LAYER_DATA, tbs, &p) != 0)
        continue;
      Bits z(8 * tbs);
      ASSERT_EQ(aiot_mac_pack_d2r(&p, z.data(), (int)z.size()), 8 * tbs);
      const int body = 16 + 8 * p.sdu_len;
      for (int j = 0; j < 8 * tbs; j++) {
        const bool ignored = (j >= 2 && j < 8) || j >= body; // R, MAC Padding
        ASSERT_EQ(z[j], ignored ? 0 : y[j]) << "bit " << i << " flipped, bit " << j;
      }
    }
    for (int cut = 8; cut < 8 * tbs; cut += 8)
      EXPECT_EQ(aiot_mac_parse_d2r(x.data(), 8 * tbs - cut, AIOT_D2R_UPPER_LAYER_DATA, tbs, &p), AIOT_EINVAL);
  }
}

// NACK Feedback (6.2.1.5) with the most AS IDs a TBS holds, 61 in 124 bytes: every one comes back,
// a 125-byte PDU (half an AS ID left over) and a 2-byte one (no AS ID) are refused.
TEST(MacPdu, NackWithSixtyOneAsIds)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_NACK_FEEDBACK;
  m.n_as_ids = AIOT_MAC_MAX_NACK_IDS;
  for (int i = 0; i < m.n_as_ids; i++)
    m.as_ids[i] = (i * 1093 + 7) & 0xFFFF;
  m.as_ids[0] = 0;
  m.as_ids[60] = 0xFFFF;
  const Bits b = Pack(m);
  ASSERT_EQ((int)b.size(), 8 * 124);
  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  ExpectSameR2d(m, back);
  EXPECT_EQ(back.r2d_tbs, 124);

  Bits odd = b;
  odd.insert(odd.end(), 8, 0);
  for (int j = 0; j < 7; j++)
    odd[3 + j] = (125 >> (6 - j)) & 1;
  EXPECT_EQ(aiot_mac_parse_r2d(odd.data(), (int)odd.size(), &back), AIOT_EINVAL);
  Bits empty;
  Put(empty, AIOT_MAC_R2D_TYPE_NACK_FEEDBACK, 3);
  Put(empty, 2, 7);
  Put(empty, 0, 6);
  EXPECT_EQ(aiot_mac_parse_r2d(empty.data(), (int)empty.size(), &back), AIOT_EINVAL);
}

// Random ID Response (6.2.1.3): at most 8 ID entries and at most 7 Fill Bits. The largest message,
// 8 entries with Frequency Index and Assigned AS ID, is 35 + 8 x 36 = 323 bits in 41 bytes.
TEST(MacPdu, RandomIdResponseEntriesAndFillBits)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_RANDOM_ID_RESPONSE;
  m.sched = Sched(AIOT_SCHED_RANDOM_ID_RESPONSE);
  m.n_entries = AIOT_MAC_MAX_ID_ENTRIES;
  for (int i = 0; i < m.n_entries; i++)
    m.entries[i] = {0xFFFF - i, i, i};
  EXPECT_EQ(aiot_mac_r2d_body_bits(&m), 323);
  Bits b = Pack(m);
  ASSERT_EQ((int)b.size(), 8 * 41);
  aiot_mac_r2d_msg_t back;
  ASSERT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), 0);
  ExpectSameR2d(m, back);

  m.n_entries = AIOT_MAC_MAX_ID_ENTRIES + 1;
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);
  m.n_entries = 2;
  m.entries[1].freq_index = -1; // FIPI applies to every entry
  EXPECT_EQ(PackInto(m), AIOT_EINVAL);

  // a ninth entry (AIPI 0: 3 + 16 + 1 bits) in the room after eight
  Bits nine(b.begin(), b.begin() + 323);
  Put(nine, 0, 3);
  Put(nine, 0x1234, 16);
  Put(nine, 0, 1);
  while (nine.size() % 8)
    nine.push_back(0);
  for (int j = 0; j < 7; j++)
    nine[3 + j] = ((int)nine.size() / 8 >> (6 - j)) & 1;
  EXPECT_EQ(aiot_mac_parse_r2d(nine.data(), (int)nine.size(), &back), AIOT_EINVAL);

  // one entry (FIPI 0, AIPI 0: 35 + 17 = 52 bits) and a whole spare byte of Fill Bits
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_RANDOM_ID_RESPONSE;
  m.sched = Sched(AIOT_SCHED_RANDOM_ID_RESPONSE);
  m.n_entries = 1;
  m.entries[0] = {0x4321, -1, -1};
  b = Pack(m);
  ASSERT_EQ((int)b.size(), 8 * 7);
  b.insert(b.end(), 8, 0);
  for (int j = 0; j < 7; j++)
    b[3 + j] = (8 >> (6 - j)) & 1;
  EXPECT_EQ(aiot_mac_parse_r2d(b.data(), (int)b.size(), &back), AIOT_EINVAL);
}
