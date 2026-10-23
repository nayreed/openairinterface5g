/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// aiot_mac_pdu.c: the field layouts of 38.391 clause 6 on hand-built PDUs, and pack and parse of
// random valid messages of every type.

#include <gtest/gtest.h>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "aiot_mac_pdu.h"

namespace {

using Bits = std::vector<uint8_t>;

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
    const int max = v == AIOT_SCHED_R2D_DATA ? 128 : dur[row][2];
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
      if (gen() % 2)
        paging_id(255);
      break;
    case AIOT_R2D_PAGING_CFA:
      m.sched = RandomSched(gen, AIOT_SCHED_PAGING_CFA);
      paging_id(255);
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
        m.received_data_size = gen() % 128;
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
  s.r_sfs[0] = 16; // the Unicast field is not limited by the bit duration...
  EXPECT_EQ(aiot_mac_sched_validate(&s), 0);
  aiot_d2r_params_t p;
  EXPECT_EQ(aiot_mac_to_d2r_params(&s, 16, AIOT_BPSK, &p), AIOT_EINVAL); // ...but the D2R is
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
  const uint8_t trigger[3] = {0, 1, 0};
  EXPECT_EQ(aiot_mac_parse_r2d(trigger, 3, &m), 0);
  EXPECT_EQ(m.kind, AIOT_R2D_ACCESS_TRIGGER);
  EXPECT_EQ(aiot_mac_parse_r2d(trigger, 2, &m), AIOT_EINVAL);
  EXPECT_EQ(aiot_mac_parse_r2d(NULL, 0, &m), AIOT_EINVAL);
}
