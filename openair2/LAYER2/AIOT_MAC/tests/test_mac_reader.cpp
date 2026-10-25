/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* The Reader's MAC with its protocols, against devices running this library's device MAC, over an
 * ideal air: every D2R a device sends reaches the Reader, and two on the same window and R_SFS
 * collide. Each test checks that the Reader expects every D2R exactly where the device sends it,
 * and what each protocol achieves. */

#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "aiot_mac.h"

namespace {

aiot_link_config_t default_link()
{
  aiot_link_config_t c = {};
  c.m_chip_symb = 6;
  c.t_bit_num = 1;
  c.t_bit_den = 8;
  c.n_r_sfs = 4;
  c.r_sfs[0] = 1;
  c.r_sfs[1] = 2;
  c.r_sfs[2] = 4;
  c.r_sfs[3] = 8;
  c.x = 1;
  c.r_code = 1;
  c.r_block = 1;
  c.l_amble = 31;
  c.d2r_tbs = 5;
  return c;
}

struct Node {
  int id;
  std::mt19937 rng;
  aiot_device_t *mac = nullptr;
  int delivered = 0;
};

int matched(void *user, const uint8_t *paging_id, int len)
{
  const Node *n = static_cast<Node *>(user);
  uint8_t mine[16];
  aiot_paging_id_of_node(n->id, mine);
  return len == 16 && std::equal(mine, mine + 16, paging_id);
}

int next_sdu(void *user, uint8_t *sdu, int cap)
{
  const Node *n = static_cast<Node *>(user);
  if (cap < 3)
    return -1;
  sdu[0] = n->id >> 8;
  sdu[1] = n->id & 0xff;
  sdu[2] = 0x2a;
  return 3;
}

uint32_t draw(void *user, uint32_t n)
{
  return std::uniform_int_distribution<uint32_t>(0, n - 1)(static_cast<Node *>(user)->rng);
}

int deliver(void *user, const uint8_t *, int)
{
  static_cast<Node *>(user)->delivered++;
  return 0;
}

struct Air {
  aiot_reader_t *reader = nullptr;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<aiot_reader_heard_t>> heard;
  int64_t now = 0;
  int64_t last_d2r_end = 0;
  int unmatched = 0; /* D2Rs no window of the Reader caught */
  int late_r2d = 0; /* R2Ds that started before a D2R had ended */
  int lose_ridr = 0; /* the next Random ID Responses whose answers the air loses */

  Air(const char *protocol, std::vector<aiot_param_t> params, int n_nodes, const aiot_link_config_t &cfg = default_link())
  {
    std::vector<int> ids;
    for (int i = 0; i < n_nodes; i++) {
      auto n = std::make_unique<Node>();
      n->id = 0x100 + i;
      n->rng.seed(1000 + i);
      aiot_device_upper_t up = {n.get(), matched, nullptr, deliver, next_sdu, draw, nullptr};
      n->mac = aiot_device_new(&up, AIOT_BPSK);
      ids.push_back(n->id);
      nodes.push_back(std::move(n));
    }
    char err[200] = "";
    reader = aiot_reader_new(&cfg, protocol, params.data(), params.size(), ids.data(), ids.size(), err, sizeof(err));
    EXPECT_NE(reader, nullptr) << err;
  }

  ~Air()
  {
    aiot_reader_free(reader);
    for (auto &n : nodes)
      aiot_device_free(n->mac);
  }

  struct Sent {
    aiot_device_send_t s;
    int64_t at;
  };

  /* One R2D and its answers. Returns false once the protocol is done. */
  bool step()
  {
    aiot_reader_r2d_t r2d;
    const int ready = aiot_reader_next_r2d(reader, now, &r2d);
    EXPECT_GE(ready, 0);
    if (ready <= 0)
      return false;
    const int64_t start = (r2d.earliest_start + AIOT_HALF_SUBFRAME - 1) / AIOT_HALF_SUBFRAME * AIOT_HALF_SUBFRAME;
    if (start < last_d2r_end)
      late_r2d++;
    const int64_t end = start + aiot_r2d_n_samples(14);

    aiot_mac_r2d_msg_t msg;
    EXPECT_EQ(aiot_mac_parse_r2d(r2d.bits, r2d.n_bits, &msg), 0);
    std::vector<Sent> sent;
    for (auto &n : nodes) {
      Sent s;
      if (aiot_device_on_r2d(n->mac, &msg, &s.s) == 1) {
        s.at = end + aiot_device_send_offset(&s.s, 6);
        sent.push_back(s);
      }
    }

    aiot_d2r_expect_t exp[16];
    const int n_exp = aiot_reader_r2d_sent(reader, start, end, end, exp, 16);
    EXPECT_GE(n_exp, 0);
    std::vector<bool> used(sent.size());
    const bool lost = msg.kind == AIOT_R2D_RANDOM_ID_RESPONSE && lose_ridr > 0 && lose_ridr--;
    for (int k = 0; k < n_exp; k++) {
      const aiot_d2r_expect_t &e = exp[k];
      auto o = std::make_unique<aiot_d2r_outcome_t>();
      o->expect = e;
      for (int i = 0; i < e.n_r_sfs; i++) {
        std::vector<size_t> here;
        for (size_t j = 0; j < sent.size(); j++)
          if (!used[j] && sent[j].s.params.r_sfs == e.r_sfs[i] && sent[j].at >= e.start - e.te && sent[j].at <= e.start + e.te)
            here.push_back(j);
        for (size_t j : here) {
          used[j] = true;
          EXPECT_EQ(sent[j].s.n_bits, e.a_bits);
          const int64_t d2r_end =
              sent[j].at + (int64_t)aiot_frame_bits(e.a_bits, &sent[j].s.params) * aiot_bit_samples(&sent[j].s.params);
          last_d2r_end = std::max(last_d2r_end, d2r_end);
        }
        if (lost) {
        } else if (here.size() == 1) {
          aiot_d2r_result_t &res = o->decoded[o->n_decoded++];
          const aiot_device_send_t &s = sent[here[0]].s;
          std::copy(s.bits, s.bits + s.n_bits, res.bits);
          res.n_bits = s.n_bits;
          res.r_sfs = s.params.r_sfs;
          res.start = sent[here[0]].at;
          res.rssi_w = 1e-9;
          res.tag = e.tag;
        } else if (here.size() > 1) {
          o->collided++;
        }
      }
      now = std::max(now, e.start + e.te);
      EXPECT_EQ(aiot_reader_d2r_outcome(reader, o.get(), now), 0);
    }
    unmatched += std::count(used.begin(), used.end(), false);
    now = std::max(now, end);

    auto h = std::make_unique<aiot_reader_heard_t>();
    while (aiot_reader_pop_heard(reader, h.get()) == 1) {
      heard.push_back(std::move(h));
      h = std::make_unique<aiot_reader_heard_t>();
    }
    return true;
  }

  int run(int n_r2d)
  {
    int i = 0;
    while (i < n_r2d && step())
      i++;
    return i;
  }

  /* The node ids the data answers carried (first two SDU bytes). */
  std::vector<int> data_from() const
  {
    std::vector<int> out;
    for (auto &h : heard)
      for (int i = 0; i < h->n_answers; i++)
        if (h->answers[i].kind == AIOT_ANSWER_DATA && h->answers[i].message.sdu_len >= 2)
          out.push_back(h->answers[i].message.sdu[0] << 8 | h->answers[i].message.sdu[1]);
    return out;
  }
};

} // namespace

TEST(ReaderProtocols, ListsThemAll)
{
  std::set<std::string> names;
  for (int i = 0; i < aiot_protocol_count(); i++)
    names.insert(aiot_protocol_name(i));
  EXPECT_EQ(names, (std::set<std::string>{"adaptive_aloha", "broadcast", "gen2_q", "manual", "registration", "unicast"}));
  const char *pn[8];
  double def[8];
  int is_int[8];
  ASSERT_EQ(aiot_protocol_params("gen2_q", pn, def, is_int, 8), 2);
  EXPECT_STREQ(pn[0], "q0");
  EXPECT_DOUBLE_EQ(def[1], 0.3);
  EXPECT_LT(aiot_protocol_params("nope", pn, def, is_int, 8), 0);
}

TEST(ReaderProtocols, RejectsBadParameters)
{
  const aiot_link_config_t c = default_link();
  int id = 1;
  char err[200];
  EXPECT_EQ(aiot_reader_new(&c, "nope", nullptr, 0, &id, 1, err, sizeof(err)), nullptr);
  EXPECT_NE(std::string(err).find("nope"), std::string::npos);
  aiot_param_t p = {"occasions", 2};
  EXPECT_EQ(aiot_reader_new(&c, "broadcast", &p, 1, &id, 1, err, sizeof(err)), nullptr);
  EXPECT_NE(std::string(err).find("occasions"), std::string::npos);
  p = {"occasions_exp", 2.5};
  EXPECT_EQ(aiot_reader_new(&c, "broadcast", &p, 1, &id, 1, err, sizeof(err)), nullptr);
  p = {"occasions_exp", 16};
  EXPECT_EQ(aiot_reader_new(&c, "broadcast", &p, 1, &id, 1, err, sizeof(err)), nullptr);
  aiot_link_config_t bad = c;
  bad.m_chip_symb = 3;
  EXPECT_EQ(aiot_reader_new(&bad, "broadcast", nullptr, 0, &id, 1, err, sizeof(err)), nullptr);
}

TEST(Reader, UnicastAsksEachNodeInTurn)
{
  Air air("unicast", {}, 3);
  air.run(6);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  ASSERT_EQ(air.heard.size(), 6u);
  for (size_t i = 0; i < air.heard.size(); i++)
    EXPECT_EQ(air.heard[i]->sent.kind, AIOT_R2D_PAGING_CFA);
  EXPECT_EQ(air.data_from(), (std::vector<int>{0x100, 0x101, 0x102, 0x100, 0x101, 0x102}));
}

TEST(Reader, BroadcastReadsEveryNode)
{
  Air air("broadcast", {{"occasions_exp", 4}}, 5);
  air.run(60);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  const auto from = air.data_from();
  EXPECT_EQ(std::set<int>(from.begin(), from.end()), (std::set<int>{0x100, 0x101, 0x102, 0x103, 0x104}));
}

TEST(Reader, RandomIdResponseAnswersTheMsg1sInOrder)
{
  Air air("broadcast", {{"occasions_exp", 2}}, 3);
  air.run(20);
  int next_as_id = 1;
  for (size_t k = 0; k + 1 < air.heard.size(); k++) {
    const auto &h = *air.heard[k];
    const auto &r = *air.heard[k + 1];
    std::vector<int> msg1;
    for (int i = 0; i < h.n_answers; i++)
      if (h.answers[i].kind == AIOT_ANSWER_MSG1)
        msg1.push_back(i);
    if (h.sent.kind == AIOT_R2D_RANDOM_ID_RESPONSE || msg1.empty())
      continue;
    ASSERT_EQ(r.sent.kind, AIOT_R2D_RANDOM_ID_RESPONSE);
    ASSERT_EQ(r.sent.n_entries, (int)msg1.size());
    for (int i = 0; i < r.sent.n_entries; i++) {
      const auto &a = h.answers[msg1[i]];
      EXPECT_EQ(r.sent.entries[i].echoed_random_id, a.message.random_id);
      EXPECT_EQ(r.sent.entries[i].assigned_as_id, next_as_id++);
      EXPECT_EQ(aiot_mac_freq_index_to_r_sfs(r.sent.entries[i].freq_index), a.r_sfs);
      EXPECT_EQ(r.sent.sched.r_sfs[i], 1 << i);
    }
  }
  EXPECT_GT(next_as_id, 1);
}

TEST(Reader, RegistrationGivesEveryNodeAnAsIdAndPollsIt)
{
  Air air("registration", {{"occasions_exp", 3}}, 6);
  air.run(80);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  std::set<int> as_ids;
  for (auto &n : air.nodes) {
    aiot_device_state_t st;
    aiot_device_state(n->mac, &st);
    EXPECT_GE(st.as_id, 1);
    as_ids.insert(st.as_id);
  }
  EXPECT_EQ(as_ids.size(), air.nodes.size());
  /* the polls: R2D data to an AS ID, answered by that node */
  int polls = 0;
  for (auto &h : air.heard)
    if (h->sent.kind == AIOT_R2D_UPPER_LAYER_DATA) {
      polls++;
      ASSERT_EQ(h->n_answers, 1);
      EXPECT_EQ(h->answers[0].as_id, h->sent.as_id);
    }
  EXPECT_GT(polls, 6);
}

// The data a Tag sends after its Random ID Response is lost (a collision, a fade, or a split
// Reader's window given up). The Tag keeps its AS ID and would ignore the later pagings of the run's
// one Transaction ID (38.391 5.2). The Reader NACKs the AS IDs whose data it did not hear: the Tag
// releases its AS ID, finds its procedure failed (5.5.2) and joins again at the next paging.
TEST(Reader, RegistrationNacksATagWhoseFirstDataWasLost)
{
  Air air("registration", {{"occasions_exp", 3}}, 6);
  air.lose_ridr = 1;
  air.run(120);
  EXPECT_EQ(air.lose_ridr, 0);
  EXPECT_EQ(air.unmatched, 0);

  /* right after the first Random ID Response, a NACK Feedback with every AS ID it gave */
  size_t first = 0;
  while (first < air.heard.size() && air.heard[first]->sent.kind != AIOT_R2D_RANDOM_ID_RESPONSE)
    first++;
  ASSERT_LT(first + 1, air.heard.size());
  const aiot_mac_r2d_msg_t &ridr = air.heard[first]->sent;
  const aiot_mac_r2d_msg_t &nack = air.heard[first + 1]->sent;
  ASSERT_EQ(nack.kind, AIOT_R2D_NACK_FEEDBACK);
  ASSERT_EQ(nack.n_as_ids, ridr.n_entries);
  std::set<int> nacked;
  for (int i = 0; i < nack.n_as_ids; i++) {
    EXPECT_EQ(nack.as_ids[i], ridr.entries[i].assigned_as_id);
    nacked.insert(nack.as_ids[i]);
  }

  /* every node joined again and sent data; no poll goes to an AS ID that was NACKed, and every
   * poll is answered */
  const std::vector<int> from = air.data_from();
  for (auto &n : air.nodes) {
    EXPECT_NE(std::find(from.begin(), from.end(), n->id), from.end()) << "no data from node " << n->id;
    aiot_device_state_t st;
    aiot_device_state(n->mac, &st);
    EXPECT_GE(st.as_id, 1);
    EXPECT_EQ(nacked.count(st.as_id), 0u);
  }
  int polls = 0;
  for (auto &h : air.heard)
    if (h->sent.kind == AIOT_R2D_UPPER_LAYER_DATA) {
      polls++;
      EXPECT_EQ(nacked.count(h->sent.as_id), 0u);
      EXPECT_EQ(h->n_answers, 1);
    }
  EXPECT_GT(polls, 6);
}

// Every Tag's data is heard: no NACK Feedback is sent.
TEST(Reader, RegistrationSendsNoNackWhenEveryDataIsHeard)
{
  Air air("registration", {{"occasions_exp", 3}}, 6);
  air.run(80);
  for (auto &h : air.heard)
    EXPECT_NE(h->sent.kind, AIOT_R2D_NACK_FEEDBACK);
}

TEST(Reader, Gen2QKeepsItsTable)
{
  Air air("gen2_q", {}, 8);
  air.run(60);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  const char *name;
  const char *const *cols;
  int nc;
  ASSERT_EQ(aiot_reader_table(air.reader, &name, &cols, &nc), 1);
  EXPECT_STREQ(name, "q.csv");
  ASSERT_EQ(nc, 4);
  std::vector<double> rows(64 * nc);
  const int n = aiot_reader_pop_rows(air.reader, rows.data(), 64);
  EXPECT_GT(n, 0);
  for (int i = 1; i < n; i++)
    EXPECT_GE(rows[i * nc], rows[(i - 1) * nc]); /* time */
  EXPECT_EQ(aiot_reader_pop_rows(air.reader, rows.data(), 64), 0);
}

TEST(Reader, AdaptiveAlohaKeepsItsTable)
{
  Air air("adaptive_aloha", {}, 8);
  air.run(60);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  const char *name;
  const char *const *cols;
  int nc;
  ASSERT_EQ(aiot_reader_table(air.reader, &name, &cols, &nc), 1);
  EXPECT_STREQ(name, "adaptation.csv");
  std::vector<double> rows(64 * nc);
  EXPECT_GT(aiot_reader_pop_rows(air.reader, rows.data(), 64), 0);
}

TEST(Reader, TwoSlotsOfOccasions)
{
  aiot_link_config_t c = default_link();
  c.t_bit_den = 2;
  c.n_r_sfs = 2;
  c.x = 2;
  Air air("broadcast", {{"occasions_exp", 3}}, 4, c);
  air.run(30);
  EXPECT_EQ(air.unmatched, 0);
  EXPECT_EQ(air.late_r2d, 0);
  bool slot1 = false;
  for (auto &h : air.heard)
    for (int i = 0; i < h->n_answers; i++)
      slot1 |= h->answers[i].kind == AIOT_ANSWER_MSG1 && h->answers[i].slot == 1;
  EXPECT_TRUE(slot1);
}

TEST(Reader, RejectsAnOutcomeItDidNotExpect)
{
  Air air("unicast", {}, 1);
  aiot_reader_r2d_t r2d;
  ASSERT_EQ(aiot_reader_next_r2d(air.reader, 0, &r2d), 1);
  aiot_d2r_expect_t exp[16];
  ASSERT_EQ(aiot_reader_r2d_sent(air.reader, 0, 1000, 1000, exp, 16), 1);
  EXPECT_EQ(aiot_reader_next_r2d(air.reader, 1000, &r2d), 0); /* waits for the answer */
  auto o = std::make_unique<aiot_d2r_outcome_t>();
  o->expect = exp[0];
  o->expect.tag += 1;
  EXPECT_LT(aiot_reader_d2r_outcome(air.reader, o.get(), 2000), 0);
  o->expect = exp[0];
  EXPECT_EQ(aiot_reader_d2r_outcome(air.reader, o.get(), 2000), 0);
  EXPECT_LT(aiot_reader_d2r_outcome(air.reader, o.get(), 2000), 0); /* already in */
  ASSERT_EQ(aiot_reader_next_r2d(air.reader, 0, &r2d), 1);
  EXPECT_GT(r2d.earliest_start, 1000); /* after the D2R that never came, and T_D2R_min */
}

// "manual" sends only what the caller pushes; a pushed access message still gets its Random ID
// Response from the Reader.
TEST(Reader, ManualSendsWhatIsPushed)
{
  Air air("manual", {}, 2);
  aiot_reader_r2d_t r2d;
  EXPECT_EQ(aiot_reader_next_r2d(air.reader, 0, &r2d), 0);
  EXPECT_EQ(aiot_reader_idle(air.reader), 1);
  EXPECT_EQ(aiot_reader_done(air.reader), 0);

  aiot_link_config_t c = default_link();
  aiot_mac_r2d_msg_t cfa = {};
  cfa.kind = AIOT_R2D_PAGING_CFA;
  ASSERT_EQ(aiot_link_cfa(&c, 0, 0, &cfa.sched), 0);
  cfa.has_paging_id = 1;
  cfa.paging_id_len = 16;
  aiot_paging_id_of_node(0x101, cfa.paging_id);
  EXPECT_LT(aiot_reader_push(air.reader, &cfa, 1), 0) << "a CFA paging is not an access message";
  ASSERT_EQ(aiot_reader_push(air.reader, &cfa, 0), 0);
  EXPECT_LT(aiot_reader_push(air.reader, &cfa, 0), 0) << "one at a time";
  EXPECT_EQ(air.run(5), 1);
  ASSERT_EQ(air.heard.size(), 1u);
  EXPECT_EQ(air.data_from(), (std::vector<int>{0x101}));
  EXPECT_EQ(aiot_reader_idle(air.reader), 1);

  aiot_mac_r2d_msg_t page = {};
  page.kind = AIOT_R2D_PAGING_CBRA;
  ASSERT_EQ(aiot_link_cbra(&c, nullptr, 0, 0, &page.sched), 0);
  page.transaction_id = 1;
  page.num_access_occasions_code = 3;
  ASSERT_EQ(aiot_reader_push(air.reader, &page, 1), 0);
  air.run(5);
  EXPECT_EQ(air.unmatched, 0);
  ASSERT_GE(air.heard.size(), 2u);
  EXPECT_EQ(air.heard[1]->sent.kind, AIOT_R2D_PAGING_CBRA);
  if (air.heard.size() > 2) {
    EXPECT_EQ(air.heard[2]->sent.kind, AIOT_R2D_RANDOM_ID_RESPONSE);
  }
}

namespace {

std::vector<uint8_t> security_of(const aiot_mac_r2d_msg_t &m)
{
  return std::vector<uint8_t>(m.security_param, m.security_param + AIOT_MAC_SECURITY_PARAM_BITS);
}

/* The first R2D of a Reader, asked for twice: both asks must give the same message and bits. */
aiot_reader_r2d_t first_r2d(aiot_reader_t *r)
{
  aiot_reader_r2d_t a, b;
  EXPECT_EQ(aiot_reader_next_r2d(r, 0, &a), 1);
  EXPECT_EQ(aiot_reader_next_r2d(r, 0, &b), 1);
  EXPECT_EQ(a.n_bits, b.n_bits);
  EXPECT_EQ(std::vector<uint8_t>(a.bits, a.bits + a.n_bits), std::vector<uint8_t>(b.bits, b.bits + b.n_bits));
  EXPECT_EQ(security_of(a.msg), security_of(b.msg));
  return a;
}

} // namespace

// Every paging the protocol builds carries SPPI 1 and 128 bits of the Reader's own generator: the
// bits in the packed R2D, in the message it reports, and in the record of what was sent.
TEST(Reader, ProtocolPagingsCarryASecurityParameter)
{
  Air air("broadcast", {{"occasions_exp", 2}}, 3);
  const aiot_reader_r2d_t first = first_r2d(air.reader);
  ASSERT_EQ(first.msg.kind, AIOT_R2D_PAGING_CBRA);
  EXPECT_EQ(first.msg.has_security_param, 1);
  aiot_mac_r2d_msg_t parsed;
  ASSERT_EQ(aiot_mac_parse_r2d(first.bits, first.n_bits, &parsed), 0);
  ASSERT_EQ(parsed.has_security_param, 1);
  EXPECT_EQ(security_of(parsed), security_of(first.msg));
  int ones = 0;
  for (uint8_t b : security_of(first.msg))
    ones += b;
  EXPECT_GT(ones, 32);
  EXPECT_LT(ones, 96);

  air.run(40);
  std::vector<std::vector<uint8_t>> sec;
  for (auto &h : air.heard)
    if (h->sent.kind == AIOT_R2D_PAGING_CBRA || h->sent.kind == AIOT_R2D_PAGING_CFA) {
      EXPECT_EQ(h->sent.has_security_param, 1);
      sec.push_back(security_of(h->sent));
    } else {
      EXPECT_EQ(h->sent.has_security_param, 0);
    }
  ASSERT_GE(sec.size(), 2u);
  EXPECT_EQ(sec[0], security_of(first.msg)) << "the record is what was packed";
  EXPECT_NE(sec[0], sec[1]);

  Air again("broadcast", {{"occasions_exp", 2}}, 3);
  EXPECT_EQ(security_of(first_r2d(again.reader).msg), security_of(first.msg));
  Air seeded("broadcast", {{"occasions_exp", 2}}, 3);
  aiot_reader_seed(seeded.reader, 7);
  const aiot_reader_r2d_t other = first_r2d(seeded.reader);
  EXPECT_EQ(other.msg.has_security_param, 1);
  EXPECT_NE(security_of(other.msg), security_of(first.msg));

  Air cfa("unicast", {}, 2);
  const aiot_reader_r2d_t page = first_r2d(cfa.reader);
  ASSERT_EQ(page.msg.kind, AIOT_R2D_PAGING_CFA);
  EXPECT_EQ(page.msg.has_security_param, 1);
}

// A message the caller pushes is sent as built: SPPI 0 stays 0, given bits stay as given.
TEST(Reader, PushedPagingKeepsItsSecurityParameter)
{
  Air air("manual", {}, 2);
  aiot_link_config_t c = default_link();
  aiot_mac_r2d_msg_t cfa = {};
  cfa.kind = AIOT_R2D_PAGING_CFA;
  ASSERT_EQ(aiot_link_cfa(&c, 0, 0, &cfa.sched), 0);
  cfa.has_paging_id = 1;
  cfa.paging_id_len = 16;
  aiot_paging_id_of_node(0x100, cfa.paging_id);

  ASSERT_EQ(aiot_reader_push(air.reader, &cfa, 0), 0);
  aiot_reader_r2d_t out = first_r2d(air.reader);
  EXPECT_EQ(out.msg.has_security_param, 0);
  aiot_mac_r2d_msg_t parsed;
  ASSERT_EQ(aiot_mac_parse_r2d(out.bits, out.n_bits, &parsed), 0);
  EXPECT_EQ(parsed.has_security_param, 0);
  ASSERT_TRUE(air.step());
  EXPECT_EQ(air.heard.back()->sent.has_security_param, 0);

  cfa.has_security_param = 1;
  for (int i = 0; i < AIOT_MAC_SECURITY_PARAM_BITS; i++)
    cfa.security_param[i] = i % 3 == 0;
  ASSERT_EQ(aiot_reader_push(air.reader, &cfa, 0), 0);
  out = first_r2d(air.reader);
  ASSERT_EQ(out.msg.has_security_param, 1);
  EXPECT_EQ(security_of(out.msg), security_of(cfa));
  ASSERT_EQ(aiot_mac_parse_r2d(out.bits, out.n_bits, &parsed), 0);
  ASSERT_EQ(parsed.has_security_param, 1);
  EXPECT_EQ(security_of(parsed), security_of(cfa));
  ASSERT_TRUE(air.step());
  EXPECT_EQ(security_of(air.heard.back()->sent), security_of(cfa));
}

// Each window gives one outcome. With two windows open (X = 2), a second outcome for the first
// must not stand in for the second: the R2D would close with a window still unheard.
TEST(Reader, RejectsASecondOutcomeForTheSameWindow)
{
  aiot_link_config_t c = default_link();
  c.t_bit_den = 2;
  c.n_r_sfs = 2;
  c.x = 2;
  Air air("broadcast", {{"occasions_exp", 3}}, 1, c);
  aiot_reader_r2d_t r2d;
  ASSERT_EQ(aiot_reader_next_r2d(air.reader, 0, &r2d), 1);
  aiot_d2r_expect_t exp[16];
  ASSERT_EQ(aiot_reader_r2d_sent(air.reader, 0, 1000, 1000, exp, 16), 2);
  auto o = std::make_unique<aiot_d2r_outcome_t>();
  o->expect = exp[0];
  EXPECT_EQ(aiot_reader_d2r_outcome(air.reader, o.get(), 2000), 0);
  EXPECT_LT(aiot_reader_d2r_outcome(air.reader, o.get(), 2000), 0) << "window 0 is already in";
  EXPECT_EQ(aiot_reader_next_r2d(air.reader, 2000, &r2d), 0) << "window 1 is still open";
  o->expect = exp[1];
  EXPECT_EQ(aiot_reader_d2r_outcome(air.reader, o.get(), 3000), 0);
  EXPECT_EQ(aiot_reader_next_r2d(air.reader, 3000, &r2d), 1);
}
