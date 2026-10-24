/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* The messages between nodes (aiot_node_msg.h): each field where the layout puts it, a round trip,
 * and the messages a receiver must refuse. */

#include <gtest/gtest.h>
#include <cstring>
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

} // namespace
