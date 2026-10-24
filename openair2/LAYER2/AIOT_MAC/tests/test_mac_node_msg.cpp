/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* The messages between nodes (aiot_node_msg.h): each field where the layout puts it, a round trip,
 * and the messages a receiver must refuse. Tag data first, then the split Reader's four. */

#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

#include "aiot_node_msg.h"

namespace {

aiot_tag_data_t data_message()
{
  aiot_tag_data_t m = {};
  m.seq = 0x01020304;
  m.after = AIOT_R2D_RANDOM_ID_RESPONSE;
  aiot_reader_answer_t &a = m.answer;
  a.kind = AIOT_ANSWER_DATA;
  a.message.kind = AIOT_D2R_UPPER_LAYER_DATA;
  a.message.mdi = 1;
  a.message.sdu_len = 3;
  a.message.sdu[0] = 0x00;
  a.message.sdu[1] = 0x02;
  a.message.sdu[2] = 0x2a;
  a.message.padding_bytes = 2;
  a.r_sfs = 4;
  a.rssi_w = 1.5e-7;
  a.start = 0x0000000a0b0c0d0eLL;
  a.slot = 0;
  a.entry = 1;
  a.as_id = 0x1234;
  return m;
}

std::vector<uint8_t> pack(const aiot_tag_data_t &m)
{
  std::vector<uint8_t> buf(AIOT_NODE_MSG_MAX_BYTES);
  const int n = aiot_node_msg_pack_tag_data(&m, buf.data(), buf.size());
  EXPECT_GT(n, 0);
  buf.resize(n > 0 ? n : 0);
  return buf;
}

void expect_same(const aiot_tag_data_t &a, const aiot_tag_data_t &b)
{
  EXPECT_EQ(a.seq, b.seq);
  EXPECT_EQ(a.after, b.after);
  EXPECT_EQ(a.answer.kind, b.answer.kind);
  EXPECT_EQ(a.answer.message.kind, b.answer.message.kind);
  EXPECT_EQ(a.answer.message.random_id, b.answer.message.random_id);
  EXPECT_EQ(a.answer.message.mdi, b.answer.message.mdi);
  EXPECT_EQ(a.answer.message.padding_bytes, b.answer.message.padding_bytes);
  ASSERT_EQ(a.answer.message.sdu_len, b.answer.message.sdu_len);
  EXPECT_EQ(0, memcmp(a.answer.message.sdu, b.answer.message.sdu, a.answer.message.sdu_len));
  EXPECT_EQ(a.answer.r_sfs, b.answer.r_sfs);
  EXPECT_EQ(a.answer.rssi_w, b.answer.rssi_w); // the same bits, not only close
  EXPECT_EQ(a.answer.start, b.answer.start);
  EXPECT_EQ(a.answer.slot, b.answer.slot);
  EXPECT_EQ(a.answer.entry, b.answer.entry);
  EXPECT_EQ(a.answer.as_id, b.answer.as_id);
}

TEST(NodeMsg, TagDataBytesAreWhereTheLayoutSays)
{
  const std::vector<uint8_t> b = pack(data_message());
  ASSERT_EQ(b.size(), (size_t)AIOT_TAG_DATA_FIXED_BYTES + 3);
  const std::vector<uint8_t> head = {1, AIOT_NODE_MSG_TAG_DATA, 0, AIOT_TAG_DATA_FIXED_BYTES + 3, 1, 2, 3, 4};
  EXPECT_EQ(std::vector<uint8_t>(b.begin(), b.begin() + 8), head);
  const std::vector<uint8_t> start = {0, 0, 0, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e};
  EXPECT_EQ(std::vector<uint8_t>(b.begin() + 8, b.begin() + 16), start);
  uint64_t bits;
  const double rssi = 1.5e-7;
  memcpy(&bits, &rssi, sizeof(bits));
  for (int i = 0; i < 8; i++)
    EXPECT_EQ(b[16 + i], (uint8_t)(bits >> (56 - 8 * i))) << "rssi byte " << i;
  const std::vector<uint8_t> rest =
      {AIOT_R2D_RANDOM_ID_RESPONSE, AIOT_ANSWER_DATA, 4, 1, 0, 0, 0, 1, 0, 0, 0x12, 0x34, 0, 0, 2, 3, 0x00, 0x02, 0x2a};
  EXPECT_EQ(std::vector<uint8_t>(b.begin() + 24, b.end()), rest);
}

TEST(NodeMsg, TagDataComesBackAsItWent)
{
  aiot_tag_data_t msg1 = {};
  msg1.seq = 7;
  msg1.after = AIOT_R2D_PAGING_CBRA;
  msg1.answer.kind = AIOT_ANSWER_MSG1;
  msg1.answer.message.kind = AIOT_D2R_ACCESS_RANDOM_ID;
  msg1.answer.message.random_id = 0xfedc;
  msg1.answer.r_sfs = 128;
  msg1.answer.rssi_w = 3.0e-12;
  msg1.answer.start = -5;
  msg1.answer.slot = 1;
  msg1.answer.entry = -1;
  msg1.answer.as_id = -1;
  aiot_tag_data_t full = data_message();
  full.seq = UINT32_MAX;
  full.answer.message.sdu_len = AIOT_MAC_MAX_TBS;
  for (int i = 0; i < AIOT_MAC_MAX_TBS; i++)
    full.answer.message.sdu[i] = 3 * i;
  for (const aiot_tag_data_t &m : {msg1, data_message(), full}) {
    const std::vector<uint8_t> b = pack(m);
    ASSERT_EQ(aiot_node_msg_type(b.data(), b.size()), AIOT_NODE_MSG_TAG_DATA);
    aiot_tag_data_t back;
    memset(&back, 0x55, sizeof(back));
    ASSERT_EQ(aiot_node_msg_parse_tag_data(b.data(), b.size(), &back), 0);
    expect_same(m, back);
  }
  EXPECT_EQ(pack(full).size(), (size_t)AIOT_NODE_MSG_MAX_BYTES);
}

TEST(NodeMsg, PackRefusesWhatTheLayoutCannotHold)
{
  std::vector<uint8_t> buf(AIOT_NODE_MSG_MAX_BYTES);
  const aiot_tag_data_t good = data_message();
  EXPECT_EQ(aiot_node_msg_pack_tag_data(&good, buf.data(), AIOT_TAG_DATA_FIXED_BYTES + 2), AIOT_ESPACE);
  EXPECT_EQ(aiot_node_msg_pack_tag_data(&good, buf.data(), AIOT_TAG_DATA_FIXED_BYTES + 3), AIOT_TAG_DATA_FIXED_BYTES + 3);
  EXPECT_EQ(aiot_node_msg_pack_tag_data(nullptr, buf.data(), buf.size()), AIOT_EINVAL);
  EXPECT_EQ(aiot_node_msg_pack_tag_data(&good, nullptr, buf.size()), AIOT_EINVAL);
  auto refused = [&](void (*spoil)(aiot_tag_data_t &)) {
    aiot_tag_data_t m = data_message();
    spoil(m);
    return aiot_node_msg_pack_tag_data(&m, buf.data(), buf.size()) == AIOT_EINVAL;
  };
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.after = (aiot_r2d_kind_t)6; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.kind = (aiot_answer_kind_t)2; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.message.kind = AIOT_D2R_ACCESS_RANDOM_ID; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.r_sfs = 256; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.message.mdi = 2; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.slot = 40000; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.entry = -2; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.message.random_id = 65536; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.message.sdu_len = AIOT_MAC_MAX_TBS + 1; }));
  EXPECT_TRUE(refused([](aiot_tag_data_t &m) { m.answer.message.padding_bytes = -1; }));
}

TEST(NodeMsg, ParseRefusesWhatIsNotOneTagDataMessage)
{
  const std::vector<uint8_t> good = pack(data_message());
  aiot_tag_data_t m;
  auto refused = [&](std::vector<uint8_t> b) { return aiot_node_msg_parse_tag_data(b.data(), b.size(), &m) == AIOT_EINVAL; };
  auto with = [&](int at, uint8_t v) {
    std::vector<uint8_t> b = good;
    b[at] = v;
    return b;
  };
  ASSERT_FALSE(refused(good));
  EXPECT_TRUE(refused(with(0, 2))); // another version
  EXPECT_TRUE(refused(with(1, 0))); // no such type
  EXPECT_TRUE(refused(with(3, good.size() + 1))); // the length field is not the datagram's
  EXPECT_TRUE(refused(std::vector<uint8_t>(good.begin(), good.end() - 1)));
  std::vector<uint8_t> longer = good;
  longer.push_back(0);
  EXPECT_TRUE(refused(longer));
  EXPECT_TRUE(refused(with(24, 6))); // an R2D kind that does not exist
  EXPECT_TRUE(refused(with(25, 2))); // an answer kind that does not exist
  EXPECT_TRUE(refused(with(27, 2))); // mdi is one bit
  EXPECT_TRUE(refused(with(38, AIOT_MAC_MAX_TBS + 1)));
  EXPECT_TRUE(refused(with(39, 4))); // the SDU length is not what the message holds
  EXPECT_TRUE(refused(std::vector<uint8_t>(good.begin(), good.begin() + 3)));
  const std::vector<uint8_t> header_only = {1, AIOT_NODE_MSG_TAG_DATA, 0, 4};
  EXPECT_EQ(aiot_node_msg_type(header_only.data(), header_only.size()), AIOT_NODE_MSG_TAG_DATA);
  EXPECT_TRUE(refused(header_only));
  EXPECT_EQ(aiot_node_msg_type(nullptr, 4), AIOT_EINVAL);
  EXPECT_EQ(aiot_node_msg_parse_tag_data(good.data(), good.size(), nullptr), AIOT_EINVAL);
}

/* ---------------------------------------------------------------------------------------------
 * The split Reader's messages
 * ------------------------------------------------------------------------------------------- */

template <typename M>
std::vector<uint8_t> pack_with(int (*f)(const M *, uint8_t *, int), const M &m)
{
  std::vector<uint8_t> buf(AIOT_NODE_SPLIT_MAX_BYTES);
  const int n = f(&m, buf.data(), buf.size());
  EXPECT_GT(n, 0);
  buf.resize(n > 0 ? n : 0);
  return buf;
}

aiot_node_r2d_t r2d_message()
{
  aiot_node_r2d_t m = {};
  m.seq = 0x0a0b0c0d;
  m.frame = 1023;
  m.slot = 9;
  m.symbol = 7;
  m.m_chip_symb = 6;
  m.n_rb = 1;
  m.a_bits = 10;
  const uint8_t bits[] = {1, 0, 1, 1, 0, 0, 1, 1, 1, 0};
  memcpy(m.bits, bits, sizeof(bits));
  return m;
}

aiot_node_window_t window_message()
{
  aiot_node_window_t m = {};
  m.at.frame = 513;
  m.at.slot = 4;
  m.at.sample = 5759;
  aiot_d2r_expect_t &e = m.expect;
  e.tag = -2;
  e.te = 0x01020304;
  e.a_bits = 40;
  e.params.t_bit_num = 1;
  e.params.t_bit_den = 8;
  e.params.r_sfs = 4;
  e.params.r_block = 2;
  e.params.r_code = 1;
  e.params.l_amble = 31;
  e.params.i_bit = 768;
  e.params.i_add = 1;
  e.params.modulation = AIOT_BPSK;
  e.n_r_sfs = 3;
  e.r_sfs[0] = 1;
  e.r_sfs[1] = 2;
  e.r_sfs[2] = 8;
  return m;
}

std::unique_ptr<aiot_node_outcome_t> outcome_message()
{
  auto m = std::make_unique<aiot_node_outcome_t>();
  m->ahead = -300;
  aiot_d2r_outcome_t &o = m->outcome;
  o.expect.tag = 0x1122334455667788LL;
  o.collided = 2;
  o.n_decoded = 2;
  o.decoded[0].start = -96;
  o.decoded[0].rssi_w = 1.25e-3;
  o.decoded[0].r_sfs = 2;
  o.decoded[0].n_bits = 9;
  for (int i = 0; i < 9; i++)
    o.decoded[0].bits[i] = i % 2;
  o.decoded[1].start = 70000;
  o.decoded[1].rssi_w = 4.0;
  o.decoded[1].r_sfs = 8;
  o.decoded[1].n_bits = 0;
  return m;
}

TEST(NodeMsg, R2dBytesAreWhereTheLayoutSays)
{
  const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_r2d, r2d_message());
  const std::vector<uint8_t> want =
      {1, AIOT_NODE_MSG_R2D, 0, 18, 0x0a, 0x0b, 0x0c, 0x0d, 0x03, 0xff, 9, 7, 6, 1, 0, 10, 0xb3, 0x80};
  EXPECT_EQ(b, want);
  EXPECT_EQ(aiot_node_msg_type(b.data(), b.size()), AIOT_NODE_MSG_R2D);
}

TEST(NodeMsg, R2dDoneBytesAreWhereTheLayoutSays)
{
  const aiot_node_r2d_done_t m = {7, -10, -3};
  const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_r2d_done, m);
  const std::vector<uint8_t> want = {1, AIOT_NODE_MSG_R2D_DONE, 0, 12, 0, 0, 0, 7, 0xff, 0xf6, 0xff, 0xfd};
  EXPECT_EQ(b, want);
}

TEST(NodeMsg, WindowBytesAreWhereTheLayoutSays)
{
  const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_window, window_message());
  const std::vector<uint8_t> want = {1,    AIOT_NODE_MSG_WINDOW,
                                     0,    37,
                                     0xff, 0xff,
                                     0xff, 0xff,
                                     0xff, 0xff,
                                     0xff, 0xfe,
                                     1,    2,
                                     3,    4,
                                     2,    1,
                                     0x16, 0x7f,
                                     4,    3,
                                     0,    40,
                                     3,    0,
                                     1,    8,
                                     4,    2,
                                     1,    31,
                                     1,    1,
                                     1,    2,
                                     8};
  EXPECT_EQ(b, want);
}

TEST(NodeMsg, OutcomeBytesAreWhereTheLayoutSays)
{
  const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_outcome, *outcome_message());
  ASSERT_EQ(b.size(), (size_t)AIOT_NODE_OUTCOME_FIXED_BYTES + 2 * AIOT_NODE_D2R_FIXED_BYTES + 2);
  const std::vector<uint8_t> head = {
      1, AIOT_NODE_MSG_OUTCOME, 0, 52, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xff, 0xff, 0xfe, 0xd4, 0, 2, 0, 2};
  EXPECT_EQ(std::vector<uint8_t>(b.begin(), b.begin() + 20), head);
  EXPECT_EQ(std::vector<uint8_t>(b.begin() + 20, b.begin() + 24), std::vector<uint8_t>({0xff, 0xff, 0xff, 0xa0}));
  uint64_t bits;
  const double rssi = 1.25e-3;
  memcpy(&bits, &rssi, sizeof(bits));
  for (int i = 0; i < 8; i++)
    EXPECT_EQ(b[24 + i], (uint8_t)(bits >> (56 - 8 * i))) << "rssi byte " << i;
  EXPECT_EQ(std::vector<uint8_t>(b.begin() + 32, b.begin() + 37), std::vector<uint8_t>({0, 9, 2, 0x55, 0x00}));
  EXPECT_EQ(std::vector<uint8_t>(b.begin() + 37, b.begin() + 41), std::vector<uint8_t>({0, 1, 0x11, 0x70}));
  EXPECT_EQ(std::vector<uint8_t>(b.end() - 3, b.end()), std::vector<uint8_t>({0, 0, 8}));
}

TEST(NodeMsg, SplitMessagesComeBackAsTheyWent)
{
  aiot_node_r2d_t big = r2d_message();
  big.seq = UINT32_MAX;
  big.frame = 0;
  big.slot = 0;
  big.symbol = 0;
  big.a_bits = AIOT_MAX_TB_BITS;
  for (int i = 0; i < AIOT_MAX_TB_BITS; i++)
    big.bits[i] = (i * 7 / 3) % 2;
  for (const aiot_node_r2d_t &m : {r2d_message(), big}) {
    const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_r2d, m);
    aiot_node_r2d_t back;
    memset(&back, 0x55, sizeof(back));
    ASSERT_EQ(aiot_node_msg_parse_r2d(b.data(), b.size(), &back), 0);
    EXPECT_EQ(back.seq, m.seq);
    EXPECT_EQ(std::vector<int>({back.frame, back.slot, back.symbol, back.m_chip_symb, back.n_rb, back.a_bits}),
              std::vector<int>({m.frame, m.slot, m.symbol, m.m_chip_symb, m.n_rb, m.a_bits}));
    EXPECT_EQ(0, memcmp(back.bits, m.bits, m.a_bits));
  }

  for (const aiot_node_r2d_done_t &m : {aiot_node_r2d_done_t{0, 12, 40}, aiot_node_r2d_done_t{9, INT16_MIN, INT16_MAX}}) {
    const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_r2d_done, m);
    aiot_node_r2d_done_t back = {};
    ASSERT_EQ(aiot_node_msg_parse_r2d_done(b.data(), b.size(), &back), 0);
    EXPECT_EQ(std::vector<int>({(int)back.seq, back.result, back.ahead}), std::vector<int>({(int)m.seq, m.result, m.ahead}));
  }

  aiot_node_window_t none = window_message();
  none.expect.n_r_sfs = 0;
  none.expect.tag = INT64_MAX;
  none.at = {};
  for (const aiot_node_window_t &m : {window_message(), none}) {
    const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_window, m);
    aiot_node_window_t back;
    memset(&back, 0x55, sizeof(back));
    ASSERT_EQ(aiot_node_msg_parse_window(b.data(), b.size(), &back), 0);
    EXPECT_EQ(std::vector<int>({back.at.frame, back.at.slot, back.at.sample}),
              std::vector<int>({m.at.frame, m.at.slot, m.at.sample}));
    const aiot_d2r_expect_t &e = m.expect, &f = back.expect;
    EXPECT_EQ(f.start, 0);
    EXPECT_EQ(f.tag, e.tag);
    EXPECT_EQ(std::vector<int>({f.te, f.a_bits, f.n_r_sfs}), std::vector<int>({e.te, e.a_bits, e.n_r_sfs}));
    EXPECT_EQ(0, memcmp(&f.params, &e.params, sizeof(e.params)));
    for (int i = 0; i < e.n_r_sfs; i++)
      EXPECT_EQ(f.r_sfs[i], e.r_sfs[i]);
  }

  auto full = std::make_unique<aiot_node_outcome_t>();
  full->ahead = INT32_MAX;
  full->outcome.expect.tag = INT64_MIN;
  full->outcome.collided = UINT16_MAX;
  full->outcome.n_decoded = AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS;
  for (int i = 0; i < full->outcome.n_decoded; i++) {
    aiot_d2r_result_t &d = full->outcome.decoded[i];
    d.start = i % 2 ? INT32_MIN : INT32_MAX - i;
    d.rssi_w = 1.0 / (i + 1);
    d.r_sfs = i % 9;
    d.n_bits = AIOT_MAX_TB_BITS;
    for (int k = 0; k < d.n_bits; k++)
      d.bits[k] = (k + i) % 3 == 0;
  }
  EXPECT_EQ(pack_with(aiot_node_msg_pack_outcome, *full).size(), (size_t)AIOT_NODE_SPLIT_MAX_BYTES);
  auto empty = std::make_unique<aiot_node_outcome_t>();
  empty->outcome.expect.tag = 3;
  const auto two = outcome_message();
  for (const aiot_node_outcome_t *m : {two.get(), full.get(), empty.get()}) {
    const std::vector<uint8_t> b = pack_with(aiot_node_msg_pack_outcome, *m);
    auto back = std::make_unique<aiot_node_outcome_t>();
    memset(back.get(), 0x55, sizeof(*back));
    ASSERT_EQ(aiot_node_msg_parse_outcome(b.data(), b.size(), back.get()), 0);
    const aiot_d2r_outcome_t &o = m->outcome, &q = back->outcome;
    EXPECT_EQ(back->ahead, m->ahead);
    EXPECT_EQ(q.expect.tag, o.expect.tag);
    EXPECT_EQ(q.expect.start, 0);
    EXPECT_EQ(q.collided, o.collided);
    ASSERT_EQ(q.n_decoded, o.n_decoded);
    for (int i = 0; i < o.n_decoded; i++) {
      EXPECT_EQ(q.decoded[i].start, o.decoded[i].start);
      EXPECT_EQ(q.decoded[i].rssi_w, o.decoded[i].rssi_w);
      EXPECT_EQ(q.decoded[i].r_sfs, o.decoded[i].r_sfs);
      EXPECT_EQ(q.decoded[i].tag, o.expect.tag);
      ASSERT_EQ(q.decoded[i].n_bits, o.decoded[i].n_bits);
      EXPECT_EQ(0, memcmp(q.decoded[i].bits, o.decoded[i].bits, o.decoded[i].n_bits));
    }
  }
}

TEST(NodeMsg, SplitPackRefusesWhatTheLayoutCannotHold)
{
  std::vector<uint8_t> buf(AIOT_NODE_SPLIT_MAX_BYTES);
  const aiot_node_r2d_t r = r2d_message();
  EXPECT_EQ(aiot_node_msg_pack_r2d(&r, buf.data(), 17), AIOT_ESPACE);
  auto r2d_refused = [&](void (*spoil)(aiot_node_r2d_t &)) {
    aiot_node_r2d_t m = r2d_message();
    spoil(m);
    return aiot_node_msg_pack_r2d(&m, buf.data(), buf.size()) == AIOT_EINVAL;
  };
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.frame = 1024; }));
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.slot = 10; }));
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.symbol = 3; }));
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.a_bits = 0; }));
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.a_bits = AIOT_MAX_TB_BITS + 1; }));
  EXPECT_TRUE(r2d_refused([](aiot_node_r2d_t &m) { m.n_rb = 256; }));

  aiot_node_r2d_done_t done = {1, 40000, 0};
  EXPECT_EQ(aiot_node_msg_pack_r2d_done(&done, buf.data(), buf.size()), AIOT_EINVAL);
  done = {1, 0, -40000};
  EXPECT_EQ(aiot_node_msg_pack_r2d_done(&done, buf.data(), buf.size()), AIOT_EINVAL);
  done = {1, 0, 0};
  EXPECT_EQ(aiot_node_msg_pack_r2d_done(&done, buf.data(), AIOT_NODE_R2D_DONE_BYTES - 1), AIOT_ESPACE);

  const aiot_node_window_t w = window_message();
  EXPECT_EQ(aiot_node_msg_pack_window(&w, buf.data(), AIOT_NODE_WINDOW_FIXED_BYTES + 2), AIOT_ESPACE);
  auto window_refused = [&](void (*spoil)(aiot_node_window_t &)) {
    aiot_node_window_t m = window_message();
    spoil(m);
    return aiot_node_msg_pack_window(&m, buf.data(), buf.size()) == AIOT_EINVAL;
  };
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.at.sample = AIOT_SAMPLES_PER_MS; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.at.slot = -1; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.expect.te = -1; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.expect.n_r_sfs = AIOT_MAX_SHIFTS + 1; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.expect.r_sfs[1] = 256; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.expect.params.i_bit = 65536; }));
  EXPECT_TRUE(window_refused([](aiot_node_window_t &m) { m.expect.params.modulation = (aiot_mod_t)2; }));

  auto outcome_refused = [&](void (*spoil)(aiot_node_outcome_t &)) {
    auto m = outcome_message();
    spoil(*m);
    return aiot_node_msg_pack_outcome(m.get(), buf.data(), buf.size()) == AIOT_EINVAL;
  };
  EXPECT_TRUE(outcome_refused([](aiot_node_outcome_t &m) { m.outcome.n_decoded = AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS + 1; }));
  EXPECT_TRUE(outcome_refused([](aiot_node_outcome_t &m) { m.outcome.collided = 65536; }));
  EXPECT_TRUE(outcome_refused([](aiot_node_outcome_t &m) { m.outcome.decoded[1].start = (int64_t)INT32_MAX + 1; }));
  EXPECT_TRUE(outcome_refused([](aiot_node_outcome_t &m) { m.outcome.decoded[0].n_bits = AIOT_MAX_TB_BITS + 1; }));
  EXPECT_TRUE(outcome_refused([](aiot_node_outcome_t &m) { m.outcome.decoded[0].r_sfs = 256; }));
  auto m = outcome_message();
  EXPECT_EQ(aiot_node_msg_pack_outcome(m.get(), buf.data(), 51), AIOT_ESPACE);
  EXPECT_EQ(aiot_node_msg_pack_outcome(nullptr, buf.data(), buf.size()), AIOT_EINVAL);
}

TEST(NodeMsg, SplitParseRefusesWhatIsNotOneMessageOfItsType)
{
  aiot_node_r2d_t r;
  aiot_node_r2d_done_t done;
  aiot_node_window_t w;
  auto o = std::make_unique<aiot_node_outcome_t>();
  const std::vector<uint8_t> r2d = pack_with(aiot_node_msg_pack_r2d, r2d_message());
  const std::vector<uint8_t> d = pack_with(aiot_node_msg_pack_r2d_done, aiot_node_r2d_done_t{1, 2, 3});
  const std::vector<uint8_t> win = pack_with(aiot_node_msg_pack_window, window_message());
  const std::vector<uint8_t> out = pack_with(aiot_node_msg_pack_outcome, *outcome_message());
  auto with = [](std::vector<uint8_t> b, int at, uint8_t v) {
    b[at] = v;
    return b;
  };
  auto shorter = [](std::vector<uint8_t> b) {
    b.pop_back();
    b[3]--;
    return b;
  };
  auto longer = [](std::vector<uint8_t> b) {
    b.push_back(0);
    b[3]++;
    return b;
  };
  auto r2d_ok = [&](const std::vector<uint8_t> &b) { return aiot_node_msg_parse_r2d(b.data(), b.size(), &r) == 0; };
  auto done_ok = [&](const std::vector<uint8_t> &b) { return aiot_node_msg_parse_r2d_done(b.data(), b.size(), &done) == 0; };
  auto win_ok = [&](const std::vector<uint8_t> &b) { return aiot_node_msg_parse_window(b.data(), b.size(), &w) == 0; };
  auto out_ok = [&](const std::vector<uint8_t> &b) { return aiot_node_msg_parse_outcome(b.data(), b.size(), o.get()) == 0; };

  ASSERT_TRUE(r2d_ok(r2d) && done_ok(d) && win_ok(win) && out_ok(out));
  // each message is only its own type
  EXPECT_FALSE(r2d_ok(d) || r2d_ok(win) || r2d_ok(out));
  EXPECT_FALSE(done_ok(r2d) || done_ok(win) || done_ok(out));
  EXPECT_FALSE(win_ok(r2d) || win_ok(d) || win_ok(out));
  EXPECT_FALSE(out_ok(r2d) || out_ok(d) || out_ok(win));
  aiot_tag_data_t t;
  EXPECT_NE(aiot_node_msg_parse_tag_data(out.data(), out.size(), &t), 0);
  // a message with a byte too few or too many, its length field agreeing
  EXPECT_FALSE(r2d_ok(shorter(r2d)) || r2d_ok(longer(r2d)));
  EXPECT_FALSE(done_ok(shorter(d)) || done_ok(longer(d)));
  EXPECT_FALSE(win_ok(shorter(win)) || win_ok(longer(win)));
  EXPECT_FALSE(out_ok(shorter(out)) || out_ok(longer(out)));
  // fields out of their range
  EXPECT_FALSE(r2d_ok(with(r2d, 8, 4))); // frame 1024 and more
  EXPECT_FALSE(r2d_ok(with(r2d, 10, 10))); // slot
  EXPECT_FALSE(r2d_ok(with(r2d, 11, 1))); // symbol
  EXPECT_FALSE(r2d_ok(with(r2d, 15, 0))); // no bits
  EXPECT_FALSE(r2d_ok(with(r2d, 15, 17))); // more bits than the message holds
  EXPECT_FALSE(win_ok(with(win, 12, 0x80))); // te < 0
  EXPECT_FALSE(win_ok(with(win, 16, 4))); // frame
  EXPECT_FALSE(win_ok(with(win, 18, 0x17))); // sample 5760 and more
  EXPECT_FALSE(win_ok(with(win, 20, 10))); // slot
  EXPECT_FALSE(win_ok(with(win, 21, 4))); // more R_SFS than the message holds
  EXPECT_FALSE(win_ok(with(win, 33, 2))); // modulation
  EXPECT_FALSE(out_ok(with(out, 19, 3))); // more D2Rs than the message holds
  EXPECT_FALSE(out_ok(with(out, 19, 1))); // fewer
  EXPECT_FALSE(out_ok(with(out, 33, 17))); // a D2R with more bits than the message holds
  EXPECT_FALSE(out_ok(with(out, 0, 2))); // another version
  EXPECT_FALSE(r2d_ok({}) || done_ok({}) || win_ok({}) || out_ok({}));
  EXPECT_NE(aiot_node_msg_parse_outcome(out.data(), out.size(), nullptr), 0);
}

} // namespace
