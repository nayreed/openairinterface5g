/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aiot_mac.h"

namespace {

using Bytes = std::vector<uint8_t>;

/* ---------------------------------------------------------------------------------------------
 * The device's upper layer and random numbers
 * ------------------------------------------------------------------------------------------- */

struct Upper {
  std::mt19937 own;
  std::mt19937 *rng; /* shared between devices of an inventory */
  std::deque<uint32_t> script; /* draws to return before the generator takes over */
  std::vector<std::pair<uint32_t, uint32_t>> draws; /* (n, value) in call order */
  std::deque<Bytes> sdus;
  bool has_pid = false; /* otherwise every Paging ID matches */
  std::vector<uint8_t> pid;
  std::vector<Bytes> pids_asked;
  std::vector<Bytes> got;
  int selected = 0;
  bool fail_integrity = false; /* deliver reports an integrity protection failure */
  bool pending = false; /* next_sdu has nothing yet, due to processing delay */
  int none_code = AIOT_UPPER_SDU_NONE; /* what next_sdu returns for no data */
  std::vector<Bytes> secs; /* Security Parameters forwarded, bits 0/1 */
  std::vector<std::string> events; /* "sec", "selected", "matched" in call order */

  explicit Upper(uint32_t seed = 0, std::mt19937 *shared = nullptr) : own(seed), rng(shared ? shared : &own)
  {
  }

  static int matched(void *u, const uint8_t *id, int len)
  {
    auto *s = static_cast<Upper *>(u);
    s->pids_asked.emplace_back(id, id + len);
    s->events.push_back("matched");
    return !s->has_pid || s->pids_asked.back() == s->pid;
  }
  static void select(void *u)
  {
    auto *s = static_cast<Upper *>(u);
    s->selected++;
    s->events.push_back("selected");
  }
  static int deliver(void *u, const uint8_t *sdu, int len)
  {
    auto *s = static_cast<Upper *>(u);
    s->got.emplace_back(sdu, sdu + len);
    return s->fail_integrity ? AIOT_UPPER_INTEGRITY_FAILURE : 0;
  }
  static void security(void *u, const uint8_t *bits, int n_bits)
  {
    auto *s = static_cast<Upper *>(u);
    s->secs.emplace_back(bits, bits + n_bits);
    s->events.push_back("sec");
  }
  static int next(void *u, uint8_t *sdu, int cap)
  {
    auto *s = static_cast<Upper *>(u);
    if (s->sdus.empty())
      return s->pending ? AIOT_UPPER_SDU_PENDING : s->none_code;
    Bytes b = s->sdus.front();
    s->sdus.pop_front();
    if ((int)b.size() > cap)
      return AIOT_UPPER_SDU_NONE;
    memcpy(sdu, b.data(), b.size());
    return (int)b.size();
  }
  static uint32_t draw(void *u, uint32_t n)
  {
    auto *s = static_cast<Upper *>(u);
    uint32_t v;
    if (!s->script.empty()) {
      v = s->script.front();
      s->script.pop_front();
    } else {
      v = std::uniform_int_distribution<uint32_t>(0, n - 1)(*s->rng);
    }
    s->draws.emplace_back(n, v);
    return v;
  }
  aiot_device_upper_t api()
  {
    aiot_device_upper_t a;
    a.user = this;
    a.paging_id_matched = matched;
    a.selected = select;
    a.deliver = deliver;
    a.next_sdu = next;
    a.draw = draw;
    a.security_param = security;
    return a;
  }
};

struct Dev {
  Upper u;
  aiot_device_t *d;
  explicit Dev(uint32_t seed = 0, aiot_mod_t mod = AIOT_BPSK, std::mt19937 *shared = nullptr) : u(seed, shared)
  {
    aiot_device_upper_t a = u.api();
    d = aiot_device_new(&a, mod);
  }
  ~Dev()
  {
    aiot_device_free(d);
  }
  Dev(const Dev &) = delete;
  int on(const aiot_mac_r2d_msg_t &m, aiot_device_send_t *s)
  {
    return aiot_device_on_r2d(d, &m, s);
  }
  aiot_device_state_t st() const
  {
    aiot_device_state_t s;
    aiot_device_state(d, &s);
    return s;
  }
};

/* ---------------------------------------------------------------------------------------------
 * Scheduling Infos and R2D messages, built by hand
 * ------------------------------------------------------------------------------------------- */

aiot_sched_info_t sched(aiot_sched_variant_t v, std::vector<int> r_sfs, int tbs, int x, int t_num = 2, int t_den = 1)
{
  aiot_sched_info_t s;
  memset(&s, 0, sizeof(s));
  s.variant = v;
  s.t_bit_num = t_num;
  s.t_bit_den = t_den;
  s.n_sfs = (int)r_sfs.size();
  for (size_t i = 0; i < r_sfs.size(); i++)
    s.r_sfs[i] = r_sfs[i];
  s.x = x;
  s.r_block = 1;
  s.r_code = 1;
  s.i_bit = 0;
  s.l_amble = 31;
  s.i_add = 0;
  s.d2r_tbs = tbs;
  return s;
}
aiot_sched_info_t cbra_sched(std::vector<int> r_sfs = {8}, int x = 1)
{
  return sched(AIOT_SCHED_PAGING_CBRA, r_sfs, 0, x);
}
aiot_sched_info_t ridr_sched(std::vector<int> r_sfs = {1, 2, 4, 8, 16, 32, 64, 128}, int tbs = 12)
{
  return sched(AIOT_SCHED_RANDOM_ID_RESPONSE, r_sfs, tbs, 0);
}
aiot_sched_info_t data_sched(int r_sfs = 8, int tbs = 12)
{
  return sched(AIOT_SCHED_R2D_DATA, {r_sfs}, tbs, 0);
}

/* SPPI 1 and the bits, when given (128 of them, 0/1) */
void set_security(aiot_mac_r2d_msg_t &m, const Bytes *sec)
{
  if (!sec)
    return;
  ASSERT_EQ((size_t)AIOT_MAC_SECURITY_PARAM_BITS, sec->size());
  m.has_security_param = 1;
  memcpy(m.security_param, sec->data(), sec->size());
}

aiot_mac_r2d_msg_t paging_cbra(int tid,
                               const aiot_sched_info_t &s,
                               int code = 0,
                               int k_bit = 0,
                               const Bytes *pid = nullptr,
                               const Bytes *sec = nullptr)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_PAGING_CBRA;
  m.sched = s;
  m.transaction_id = tid;
  m.num_access_occasions_code = code;
  m.k_bit = k_bit;
  if (pid) {
    m.has_paging_id = 1;
    m.paging_id_len = (int)pid->size();
    memcpy(m.paging_id, pid->data(), pid->size());
  }
  set_security(m, sec);
  return m;
}
aiot_mac_r2d_msg_t paging_cfa(const Bytes &pid, const aiot_sched_info_t &s, const Bytes *sec = nullptr)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_PAGING_CFA;
  m.sched = s;
  m.has_paging_id = 1;
  m.paging_id_len = (int)pid.size();
  memcpy(m.paging_id, pid.data(), pid.size());
  set_security(m, sec);
  return m;
}
aiot_mac_r2d_msg_t trigger()
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_ACCESS_TRIGGER;
  return m;
}
aiot_mac_r2d_msg_t data_sdu(int as_id, const aiot_sched_info_t &s, const Bytes &sdu)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_UPPER_LAYER_DATA;
  m.sched = s;
  m.as_id = as_id;
  m.has_sdu = 1;
  m.sdu_len = (int)sdu.size();
  std::copy(sdu.begin(), sdu.end(), m.sdu);
  return m;
}
aiot_mac_r2d_msg_t data_received(int as_id, const aiot_sched_info_t &s, int received)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_UPPER_LAYER_DATA;
  m.sched = s;
  m.as_id = as_id;
  m.has_sdu = 0;
  m.received_data_size = received;
  return m;
}
aiot_mac_r2d_msg_t nack(std::vector<int> ids)
{
  aiot_mac_r2d_msg_t m;
  memset(&m, 0, sizeof(m));
  m.kind = AIOT_R2D_NACK_FEEDBACK;
  m.n_as_ids = (int)ids.size();
  for (size_t i = 0; i < ids.size(); i++)
    m.as_ids[i] = ids[i];
  return m;
}

struct Heard {
  int random_id, r_sfs, slot;
};

int freq_index_of(int r_sfs)
{
  for (int i = 0; i < 8; i++)
    if (aiot_mac_freq_index_to_r_sfs(i) == r_sfs)
      return i;
  return -1;
}

/* The Reader's AS ID bookkeeping, as far as these tests need it. */
struct TestReader {
  int next_as_id = 1;
  aiot_mac_r2d_msg_t rid_response(const std::vector<Heard> &heard,
                                  const aiot_sched_info_t &s,
                                  bool assign = true,
                                  bool with_freq = true)
  {
    aiot_mac_r2d_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = AIOT_R2D_RANDOM_ID_RESPONSE;
    m.sched = s;
    m.n_entries = (int)std::min<size_t>(heard.size(), AIOT_MAC_MAX_ID_ENTRIES);
    for (int i = 0; i < m.n_entries; i++) {
      aiot_mac_rid_entry_t &e = m.entries[i];
      e.echoed_random_id = heard[i].random_id;
      e.assigned_as_id = -1;
      if (assign) {
        e.assigned_as_id = next_as_id;
        next_as_id = (next_as_id + 1) & 0xFFFF ? (next_as_id + 1) & 0xFFFF : 1;
      }
      e.freq_index = with_freq ? freq_index_of(heard[i].r_sfs) : -1;
    }
    return m;
  }
};

Heard heard_of(const aiot_device_send_t &s)
{
  return Heard{s.message.random_id, s.params.r_sfs, s.slot};
}

/* The R2D message through its bits, as on the air. */
aiot_mac_r2d_msg_t through_bits(const aiot_mac_r2d_msg_t &m)
{
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  int n = aiot_mac_pack_r2d(&m, bits, sizeof(bits));
  EXPECT_GT(n, 0);
  aiot_mac_r2d_msg_t out;
  EXPECT_EQ(0, aiot_mac_parse_r2d(bits, n, &out));
  return out;
}

struct Upl {
  Bytes sdu;
  int mdi, padding, n_bits;
};
Upl parse_upl(const aiot_device_send_t &s)
{
  aiot_mac_d2r_msg_t m;
  EXPECT_EQ(0, aiot_mac_parse_d2r(s.bits, s.n_bits, AIOT_D2R_UPPER_LAYER_DATA, s.tbs_bytes, &m));
  return Upl{Bytes(m.sdu, m.sdu + m.sdu_len), m.mdi, m.padding_bytes, s.n_bits};
}

Bytes iota_bytes(int n)
{
  Bytes b(n);
  for (int i = 0; i < n; i++)
    b[i] = (uint8_t)i;
  return b;
}

/* (time slot, R_SFS) of access occasion k of a Scheduling Info (the module's reading of 5.3.1.1). */
std::pair<int, int> occasion_of(const aiot_sched_info_t &s, int k)
{
  return {k / s.n_sfs, s.r_sfs[k % s.n_sfs]};
}

/* CBRA up to a Msg2 answered with an assigned AS ID; the device holds AS ID first_as_id. */
void cbra_to_msg2(Dev &d, TestReader &r, int tid, aiot_device_send_t *msg2, const aiot_sched_info_t &rs = ridr_sched())
{
  aiot_device_send_t s1;
  ASSERT_EQ(1, d.on(paging_cbra(tid, cbra_sched()), &s1));
  ASSERT_EQ(1, d.on(r.rid_response({heard_of(s1)}, rs), msg2));
}

/* The same with K = 4 (the Msg2 window is 4 Access Triggers). */
void cbra_to_msg2_k4(Dev &d, TestReader &r, int tid, aiot_device_send_t *msg2)
{
  aiot_device_send_t s1;
  ASSERT_EQ(1, d.on(paging_cbra(tid, cbra_sched(), 0, 1), &s1));
  ASSERT_EQ(1, d.on(r.rid_response({heard_of(s1)}, ridr_sched()), msg2));
}

/* 128 Security Parameter bits: every third one set, shifted by off. */
Bytes sec_bits(int off = 0)
{
  Bytes b(AIOT_MAC_SECURITY_PARAM_BITS);
  for (size_t i = 0; i < b.size(); i++)
    b[i] = (i + off) % 3 == 0;
  return b;
}

/* ---------------------------------------------------------------------------------------------
 * Ported from tests/test_mac_procedures.py
 * ------------------------------------------------------------------------------------------- */

TEST(DeviceMac, NewRejectsMissingDraw)
{
  aiot_device_upper_t a;
  memset(&a, 0, sizeof(a));
  EXPECT_EQ(nullptr, aiot_device_new(&a, AIOT_BPSK));
  EXPECT_EQ(nullptr, aiot_device_new(nullptr, AIOT_BPSK));
}

TEST(DeviceMac, InitialState)
{
  Dev d;
  aiot_device_state_t s = d.st();
  EXPECT_EQ(-1, s.transaction_id);
  EXPECT_EQ(-1, s.as_id);
  EXPECT_EQ(-1, s.failed_tid);
  EXPECT_EQ(0, s.cbra_active);
  EXPECT_EQ(0, s.cbra_success);
  EXPECT_EQ(-1, s.counter);
  EXPECT_EQ(-1, s.random_id);
  EXPECT_EQ(0, s.msg2_budget);
  EXPECT_EQ(0, s.cfa_in_procedure);
  EXPECT_EQ(0, s.nack_monitoring);
  EXPECT_EQ(-1, s.sending_len);
}

TEST(DeviceMac, CbraOneDeviceEndToEnd)
{
  Dev d(0);
  TestReader r;
  d.u.sdus.push_back(Bytes{'h', 'e', 'l', 'l', 'o'});
  aiot_device_send_t s;
  ASSERT_EQ(1, d.on(paging_cbra(7, cbra_sched()), &s));
  EXPECT_EQ(AIOT_MSG1, s.msg);
  EXPECT_EQ(8, s.params.r_sfs);
  EXPECT_EQ(0, s.slot);
  EXPECT_EQ(16, s.n_bits);
  EXPECT_EQ(2, s.tbs_bytes);
  EXPECT_EQ(AIOT_D2R_ACCESS_RANDOM_ID, s.message.kind);
  /* the PDU is the Random ID, MSB first */
  int rid = 0;
  for (int i = 0; i < 16; i++)
    rid = rid << 1 | s.bits[i];
  EXPECT_EQ(s.message.random_id, rid);
  /* draws, in order: the access occasion counter in [0, 1), then the 16-bit Random ID */
  ASSERT_EQ(2u, d.u.draws.size());
  EXPECT_EQ(1u, d.u.draws[0].first);
  EXPECT_EQ(65536u, d.u.draws[1].first);
  EXPECT_EQ((uint32_t)rid, d.u.draws[1].second);

  aiot_mac_r2d_msg_t ridr = through_bits(r.rid_response({heard_of(s)}, ridr_sched()));
  aiot_device_send_t m3;
  ASSERT_EQ(1, d.on(ridr, &m3));
  EXPECT_EQ(1, d.st().cbra_success);
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(AIOT_MSG2, m3.msg);
  EXPECT_EQ(1, m3.params.r_sfs);
  EXPECT_EQ(12, m3.tbs_bytes);
  EXPECT_EQ(96, m3.n_bits);
  Upl u = parse_upl(m3);
  EXPECT_EQ((Bytes{'h', 'e', 'l', 'l', 'o'}), u.sdu);
  EXPECT_EQ(0, u.mdi);
  EXPECT_EQ(5, u.padding);
  /* the same Transaction ID again: already done, the device stays quiet and draws nothing */
  EXPECT_EQ(0, d.on(paging_cbra(7, cbra_sched()), &s));
  EXPECT_EQ(2u, d.u.draws.size());
  /* a new one starts a fresh access and drops the AS ID */
  ASSERT_EQ(1, d.on(paging_cbra(8, cbra_sched()), &s));
  EXPECT_EQ(AIOT_MSG1, s.msg);
  EXPECT_EQ(-1, d.st().as_id);
  EXPECT_EQ(8, d.st().transaction_id);
}

TEST(DeviceMac, TRtoDOffsetOfWhatTheMacAsksFor)
{
  Dev d(1);
  TestReader r;
  d.u.sdus.push_back(Bytes(9, 'x'));
  aiot_device_send_t s1, s3;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched({4, 8}, 2), 2), &s1));
  ASSERT_EQ(1, d.on(r.rid_response({heard_of(s1)}, ridr_sched()), &s3));
  for (const aiot_device_send_t *s : {&s1, &s3}) {
    int n = aiot_device_send_offset(s, 6);
    EXPECT_GT(n, 0);
    int frame = aiot_frame_bits(8 * s->tbs_bytes, &s->params);
    EXPECT_EQ(aiot_t_r_to_d(&s->params,
                            6,
                            s->msg,
                            s->tbs_bytes,
                            s->candidates,
                            s->n_candidates,
                            s->slot > 0,
                            frame * 2 * s->params.r_sfs),
              n);
  }
}

class CountDown : public ::testing::TestWithParam<int> {};

/* n = 16 occasions, m = X * N_SFS = 4 per R2D: counter c answers the (c / 4)-th Access Trigger
 * (0 = the paging itself) on occasion c % 4. */
TEST_P(CountDown, CounterAnswersItsTrigger)
{
  Dev d(GetParam());
  aiot_sched_info_t sc = cbra_sched({2, 8}, 2);
  std::vector<aiot_device_send_t> out(4);
  std::vector<int> rc(4);
  rc[0] = d.on(paging_cbra(1, sc, 4), &out[0]);
  ASSERT_EQ(16u, d.u.draws[0].first);
  int c = (int)d.u.draws[0].second;
  for (int i = 1; i < 4; i++)
    rc[i] = d.on(trigger(), &out[i]);
  std::vector<int> sent;
  for (int i = 0; i < 4; i++) {
    ASSERT_GE(rc[i], 0);
    if (rc[i])
      sent.push_back(i);
  }
  ASSERT_EQ(std::vector<int>{c / 4}, sent);
  auto occ = occasion_of(sc, c % 4);
  EXPECT_EQ(occ.first, out[c / 4].slot);
  EXPECT_EQ(occ.second, out[c / 4].params.r_sfs);
  EXPECT_EQ(2, out[c / 4].n_candidates);
  EXPECT_EQ(2, out[c / 4].candidates[0]);
  EXPECT_EQ(8, out[c / 4].candidates[1]);
  EXPECT_EQ(2u, d.u.draws.size()); /* one counter draw, one Random ID draw */
}
INSTANTIATE_TEST_SUITE_P(Seeds, CountDown, ::testing::Range(0, 12));

TEST(DeviceMac, CountDownEveryCounterValue)
{
  /* scripted counters 0..15: this pins the occasion numbering, whatever the generator does */
  for (int c = 0; c < 16; c++) {
    Dev d;
    d.u.script = {(uint32_t)c, 0x1234};
    aiot_sched_info_t sc = cbra_sched({2, 8}, 2);
    aiot_device_send_t s;
    int rc = d.on(paging_cbra(1, sc, 4), &s);
    for (int t = 0; t < c / 4; t++) {
      EXPECT_EQ(0, rc) << "c=" << c << " before trigger " << t;
      EXPECT_EQ(c - 4 * (t), d.st().counter);
      rc = d.on(trigger(), &s);
    }
    ASSERT_EQ(1, rc) << "c=" << c;
    auto occ = occasion_of(sc, c % 4);
    EXPECT_EQ(occ.first, s.slot) << "c=" << c;
    EXPECT_EQ(occ.second, s.params.r_sfs) << "c=" << c;
    EXPECT_EQ(0x1234, s.message.random_id);
    EXPECT_EQ(-1, d.st().counter);
  }
}

TEST(DeviceMac, CounterSpreadsOverAllOccasions)
{
  std::map<int, int> seen;
  for (int seed = 0; seed < 400; seed++) {
    Dev d(seed);
    aiot_device_send_t s;
    d.on(paging_cbra(1, cbra_sched(), 3), &s);
    ASSERT_EQ(8u, d.u.draws[0].first);
    seen[d.u.draws[0].second]++;
  }
  ASSERT_EQ(8u, seen.size());
  for (auto &kv : seen)
    EXPECT_GT(kv.second, 25) << "occasion " << kv.first;
}

TEST(DeviceMac, CounterBeyondTheOccasionsStaysSilentUntilItsTrigger)
{
  Dev d;
  d.u.script = {9, 7};
  aiot_device_send_t s;
  EXPECT_EQ(0, d.on(paging_cbra(1, cbra_sched({2, 8}, 2), 4), &s));
  EXPECT_EQ(9, d.st().counter);
  EXPECT_EQ(1u, d.u.draws.size()); /* no Random ID until Msg1 is sent */
  EXPECT_EQ(-1, d.st().random_id);
  EXPECT_EQ(0, d.on(trigger(), &s));
  EXPECT_EQ(5, d.st().counter); /* 9 - 4: still not among the 4 occasions of this R2D */
  ASSERT_EQ(1, d.on(trigger(), &s)); /* 5 - 4 = 1 < 4: occasion 1 */
  EXPECT_EQ(-1, d.st().counter);
  EXPECT_EQ(AIOT_MSG1, s.msg);
  EXPECT_EQ(7, s.message.random_id);
  EXPECT_EQ(0, s.slot);
  EXPECT_EQ(8, s.params.r_sfs);
  /* after Msg1 a further trigger is no access: it only uses up the Msg2 window */
  EXPECT_EQ(0, d.on(trigger(), &s));
  EXPECT_EQ(2u, d.u.draws.size());
}

TEST(DeviceMac, Msg2WindowIsKTriggers)
{
  /* 5.3.1.3: after K Access Triggers (or pagings) the device stops listening for Msg2 */
  for (int k_bit = 0; k_bit < 2; k_bit++) {
    int k = k_bit ? 4 : 1;
    for (int quiet = 0; quiet < 2; quiet++) {
      Dev d(3);
      TestReader r;
      aiot_device_send_t s, x;
      ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched(), 0, k_bit), &s));
      EXPECT_EQ(k, d.st().msg2_budget);
      int n_trig = quiet ? k : k - 1;
      for (int i = 0; i < n_trig; i++)
        EXPECT_EQ(0, d.on(trigger(), &x));
      EXPECT_EQ(k - n_trig, d.st().msg2_budget);
      int rc = d.on(r.rid_response({heard_of(s)}, ridr_sched()), &x);
      EXPECT_EQ(quiet ? 0 : 1, rc) << "K=" << k;
      EXPECT_EQ(quiet ? 0 : 1, d.st().cbra_success);
    }
  }
}

TEST(DeviceMac, TriggerAfterSuccessUsesUpTheWindow)
{
  /* 5.3.1.3: the K Access Triggers are counted from the Access Random ID message, success or not */
  Dev d(2);
  TestReader r;
  aiot_device_send_t s, x;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched(), 0, 1), &s));
  ASSERT_EQ(1, d.on(r.rid_response({heard_of(s)}, ridr_sched()), &x));
  EXPECT_EQ(4, d.st().msg2_budget);
  EXPECT_EQ(0, d.on(trigger(), &x));
  EXPECT_EQ(3, d.st().msg2_budget);
}

TEST(DeviceMac, Msg2WindowAppliesWithAStoredAsId)
{
  /* a device that already has its AS ID listens for further Random ID Responses in the window only */
  for (int in_window = 0; in_window < 2; in_window++) {
    Dev d(2);
    TestReader r;
    aiot_device_send_t m3, x;
    cbra_to_msg2(d, r, 1, &m3); /* K = 1 */
    ASSERT_EQ(1, d.st().as_id);
    if (!in_window) {
      EXPECT_EQ(0, d.on(trigger(), &x)); /* the one Access Trigger of the window */
      EXPECT_EQ(0, d.st().msg2_budget);
    }
    aiot_mac_r2d_msg_t again = r.rid_response({Heard{0, 0, 0}}, ridr_sched(), false, false);
    again.entries[0].echoed_random_id = 1; /* the stored AS ID */
    EXPECT_EQ(in_window ? 1 : 0, d.on(again, &x)) << "in_window=" << in_window;
  }
}

TEST(DeviceMac, Msg2WindowEndsAtR2dDataForTheDevice)
{
  Dev d(2);
  TestReader r;
  aiot_device_send_t m3, x;
  d.u.sdus = {Bytes{'a'}, Bytes{'b'}};
  cbra_to_msg2_k4(d, r, 1, &m3);
  ASSERT_EQ(1, d.st().as_id);
  EXPECT_EQ(4, d.st().msg2_budget);
  /* data for another AS ID leaves the window as it is */
  EXPECT_EQ(0, d.on(data_sdu(9, data_sched(), Bytes{'x'}), &x));
  EXPECT_EQ(4, d.st().msg2_budget);
  /* data for the device ends it */
  ASSERT_EQ(1, d.on(data_sdu(1, data_sched(), Bytes{'y'}), &x));
  EXPECT_EQ(0, d.st().msg2_budget);
  aiot_mac_r2d_msg_t again = r.rid_response({Heard{0, 0, 0}}, ridr_sched(), false, false);
  again.entries[0].echoed_random_id = 1;
  EXPECT_EQ(0, d.on(again, &x));
  /* also for a Received Data Size */
  Dev e(2);
  TestReader r2;
  e.u.sdus = {Bytes{'a'}, Bytes{'b'}};
  cbra_to_msg2_k4(e, r2, 1, &m3);
  ASSERT_EQ(1, e.on(data_received(1, data_sched(), 0), &x));
  EXPECT_EQ(0, e.st().msg2_budget);
}

TEST(DeviceMac, Msg2WindowEndsAtANackNamingTheDevice)
{
  Dev d(2);
  TestReader r;
  aiot_device_send_t s1, m3, x;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched(), 0, 1), &s1));
  ASSERT_EQ(1, d.on(r.rid_response({heard_of(s1)}, ridr_sched()), &m3));
  ASSERT_EQ(1, d.st().as_id);
  EXPECT_EQ(4, d.st().msg2_budget);
  /* a NACK for somebody else leaves the window as it is */
  EXPECT_EQ(0, d.on(nack({7}), &x));
  EXPECT_EQ(4, d.st().msg2_budget);
  /* one naming the device ends it, and releases the AS ID */
  EXPECT_EQ(0, d.on(nack({7, 1}), &x));
  EXPECT_EQ(0, d.st().msg2_budget);
  EXPECT_EQ(-1, d.st().as_id);
  /* the old Random ID is not answered any more */
  aiot_mac_r2d_msg_t old = r.rid_response({heard_of(s1)}, ridr_sched());
  EXPECT_EQ(0, d.on(old, &x));
  EXPECT_EQ(-1, d.st().as_id);
}

TEST(DeviceMac, NackNamingTheDeviceEndsTheWindowWithoutNackMonitoring)
{
  /* 5.3.1.3 and 5.5.2 are separate: with the monitoring over (R2D data), a NACK keeps the AS ID */
  Dev d(2);
  TestReader r;
  aiot_device_send_t m3, x;
  cbra_to_msg2_k4(d, r, 1, &m3);
  ASSERT_EQ(1, d.on(data_sdu(1, data_sched(), Bytes{'y'}), &x));
  EXPECT_EQ(0, d.st().nack_monitoring);
  EXPECT_EQ(0, d.on(nack({1}), &x));
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(0, d.st().msg2_budget);
}

TEST(DeviceMac, Msg2MatchingRules)
{
  Dev d(4);
  TestReader r;
  aiot_device_send_t s, m3;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched({4, 8})), &s));
  int rid = s.message.random_id, sfs = s.params.r_sfs;
  int other = sfs == 8 ? 4 : 8;
  /* right ID, wrong Frequency Index: not mine */
  EXPECT_EQ(0, d.on(r.rid_response({Heard{rid, other, 0}}, ridr_sched()), &m3));
  EXPECT_EQ(0, d.st().cbra_success);
  /* someone else's ID: not mine */
  EXPECT_EQ(0, d.on(r.rid_response({Heard{rid ^ 1, sfs, 0}}, ridr_sched()), &m3));
  /* FIPI = 0, no AS ID assigned: match on the ID alone, and the AS ID is the Random ID */
  ASSERT_EQ(1, d.on(r.rid_response({Heard{rid ^ 1, sfs, 0}, Heard{rid, sfs, 0}}, ridr_sched(), false, false), &m3));
  EXPECT_EQ(rid, d.st().as_id);
  EXPECT_EQ(2, m3.params.r_sfs); /* second entry -> second bitmap value */
  /* a later Random ID Response naming the stored AS ID asks for the data again */
  aiot_mac_r2d_msg_t again;
  memset(&again, 0, sizeof(again));
  again.kind = AIOT_R2D_RANDOM_ID_RESPONSE;
  again.sched = ridr_sched();
  again.n_entries = 1;
  again.entries[0] = {rid, -1, -1};
  EXPECT_EQ(1, d.on(again, &m3));
}

TEST(DeviceMac, Fipi0AndAipiCombinations)
{
  aiot_device_send_t s, m3;
  /* FIPI = 0 ignores the R_SFS altogether; an assigned AS ID is taken over the Random ID */
  {
    Dev d(11);
    TestReader r;
    r.next_as_id = 0x0BEE;
    ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched({4, 8})), &s));
    int rid = s.message.random_id, other = s.params.r_sfs == 8 ? 4 : 8;
    ASSERT_EQ(1, d.on(r.rid_response({Heard{rid, other, 0}}, ridr_sched(), true, false), &m3));
    EXPECT_EQ(0x0BEE, d.st().as_id);
    EXPECT_EQ(1, d.st().cbra_success);
    EXPECT_EQ(1, d.st().nack_monitoring);
  }
  /* FIPI = 1 with the right R_SFS matches */
  {
    Dev d(11);
    TestReader r;
    ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched({4, 8})), &s));
    ASSERT_EQ(1, d.on(r.rid_response({Heard{s.message.random_id, s.params.r_sfs, 0}}, ridr_sched(), false, true), &m3));
    EXPECT_EQ(s.message.random_id, d.st().as_id);
  }
}

TEST(DeviceMac, Msg2NeedsAnAccessInProgress)
{
  TestReader r;
  aiot_device_send_t x;
  /* nothing was paged */
  {
    Dev d;
    EXPECT_EQ(0, d.on(r.rid_response({Heard{5, 8, 0}}, ridr_sched()), &x));
    EXPECT_EQ(0, d.st().cbra_success);
  }
  /* Msg1 not yet sent: the device cannot know a Random ID */
  {
    Dev d;
    d.u.script = {3, 5};
    ASSERT_EQ(0, d.on(paging_cbra(1, cbra_sched(), 2), &x));
    EXPECT_EQ(0, d.on(r.rid_response({Heard{5, 8, 0}}, ridr_sched()), &x));
    EXPECT_EQ(0, d.st().cbra_success);
    EXPECT_EQ(-1, d.st().as_id);
  }
}

TEST(DeviceMac, Msg2EntryWithoutRsfsIsAnError)
{
  Dev d(5);
  TestReader r;
  aiot_device_send_t s, m3;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched()), &s));
  /* the bitmap indicates one R_SFS, the device is the second entry */
  EXPECT_EQ(AIOT_EINVAL,
            d.on(r.rid_response({Heard{(s.message.random_id ^ 1) & 0xFFFF, 8, 0}, heard_of(s)}, ridr_sched({4})), &m3));
}

TEST(DeviceMac, FailedCbraRetriesSameTransaction)
{
  /* 5.5.3 then 5.2: no Msg2 before the next paging means failure, so the same TID starts again */
  Dev d(5);
  aiot_device_send_t s;
  ASSERT_EQ(1, d.on(paging_cbra(9, cbra_sched()), &s));
  EXPECT_EQ(9, d.st().transaction_id);
  EXPECT_EQ(-1, d.st().failed_tid);
  ASSERT_EQ(1, d.on(paging_cbra(9, cbra_sched()), &s));
  EXPECT_EQ(AIOT_MSG1, s.msg);
  EXPECT_EQ(4u, d.u.draws.size()); /* a whole new access: counter and Random ID again */
  EXPECT_EQ(-1, d.st().failed_tid);
  EXPECT_EQ(1, d.st().cbra_active);
}

TEST(DeviceMac, FailedCbraBeforeMsg1StillCountsAsFailed)
{
  /* the device never found its occasion: the CBRA has not succeeded, so TID 3 may start again */
  Dev d;
  d.u.script = {7, 0, 0x4321};
  aiot_device_send_t s;
  EXPECT_EQ(0, d.on(paging_cbra(3, cbra_sched(), 3), &s));
  EXPECT_EQ(7, d.st().counter);
  ASSERT_EQ(1, d.on(paging_cbra(3, cbra_sched(), 3), &s)); /* counter 0 now */
  EXPECT_EQ(0x4321, s.message.random_id);
  EXPECT_EQ(-1, d.st().counter);
}

TEST(DeviceMac, SuccessfulCbraIsNotFailedByNextPaging)
{
  Dev d(5);
  TestReader r;
  aiot_device_send_t m3;
  cbra_to_msg2(d, r, 9, &m3);
  aiot_device_send_t s;
  EXPECT_EQ(0, d.on(paging_cbra(9, cbra_sched()), &s)); /* TID done: ignored */
  EXPECT_EQ(-1, d.st().failed_tid);
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(0, d.st().cbra_active);
  EXPECT_EQ(0, d.st().msg2_budget);
}

TEST(DeviceMac, NackMakesTheDeviceRetry)
{
  Dev d(6);
  TestReader r;
  d.u.sdus = {Bytes{'a'}, Bytes{'b'}};
  aiot_device_send_t m3, s;
  cbra_to_msg2(d, r, 2, &m3);
  EXPECT_EQ(1, d.st().nack_monitoring);
  EXPECT_EQ(0, d.on(nack({0x7777}), &s)); /* someone else */
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(1, d.st().nack_monitoring);
  EXPECT_EQ(0, d.on(nack({0x7777, 1}), &s));
  EXPECT_EQ(-1, d.st().as_id);
  EXPECT_EQ(2, d.st().failed_tid);
  EXPECT_EQ(2, d.st().transaction_id);
  EXPECT_EQ(0, d.st().nack_monitoring);
  ASSERT_EQ(1, d.on(paging_cbra(2, cbra_sched()), &s));
  EXPECT_EQ(AIOT_MSG1, s.msg);
  EXPECT_EQ(-1, d.st().failed_tid);
}

TEST(DeviceMac, NackIgnoredAfterR2dData)
{
  Dev d(7);
  TestReader r;
  d.u.sdus = {Bytes{'a'}, Bytes{'b'}};
  aiot_device_send_t m3, s;
  cbra_to_msg2(d, r, 2, &m3);
  EXPECT_EQ(1, d.on(data_sdu(1, data_sched(), Bytes{'c', 'm', 'd'}), &s));
  EXPECT_EQ(0, d.st().nack_monitoring);
  EXPECT_EQ(0, d.on(nack({1}), &s));
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(std::vector<Bytes>{(Bytes{'c', 'm', 'd'})}, d.u.got);
}

TEST(DeviceMac, NackIgnoredBeforeMsg2AndAfterAPaging)
{
  TestReader r;
  aiot_device_send_t s, m3;
  {
    /* before Msg2 the device holds no AS ID and monitors nothing */
    Dev d(7);
    ASSERT_EQ(1, d.on(paging_cbra(2, cbra_sched()), &s));
    EXPECT_EQ(0, d.st().nack_monitoring);
    EXPECT_EQ(0, d.on(nack({0, 1, 2, 3}), &s));
    EXPECT_EQ(-1, d.st().failed_tid);
    EXPECT_EQ(1, d.st().cbra_active);
  }
  {
    /* 5.5.2: the monitoring ends at a paging, even one the device ignores */
    Dev d(7);
    TestReader r2;
    cbra_to_msg2(d, r2, 2, &m3);
    EXPECT_EQ(0, d.on(paging_cbra(2, cbra_sched()), &s));
    EXPECT_EQ(0, d.st().nack_monitoring);
    EXPECT_EQ(0, d.on(nack({1}), &s));
    EXPECT_EQ(1, d.st().as_id);
    EXPECT_EQ(-1, d.st().failed_tid);
  }
}

TEST(DeviceMac, NackWithAFurtherRandomIdResponseKeepsMonitoring)
{
  /* a second Random ID Response naming the device's AS ID is a new D2R, monitoring starts again */
  Dev d(8);
  TestReader r;
  aiot_device_send_t m3, s;
  cbra_to_msg2(d, r, 2, &m3);
  aiot_mac_r2d_msg_t again = r.rid_response({Heard{0, 0, 0}}, ridr_sched(), false, false);
  again.entries[0].echoed_random_id = 1; /* the stored AS ID */
  ASSERT_EQ(1, d.on(again, &s));
  EXPECT_EQ(AIOT_MSG2, s.msg);
  EXPECT_EQ(1, d.st().nack_monitoring);
}

TEST(DeviceMac, Cfa)
{
  Bytes pid = {1, 0, 1, 1, 0, 0, 1, 0};
  Dev d(8);
  d.u.has_pid = true;
  d.u.pid = pid;
  d.u.sdus = {Bytes{'m', 'e', 't', 'e', 'r', '=', '4', '2'}};
  aiot_sched_info_t cfa = sched(AIOT_SCHED_PAGING_CFA, {16}, 10, 0, 1, 1);
  aiot_device_send_t s;
  EXPECT_EQ(0, d.on(paging_cfa(Bytes(8, 0), cfa), &s)); /* not my Paging ID */
  EXPECT_EQ(0, d.st().cfa_in_procedure);
  ASSERT_EQ(1u, d.u.pids_asked.size());
  EXPECT_EQ(Bytes(8, 0), d.u.pids_asked[0]);
  EXPECT_EQ(0u, d.u.draws.size());
  ASSERT_EQ(1, d.on(paging_cfa(pid, cfa), &s));
  EXPECT_EQ(pid, d.u.pids_asked[1]);
  EXPECT_EQ(AIOT_CFA, s.msg);
  EXPECT_EQ(16, s.params.r_sfs);
  EXPECT_EQ(10, s.tbs_bytes);
  EXPECT_EQ(0, s.slot);
  EXPECT_EQ(0, s.n_candidates);
  EXPECT_EQ(80, s.n_bits);
  EXPECT_EQ(1, d.st().cfa_in_procedure);
  Upl u = parse_upl(s);
  EXPECT_EQ((Bytes{'m', 'e', 't', 'e', 'r', '=', '4', '2'}), u.sdu);
  EXPECT_EQ(0, u.padding);
  EXPECT_GT(aiot_device_send_offset(&s, 6), 0);
  /* 5.4.3: with no AS ID after CFA, the first R2D data addressed to anyone gives the AS ID */
  d.u.sdus = {Bytes{'o', 'k'}};
  ASSERT_EQ(1, d.on(data_sdu(0x0BEE, data_sched(), Bytes{'s', 'e', 't', ' ', '1'}), &s));
  EXPECT_EQ(0x0BEE, d.st().as_id);
  EXPECT_EQ(AIOT_DATA, s.msg);
  EXPECT_EQ(std::vector<Bytes>{(Bytes{'s', 'e', 't', ' ', '1'})}, d.u.got);
  EXPECT_EQ((Bytes{'o', 'k'}), parse_upl(s).sdu);
}

TEST(DeviceMac, CfaForgetsTheCbraTransaction)
{
  Dev d(8);
  TestReader r;
  aiot_device_send_t m3, s;
  cbra_to_msg2(d, r, 5, &m3);
  ASSERT_EQ(1, d.on(paging_cfa(Bytes(8, 1), sched(AIOT_SCHED_PAGING_CFA, {16}, 6, 0, 1, 1)), &s));
  aiot_device_state_t st = d.st();
  EXPECT_EQ(-1, st.as_id);
  EXPECT_EQ(-1, st.transaction_id);
  EXPECT_EQ(-1, st.failed_tid);
  EXPECT_EQ(1, st.cfa_in_procedure);
  EXPECT_EQ(0, st.nack_monitoring);
  EXPECT_EQ(0, st.cbra_active);
}

TEST(DeviceMac, CfaNotMatchedAlsoClearsTheProcedure)
{
  Dev d;
  d.u.has_pid = true;
  d.u.pid = {1, 1};
  aiot_device_send_t s;
  aiot_sched_info_t cfa = sched(AIOT_SCHED_PAGING_CFA, {8}, 4, 0);
  ASSERT_EQ(1, d.on(paging_cfa({1, 1}, cfa), &s));
  EXPECT_EQ(1, d.st().cfa_in_procedure);
  EXPECT_EQ(0, d.on(paging_cfa({1, 0}, cfa), &s));
  EXPECT_EQ(0, d.st().cfa_in_procedure);
  /* with no CFA procedure, data for nobody known is not for the device */
  EXPECT_EQ(0, d.on(data_sdu(9, data_sched(), Bytes{1}), &s));
  EXPECT_TRUE(d.u.got.empty());
}

TEST(DeviceMac, PagingIdsOfCbra)
{
  aiot_device_send_t s;
  Bytes pid = {1, 0, 1, 1};
  {
    /* a Paging ID that is not the device's: no access, the Transaction ID is taken all the same */
    Dev d;
    d.u.has_pid = true;
    d.u.pid = {0, 0, 0, 0};
    EXPECT_EQ(0, d.on(paging_cbra(4, cbra_sched(), 0, 0, &pid), &s));
    EXPECT_EQ(4, d.st().transaction_id);
    EXPECT_EQ(0, d.st().cbra_active);
    EXPECT_EQ(0, d.u.selected);
    EXPECT_EQ(0u, d.u.draws.size());
    ASSERT_EQ(1u, d.u.pids_asked.size());
    EXPECT_EQ(pid, d.u.pids_asked[0]);
  }
  {
    /* a Paging ID that is */
    Dev d;
    d.u.has_pid = true;
    d.u.pid = pid;
    ASSERT_EQ(1, d.on(paging_cbra(4, cbra_sched(), 0, 0, &pid), &s));
    EXPECT_EQ(0, d.u.selected);
  }
  {
    /* no Paging ID: the paging selects every device and the upper layer is told */
    Dev d;
    d.u.has_pid = true;
    d.u.pid = {0};
    ASSERT_EQ(1, d.on(paging_cbra(4, cbra_sched()), &s));
    EXPECT_EQ(1, d.u.selected);
    EXPECT_TRUE(d.u.pids_asked.empty());
  }
}

TEST(DeviceMac, NullCallbacksTakeTheDefaults)
{
  uint32_t calls = 0;
  struct Ctx {
    uint32_t *calls;
  } ctx{&calls};
  aiot_device_upper_t a;
  memset(&a, 0, sizeof(a));
  a.user = &ctx;
  a.draw = [](void *u, uint32_t) -> uint32_t {
    (*static_cast<Ctx *>(u)->calls)++;
    return 0;
  };
  aiot_device_t *d = aiot_device_new(&a, AIOT_OOK);
  ASSERT_NE(nullptr, d);
  aiot_device_send_t s;
  aiot_mac_r2d_msg_t pg = paging_cbra(1, cbra_sched());
  pg.has_security_param = 1; /* no callback for it: ignored */
  ASSERT_EQ(1, aiot_device_on_r2d(d, &pg, &s));
  EXPECT_EQ(AIOT_OOK, s.params.modulation);
  EXPECT_EQ(2u, calls);
  Bytes pid = {1, 0, 1};
  pg = paging_cbra(2, cbra_sched(), 0, 0, &pid); /* every Paging ID matches */
  ASSERT_EQ(1, aiot_device_on_r2d(d, &pg, &s));
  aiot_mac_r2d_msg_t cfa = paging_cfa(pid, sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0));
  ASSERT_EQ(1, aiot_device_on_r2d(d, &cfa, &s)); /* no SDU: an empty Upper Layer Data, padded to the TBS */
  Upl u = parse_upl(s);
  EXPECT_TRUE(u.sdu.empty());
  EXPECT_EQ(4, u.padding);
  EXPECT_EQ(48, s.n_bits);
  aiot_mac_r2d_msg_t dm = data_sdu(3, data_sched(8, 5), Bytes{1, 2}); /* nobody to deliver to */
  ASSERT_EQ(1, aiot_device_on_r2d(d, &dm, &s));
  EXPECT_EQ(3, [&] {
    aiot_device_state_t st;
    aiot_device_state(d, &st);
    return st.as_id;
  }());
  EXPECT_EQ(40, s.n_bits);
  aiot_device_free(d);
}

TEST(DeviceMac, SegmentationRoundTrip)
{
  Bytes sdu = iota_bytes(35);
  Dev d(9);
  TestReader r;
  r.next_as_id = 5;
  d.u.sdus = {sdu};
  aiot_device_send_t s;
  cbra_to_msg2(d, r, 1, &s);
  ASSERT_EQ(5, d.st().as_id);
  EXPECT_EQ(AIOT_MSG2, s.msg);
  Bytes whole;
  Upl u = parse_upl(s);
  EXPECT_EQ(1, u.mdi);
  EXPECT_EQ(10u, u.sdu.size());
  EXPECT_EQ(0, u.padding);
  EXPECT_EQ(35, d.st().sending_len);
  whole.insert(whole.end(), u.sdu.begin(), u.sdu.end());
  int guard = 0;
  while (u.mdi && guard++ < 10) {
    ASSERT_EQ(1, d.on(data_received(5, data_sched(8, 12), (int)whole.size()), &s));
    EXPECT_EQ(AIOT_DATA, s.msg);
    EXPECT_EQ(12, s.tbs_bytes);
    EXPECT_EQ(96, s.n_bits);
    u = parse_upl(s);
    whole.insert(whole.end(), u.sdu.begin(), u.sdu.end());
  }
  EXPECT_EQ(sdu, whole);
  EXPECT_EQ(3, guard); /* 10 + 10 + 10 + 5 */
  EXPECT_EQ(0, u.mdi);
  EXPECT_EQ(5u, u.sdu.size());
  EXPECT_EQ(5, u.padding);
  EXPECT_EQ(-1, d.st().sending_len);
  /* nothing is being segmented any more: a further request makes no D2R */
  EXPECT_EQ(0, d.on(data_received(5, data_sched(8, 12), 35), &s));
}

TEST(DeviceMac, SegmentationBoundariesAndMdi)
{
  /* the SDU just fits: no segmentation, MDI 0 */
  for (int len : {10, 11, 20, 21, 1}) {
    Dev d;
    TestReader r;
    d.u.sdus = {iota_bytes(len)};
    aiot_device_send_t s;
    cbra_to_msg2(d, r, 1, &s);
    Upl u = parse_upl(s);
    int expect_first = std::min(len, 10);
    EXPECT_EQ((size_t)expect_first, u.sdu.size()) << "len " << len;
    EXPECT_EQ(len > 10 ? 1 : 0, u.mdi) << "len " << len;
    EXPECT_EQ(len > 10 ? 0 : 10 - len, u.padding) << "len " << len;
    EXPECT_EQ(len > 10 ? len : -1, d.st().sending_len);
    if (len > 10) {
      ASSERT_EQ(1, d.on(data_received(1, data_sched(8, 12), 10), &s));
      u = parse_upl(s);
      int rest = len - 10;
      EXPECT_EQ((size_t)std::min(rest, 10), u.sdu.size()) << "len " << len;
      EXPECT_EQ(rest > 10 ? 1 : 0, u.mdi) << "len " << len << " 20 bytes is two full segments";
      Bytes all = iota_bytes(len);
      EXPECT_EQ(Bytes(all.begin() + 10, all.begin() + 10 + std::min(rest, 10)), u.sdu);
    }
  }
}

TEST(DeviceMac, SegmentsFollowTheSchedulingInfoOfTheRequest)
{
  /* the room of a segment is the D2R TBS of the R2D that asks for it */
  Dev d;
  TestReader r;
  r.next_as_id = 5;
  d.u.sdus = {iota_bytes(40)};
  aiot_device_send_t s;
  cbra_to_msg2(d, r, 1, &s);
  EXPECT_EQ(10u, parse_upl(s).sdu.size());
  ASSERT_EQ(1, d.on(data_received(5, data_sched(4, 8), 10), &s)); /* TBS 8: room 6 */
  Upl u = parse_upl(s);
  Bytes all = iota_bytes(40);
  EXPECT_EQ(Bytes(all.begin() + 10, all.begin() + 16), u.sdu);
  EXPECT_EQ(1, u.mdi);
  EXPECT_EQ(8, s.tbs_bytes);
  EXPECT_EQ(4, s.params.r_sfs);
  ASSERT_EQ(1, d.on(data_received(5, data_sched(8, 125), 16), &s)); /* the rest: 24 bytes */
  u = parse_upl(s);
  EXPECT_EQ(24u, u.sdu.size());
  EXPECT_EQ(0, u.mdi);
  EXPECT_EQ(125 - 2 - 24, u.padding);
}

TEST(DeviceMac, LargeSduSegmentedAtMaxTbs)
{
  Bytes sdu = iota_bytes(300);
  Dev d;
  TestReader r;
  d.u.sdus = {sdu};
  aiot_device_send_t s;
  cbra_to_msg2(d, r, 1, &s, ridr_sched({1, 2, 4, 8, 16, 32, 64, 128}, 125));
  Bytes whole;
  Upl u = parse_upl(s);
  EXPECT_EQ(123u, u.sdu.size());
  whole = u.sdu;
  int segs = 1;
  while (u.mdi) {
    ASSERT_EQ(1, d.on(data_received(1, data_sched(8, 125), (int)whole.size()), &s));
    u = parse_upl(s);
    whole.insert(whole.end(), u.sdu.begin(), u.sdu.end());
    segs++;
    ASSERT_LT(segs, 10);
  }
  EXPECT_EQ(3, segs);
  EXPECT_EQ(sdu, whole);
}

TEST(DeviceMac, NoRoomForASegmentIsAnError)
{
  Dev d;
  TestReader r;
  d.u.sdus = {Bytes{1}};
  aiot_device_send_t s1, s;
  ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched()), &s1));
  /* TBS 2 carries no SDU byte: the SDU cannot be sent at all */
  EXPECT_EQ(AIOT_EINVAL, d.on(r.rid_response({heard_of(s1)}, ridr_sched({1}, 2)), &s));
  /* without an SDU the same TBS is just the 2-byte header */
  Dev e;
  TestReader r2;
  ASSERT_EQ(1, e.on(paging_cbra(1, cbra_sched()), &s1));
  ASSERT_EQ(1, e.on(r2.rid_response({heard_of(s1)}, ridr_sched({1}, 2)), &s));
  EXPECT_EQ(16, s.n_bits);
  EXPECT_TRUE(parse_upl(s).sdu.empty());
}

TEST(DeviceMac, DataRequestWithNothingSegmentedMakesNoD2r)
{
  Dev d;
  TestReader r;
  aiot_device_send_t s;
  cbra_to_msg2(d, r, 1, &s); /* no SDU: nothing pending */
  EXPECT_EQ(0, d.on(data_received(1, data_sched(), 5), &s));
  /* received size 0 is a request for data now: the device sends whatever the upper layer has */
  d.u.sdus = {Bytes{9, 9}};
  ASSERT_EQ(1, d.on(data_received(1, data_sched(), 0), &s));
  EXPECT_EQ((Bytes{9, 9}), parse_upl(s).sdu);
  /* and an empty one when it has none */
  ASSERT_EQ(1, d.on(data_received(1, data_sched(), 0), &s));
  EXPECT_TRUE(parse_upl(s).sdu.empty());
}

TEST(DeviceMac, R2dDataWithSduStartsANewSdu)
{
  /* 5.4.2: each data transmission asks the upper layer afresh; a half-sent SDU is dropped */
  Dev d;
  TestReader r;
  d.u.sdus = {iota_bytes(30)};
  aiot_device_send_t s;
  cbra_to_msg2(d, r, 1, &s);
  EXPECT_EQ(30, d.st().sending_len);
  d.u.sdus = {Bytes{7}};
  ASSERT_EQ(1, d.on(data_sdu(1, data_sched(), Bytes{1}), &s));
  EXPECT_EQ(-1, d.st().sending_len);
  EXPECT_EQ((Bytes{7}), parse_upl(s).sdu);
  EXPECT_EQ(0, d.on(data_received(1, data_sched(), 10), &s));
}

TEST(DeviceMac, R2dDataNotForMe)
{
  Dev d(10);
  TestReader r;
  aiot_device_send_t s, m3;
  cbra_to_msg2(d, r, 1, &m3);
  ASSERT_EQ(1, d.st().as_id);
  EXPECT_EQ(0, d.on(data_sdu(4, data_sched(), Bytes{'x'}), &s));
  EXPECT_EQ(0, d.on(data_received(4, data_sched(), 0), &s));
  EXPECT_TRUE(d.u.got.empty());
  EXPECT_EQ(1, d.st().as_id);
  EXPECT_EQ(1, d.st().nack_monitoring); /* data for somebody else does not end the monitoring */
  /* a device without an AS ID and no CFA does not take it either */
  Dev e;
  EXPECT_EQ(0, e.on(data_sdu(4, data_sched(), Bytes{'x'}), &s));
  EXPECT_EQ(-1, e.st().as_id);
  EXPECT_TRUE(e.u.got.empty());
}

TEST(DeviceMac, ImplicitAsIdAfterCfaNeedsAnSdu)
{
  Dev d;
  aiot_device_send_t s;
  ASSERT_EQ(1, d.on(paging_cfa(Bytes(4, 1), sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
  EXPECT_EQ(0, d.on(data_received(0x55, data_sched(), 3), &s)); /* CI = 0 and size 3: not taken */
  EXPECT_EQ(-1, d.st().as_id);
  ASSERT_EQ(1, d.on(data_sdu(0x55, data_sched(), Bytes{1}), &s));
  EXPECT_EQ(0x55, d.st().as_id);
}

TEST(DeviceMac, CfaReceivedDataSizeZeroGivesTheAsId)
{
  /* 5.4.3: after CFA, CI = 0 with a Received Data Size of 0 sets the AS ID and starts the D2R */
  Dev d;
  aiot_device_send_t s;
  ASSERT_EQ(1, d.on(paging_cfa(Bytes(4, 1), sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
  EXPECT_TRUE(parse_upl(s).sdu.empty()); /* the CFA D2R found no data yet */
  d.u.sdus = {Bytes{4, 5}};
  ASSERT_EQ(1, d.on(data_received(0x55, data_sched(8, 9), 0), &s));
  EXPECT_EQ(0x55, d.st().as_id);
  EXPECT_EQ(AIOT_DATA, s.msg);
  EXPECT_EQ(9, s.tbs_bytes);
  EXPECT_EQ((Bytes{4, 5}), parse_upl(s).sdu);
  EXPECT_TRUE(d.u.got.empty());
  /* later data for the AS ID goes the usual way */
  ASSERT_EQ(1, d.on(data_sdu(0x55, data_sched(), Bytes{1}), &s));
  /* without a CFA, no */
  Dev e;
  EXPECT_EQ(0, e.on(data_received(0x55, data_sched(), 0), &s));
  EXPECT_EQ(-1, e.st().as_id);
}

TEST(DeviceMac, PendingUpperDataSetsMdi)
{
  /* 5.4.2: data not available yet, due to processing delay: MDI 1 and SDU Length 0; ignored: MDI 0 */
  TestReader r;
  aiot_device_send_t m3, s;
  {
    Dev d;
    d.u.pending = true;
    cbra_to_msg2(d, r, 1, &m3);
    Upl u = parse_upl(m3);
    EXPECT_TRUE(u.sdu.empty());
    EXPECT_EQ(1, u.mdi);
    EXPECT_EQ(12 * 8, u.n_bits);
    EXPECT_EQ(-1, d.st().sending_len);
    /* the Reader asks again with a Received Data Size of 0, and the data is there now */
    d.u.pending = false;
    d.u.sdus = {Bytes{1, 2, 3}};
    ASSERT_EQ(1, d.on(data_received(1, data_sched(), 0), &s));
    u = parse_upl(s);
    EXPECT_EQ((Bytes{1, 2, 3}), u.sdu);
    EXPECT_EQ(0, u.mdi);
  }
  {
    Dev d; /* nothing and not pending: MDI 0, whatever the negative value */
    for (int code : {AIOT_UPPER_SDU_NONE, -7}) {
      d.u.none_code = code;
      ASSERT_EQ(1, d.on(paging_cfa(Bytes(4, 1), sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
      Upl u = parse_upl(s);
      EXPECT_TRUE(u.sdu.empty());
      EXPECT_EQ(0, u.mdi) << code;
    }
  }
  {
    Dev d; /* CFA */
    d.u.pending = true;
    ASSERT_EQ(1, d.on(paging_cfa(Bytes(4, 1), sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
    Upl u = parse_upl(s);
    EXPECT_TRUE(u.sdu.empty());
    EXPECT_EQ(1, u.mdi);
  }
}

TEST(DeviceMac, IntegrityFailureDeliversButSendsNothing)
{
  aiot_device_send_t s;
  {
    /* CBRA: the SDU is delivered, no D2R; the monitoring of 5.5.2 and 5.3.1.3 ended all the same */
    Dev d;
    TestReader r;
    d.u.sdus = {Bytes{'a'}, Bytes{'b'}};
    cbra_to_msg2_k4(d, r, 1, &s);
    d.u.fail_integrity = true;
    EXPECT_EQ(0, d.on(data_sdu(1, data_sched(), Bytes{'c', 'm', 'd'}), &s));
    EXPECT_EQ(std::vector<Bytes>{(Bytes{'c', 'm', 'd'})}, d.u.got);
    EXPECT_EQ(1, d.st().as_id);
    EXPECT_EQ(0, d.st().nack_monitoring);
    EXPECT_EQ(0, d.st().msg2_budget);
    /* the SDU 'b' was not asked for: the next good one is answered with it */
    d.u.fail_integrity = false;
    ASSERT_EQ(1, d.on(data_sdu(1, data_sched(), Bytes{'z'}), &s));
    EXPECT_EQ((Bytes{'b'}), parse_upl(s).sdu);
  }
  {
    /* CFA: the AS ID is stored, the SDU delivered, no D2R */
    Dev d;
    d.u.fail_integrity = true;
    d.u.sdus = {Bytes{'a'}};
    ASSERT_EQ(1, d.on(paging_cfa(Bytes(4, 1), sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
    EXPECT_EQ((Bytes{'a'}), parse_upl(s).sdu);
    d.u.sdus = {Bytes{'b'}};
    EXPECT_EQ(0, d.on(data_sdu(0x55, data_sched(), Bytes{'x'}), &s));
    EXPECT_EQ(std::vector<Bytes>{(Bytes{'x'})}, d.u.got);
    EXPECT_EQ(0x55, d.st().as_id);
    d.u.fail_integrity = false;
    ASSERT_EQ(1, d.on(data_sdu(0x55, data_sched(), Bytes{'y'}), &s));
    EXPECT_EQ((Bytes{'b'}), parse_upl(s).sdu);
  }
}

TEST(DeviceMac, SecurityParameterGoesToTheUpperLayer)
{
  const Bytes sec = sec_bits(), sec2 = sec_bits(1);
  const Bytes mine = {1, 0, 1}, other = {0, 0, 0};
  aiot_device_send_t s, x;
  {
    /* 5.2 CBRA: forwarded before the Paging ID, or before the selection indication */
    Dev d;
    TestReader r;
    d.u.has_pid = true;
    d.u.pid = mine;
    ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched(), 0, 0, &mine, &sec), &s));
    ASSERT_EQ(1u, d.u.secs.size());
    EXPECT_EQ(sec, d.u.secs[0]);
    EXPECT_EQ((std::vector<std::string>{"sec", "matched"}), d.u.events);
    /* the same Transaction ID, done: not a fresh paging, nothing forwarded */
    ASSERT_EQ(1, d.on(r.rid_response({heard_of(s)}, ridr_sched()), &x));
    EXPECT_EQ(0, d.on(paging_cbra(1, cbra_sched(), 0, 0, &mine, &sec2), &x));
    EXPECT_EQ(1u, d.u.secs.size());
    /* a new one is */
    ASSERT_EQ(1, d.on(paging_cbra(2, cbra_sched(), 0, 0, &mine, &sec2), &x));
    ASSERT_EQ(2u, d.u.secs.size());
    EXPECT_EQ(sec2, d.u.secs[1]);
    /* so is the same one after a failed access (no Msg2 before this paging) */
    ASSERT_EQ(1, d.on(paging_cbra(2, cbra_sched(), 0, 0, &mine, &sec), &x));
    ASSERT_EQ(3u, d.u.secs.size());
    EXPECT_EQ(sec, d.u.secs[2]);
    /* a Paging ID that is not the device's: forwarded all the same */
    EXPECT_EQ(0, d.on(paging_cbra(3, cbra_sched(), 0, 0, &other, &sec2), &x));
    ASSERT_EQ(4u, d.u.secs.size());
    EXPECT_EQ(sec2, d.u.secs[3]);
  }
  {
    Dev d; /* no Paging ID: forwarded, then the device is selected */
    ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched(), 0, 0, nullptr, &sec), &s));
    EXPECT_EQ((std::vector<std::string>{"sec", "selected"}), d.u.events);
  }
  {
    /* 5.2 CFA: forwarded, then the Paging ID is asked; a Paging ID that does not match is no reason not to */
    Dev d;
    d.u.has_pid = true;
    d.u.pid = mine;
    EXPECT_EQ(0, d.on(paging_cfa(other, sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0), &sec), &s));
    ASSERT_EQ(1u, d.u.secs.size());
    EXPECT_EQ(sec, d.u.secs[0]);
    EXPECT_EQ((std::vector<std::string>{"sec", "matched"}), d.u.events);
    ASSERT_EQ(1, d.on(paging_cfa(mine, sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0), &sec2), &s));
    ASSERT_EQ(2u, d.u.secs.size());
    EXPECT_EQ(sec2, d.u.secs[1]);
  }
  {
    /* SPPI 0: nothing to forward */
    Dev d;
    ASSERT_EQ(1, d.on(paging_cbra(1, cbra_sched()), &s));
    ASSERT_EQ(1, d.on(paging_cfa(mine, sched(AIOT_SCHED_PAGING_CFA, {8}, 6, 0)), &s));
    EXPECT_TRUE(d.u.secs.empty());
    EXPECT_EQ(std::count(d.u.events.begin(), d.u.events.end(), "sec"), 0);
  }
}

TEST(DeviceMac, WrongMessageKindIsRejected)
{
  Dev d;
  aiot_mac_r2d_msg_t m = trigger();
  m.kind = (aiot_r2d_kind_t)99;
  aiot_device_send_t s;
  EXPECT_EQ(AIOT_EINVAL, d.on(m, &s));
  /* a CBRA paging whose fields cannot be acted on */
  aiot_mac_r2d_msg_t p = paging_cbra(1, cbra_sched(), 16);
  EXPECT_EQ(AIOT_EINVAL, d.on(p, &s));
}

TEST(DeviceMac, ParametersOfTheSend)
{
  for (aiot_mod_t mod : {AIOT_OOK, AIOT_BPSK}) {
    Dev d(0, mod);
    TestReader r;
    aiot_sched_info_t cs = cbra_sched({4, 8});
    cs.r_block = 2;
    cs.l_amble = 7;
    cs.t_bit_num = 1;
    cs.t_bit_den = 2;
    aiot_device_send_t s, m3;
    ASSERT_EQ(1, d.on(paging_cbra(1, cs), &s));
    aiot_d2r_params_t p;
    ASSERT_EQ(0, aiot_mac_to_d2r_params(&cs, s.params.r_sfs, mod, &p));
    EXPECT_EQ(0, memcmp(&p, &s.params, sizeof(p)));
    EXPECT_EQ(mod, s.params.modulation);
    EXPECT_EQ(2, s.n_candidates);
    aiot_sched_info_t rs = ridr_sched({2, 4}, 9);
    rs.r_block = 2;
    ASSERT_EQ(1, d.on(r.rid_response({heard_of(s)}, rs), &m3));
    ASSERT_EQ(0, aiot_mac_to_d2r_params(&rs, 2, mod, &p));
    EXPECT_EQ(0, memcmp(&p, &m3.params, sizeof(p)));
    EXPECT_EQ(2, m3.n_candidates);
    EXPECT_EQ(2, m3.candidates[0]);
    EXPECT_EQ(4, m3.candidates[1]);
    EXPECT_EQ(0, m3.slot);
    aiot_device_send_t dd;
    ASSERT_EQ(1, d.on(data_sdu(1, data_sched(16, 7), Bytes{}), &dd));
    EXPECT_EQ(16, dd.params.r_sfs);
    EXPECT_EQ(0, dd.n_candidates);
    EXPECT_EQ(AIOT_DATA, dd.msg);
  }
}

TEST(DeviceMac, SendOffsetUsesTheSlotAndTheFrame)
{
  /* Msg1 of the second time slot is the one an earlier occasion follows the same R2D */
  Dev a, b;
  a.u.script = {0, 1};
  b.u.script = {1, 1};
  aiot_device_send_t s0, s1;
  aiot_sched_info_t sc = cbra_sched({8}, 2);
  ASSERT_EQ(1, a.on(paging_cbra(1, sc, 1), &s0));
  ASSERT_EQ(1, b.on(paging_cbra(1, sc, 1), &s1));
  EXPECT_EQ(0, s0.slot);
  EXPECT_EQ(1, s1.slot);
  int o0 = aiot_device_send_offset(&s0, 12);
  int o1 = aiot_device_send_offset(&s1, 12);
  EXPECT_GT(o0, 0);
  EXPECT_GT(o1, 0);
  int frame = aiot_frame_bits(16, &s1.params);
  EXPECT_EQ(aiot_t_r_to_d(&s1.params, 12, AIOT_MSG1, 2, s1.candidates, s1.n_candidates, 1, frame * 2 * 8), o1);
  EXPECT_EQ(aiot_t_r_to_d(&s0.params, 12, AIOT_MSG1, 2, s0.candidates, s0.n_candidates, 0, frame * 2 * 8), o0);
  EXPECT_NE(o0, o1); /* the slot reaches T_R->D */
}

/* ---------------------------------------------------------------------------------------------
 * A whole inventory: many devices, one Reader, collisions on shared occasions
 * ------------------------------------------------------------------------------------------- */

struct InventoryResult {
  std::map<int, Bytes> read;
  int collisions = 0;
  int r2d_count = 0;
};

/* The Reader pages with one Transaction ID, then sends Access Triggers until every occasion has
 * passed, answering each R2D's singles with one Random ID Response. Occasions picked by more than
 * one device are lost (no capture). It pages again, same TID, until nobody answers. */
InventoryResult run_inventory(int n_dev, int n_code, std::vector<int> sfs, int x, uint32_t seed, int max_rounds = 30)
{
  std::mt19937 rng(seed);
  std::vector<std::unique_ptr<Dev>> devs;
  for (int i = 0; i < n_dev; i++) {
    devs.emplace_back(new Dev(0, AIOT_BPSK, &rng));
    std::string t = "tag" + std::to_string(i);
    devs.back()->u.sdus.push_back(Bytes(t.begin(), t.end()));
  }
  TestReader r;
  InventoryResult res;
  aiot_sched_info_t sc = cbra_sched(sfs, x);
  int per_r2d = x * (int)sfs.size();
  int triggers = -(-(1 << n_code) / per_r2d) - 1;
  for (int round = 0; round < max_rounds; round++) {
    bool quiet = true;
    for (int t = 0; t <= triggers; t++) {
      aiot_mac_r2d_msg_t msg = t == 0 ? paging_cbra(1, sc, n_code, 0) : trigger();
      res.r2d_count++;
      std::vector<std::pair<int, int>> keys;
      std::map<std::pair<int, int>, std::vector<Heard>> occ;
      for (auto &dv : devs) {
        aiot_device_send_t s;
        int rc = dv->on(msg, &s);
        EXPECT_GE(rc, 0);
        if (rc == 1) {
          std::pair<int, int> key{s.slot, s.params.r_sfs};
          if (!occ.count(key))
            keys.push_back(key);
          occ[key].push_back(heard_of(s));
        }
      }
      quiet &= occ.empty();
      std::vector<Heard> singles;
      for (auto &k : keys) {
        if (occ[k].size() == 1)
          singles.push_back(occ[k][0]);
        else
          res.collisions++;
      }
      if (singles.empty())
        continue;
      aiot_mac_r2d_msg_t ridr = r.rid_response(singles, ridr_sched());
      res.r2d_count++;
      for (auto &dv : devs) {
        aiot_device_send_t s;
        int rc = dv->on(ridr, &s);
        EXPECT_GE(rc, 0);
        if (rc == 1) {
          Upl u = parse_upl(s);
          res.read[dv->st().as_id] = u.sdu;
        }
      }
    }
    if (quiet)
      break;
  }
  return res;
}

TEST(DeviceMacInventory, ReadsEveryDevice)
{
  InventoryResult r = run_inventory(20, 3, {1, 2, 4, 8}, 2, 1);
  std::vector<Bytes> got, want;
  for (auto &kv : r.read)
    got.push_back(kv.second);
  for (int i = 0; i < 20; i++) {
    std::string t = "tag" + std::to_string(i);
    want.emplace_back(t.begin(), t.end());
  }
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  EXPECT_EQ(want, got);
  EXPECT_GT(r.collisions, 0); /* 20 devices on 8 occasions must collide */
}

TEST(DeviceMacInventory, WiderFrameCollidesLess)
{
  int narrow = 0, wide = 0;
  for (uint32_t s = 0; s < 5; s++) {
    narrow += run_inventory(16, 2, {8}, 1, s).collisions;
    wide += run_inventory(16, 5, {8}, 1, s).collisions;
  }
  EXPECT_LT(wide, narrow);
}

} // namespace
