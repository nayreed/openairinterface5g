/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// nr_aiot_reader.c: the gNB as an A-IoT Reader, closed over an air in samples. This is the room's
// topology 1 in one process: the gNB maps each DL slot 4 slots ahead of the air (as sl_ahead does),
// OFDM sends it, and each Tag decodes the R2D with its own receiver and runs the device MAC. The
// D2R each Tag sends is backscatter of the carrier, at the centre of UL PRBs 15-21, in int16 counts
// with noise, and the gNB's D2R receiver gets it slot by slot. A split room (topology 3) has a UE
// besides, at the other end of a loopback socket (nr_aiot_peer.h), that sends the R2D or receives the
// D2R in the gNB's stead, on radio timestamps of its own.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
extern "C" {
#include "LAYER2/AIOT_MAC/nr_aiot_reader.h"
#include "LAYER2/AIOT_MAC/nr_aiot_peer.h"
#include "LAYER2/AIOT_MAC/nr_aiot_config.h"
#include "LAYER2/AIOT_MAC/aiot_node_msg.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}

namespace {

const int kSym = 14, kFft = AIOT_N_FFT, kPrb = 25, kAmp = 519, kSlot = AIOT_SAMPLES_PER_MS, kAhead = 4;
const double kShiftHz = 1080000.0, kCwCounts = 400, kTagCounts = 20;

// Steps the peer until it has handled want messages, or a second has passed. Returns how many it handled.
int StepPeer(nr_aiot_peer_t *peer, int want)
{
  int done = 0;
  for (int i = 0; i < 1000 && done < want; i++) {
    const int n = nr_aiot_peer_step(peer);
    done += n;
    if (n == 0)
      usleep(1000);
  }
  return done;
}

// A window the D2R receiver can open: a 16-bit Msg1 at R_SFS 1, T_bit 1/8.
aiot_node_window_t ValidWindow(int64_t tag)
{
  aiot_node_window_t w = {};
  w.expect.tag = tag;
  w.expect.a_bits = 16;
  aiot_d2r_params_t &p = w.expect.params;
  p.t_bit_num = 1;
  p.t_bit_den = 8;
  p.r_sfs = 1;
  p.r_block = 1;
  p.r_code = 1;
  p.l_amble = 31;
  int i_bit[4];
  EXPECT_GT(aiot_i_bit_values(1, 8, i_bit), 0);
  p.i_bit = i_bit[0];
  EXPECT_EQ(aiot_d2r_expect_check(&w.expect), 0);
  return w;
}

aiot_link_config_t RoomLink()
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

// The time samples of a slot's grid: the carrier centred on DC, the inverse DFT, CP 30 or 27.
void Modulate(const std::vector<c16_t> &grid, std::complex<double> *out)
{
  const int half = kPrb * 6;
  int at = 0;
  for (int s = 0; s < kSym; s++) {
    std::vector<std::complex<double>> u(kFft, 0);
    for (int k = 0; k < kPrb * 12; k++) {
      const c16_t v = grid[s * kFft + k];
      if (!v.r && !v.i)
        continue;
      const int bin = k < half ? kFft - half + k : k - half;
      for (int t = 0; t < kFft; t++)
        u[t] += std::complex<double>(v.r, v.i) * std::polar(1.0, 2 * M_PI * bin * t / kFft);
    }
    const int cp = s % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
    for (int t = 0; t < cp; t++)
      out[at++] = u[kFft - cp + t];
    for (int t = 0; t < kFft; t++)
      out[at++] = u[t];
  }
}

struct Tag {
  int id;
  std::mt19937 rng;
  aiot_device_t *mac = nullptr;
  aiot_r2d_rx_t *rx = nullptr;
  double phase;
  struct D2r {
    int64_t at;
    int r_sfs;
    std::vector<double> w;
  };
  std::vector<D2r> d2r;
  int late = 0; // D2Rs that would have started before the Tag decoded the R2D
  int64_t decoded_r2d = 0;
};

int Matched(void *user, const uint8_t *paging_id, int len)
{
  const Tag *t = static_cast<Tag *>(user);
  uint8_t mine[16];
  aiot_paging_id_of_node(t->id, mine);
  return len == 16 && std::equal(mine, mine + 16, paging_id);
}

int NextSdu(void *user, uint8_t *sdu, int cap)
{
  const Tag *t = static_cast<Tag *>(user);
  if (cap < 3)
    return -1;
  sdu[0] = t->id >> 8;
  sdu[1] = t->id & 0xff;
  sdu[2] = 42;
  return 3;
}

uint32_t Draw(void *user, uint32_t n)
{
  return std::uniform_int_distribution<uint32_t>(0, n - 1)(static_cast<Tag *>(user)->rng);
}

// Where the Reader's R2D and D2R are: both the gNB's (topology 1), or one of them a UE's.
enum class Split { kNone, kRemoteR2d, kRemoteD2r };

struct Room {
  nr_aiot_r2d_t r2d;
  nr_aiot_d2r_t *d2r = nullptr;
  nr_aiot_reader_t *reader = nullptr;
  // a split room's UE: it maps its UL slots ue_ahead ahead of the air, and its radio timestamps are
  // the air's plus ue_offset
  Split split;
  int ue_ahead;
  int64_t ue_offset = 777777;
  nr_aiot_r2d_t ue_r2d;
  nr_aiot_d2r_t *ue_d2r = nullptr;
  nr_aiot_peer_t *peer = nullptr;
  bool serve = true; // false: the UE does not answer
  int serve_every = 1; // the UE serves the Reader every serve_every slots
  std::vector<std::unique_ptr<Tag>> tags;
  std::mt19937_64 gen{11};
  std::vector<std::complex<double>> dl; // the air's DL from sample 0
  std::string dir;
  int64_t slot = 0; // the air's slot: the UL the gNB receives next, the DL the Tags hear next
  int64_t clock; // the node's TX slot k starts at air sample k kSlot + clock

  Room(const char *protocol,
       const char *params,
       int n_tags,
       const char *name,
       int64_t tx_clock = 0,
       const char *data_dest = "",
       Split where = Split::kNone,
       int ue_tx_ahead = kAhead)
      : split(where), ue_ahead(ue_tx_ahead), clock(tx_clock)
  {
    // the gNB's R2D has no PRBs when the UE sends it: it is then only the gNB's clock
    nr_aiot_r2d_config_t rc = {split == Split::kRemoteR2d ? -1 : 0, 3, 0.0};
    EXPECT_EQ(nr_aiot_r2d_init(&rc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &r2d), 0);
    nr_aiot_r2d_config_t uc = {split == Split::kRemoteR2d ? 0 : -1, 3, 0.0};
    EXPECT_EQ(nr_aiot_r2d_init(&uc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &ue_r2d), 0);
    nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
    (split == Split::kRemoteD2r ? ue_d2r : d2r) = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
    EXPECT_TRUE(d2r || ue_d2r);
    nr_aiot_reader_config_t cfg = {};
    if (split != Split::kNone) {
      char err[256] = "";
      const nr_aiot_peer_config_t pc = {.port = 0, .addr = "127.0.0.1", .reader = "127.0.0.1"};
      peer = nr_aiot_peer_new(&pc, &ue_r2d, ue_d2r, err, sizeof(err));
      EXPECT_NE(peer, nullptr) << err;
      const std::string at = "127.0.0.1:" + std::to_string(peer ? nr_aiot_peer_port(peer) : 9);
      EXPECT_EQ(nr_aiot_reader_parse_address(split == Split::kRemoteR2d ? cfg.remote_r2d : cfg.remote_d2r, at.c_str()), 0);
    }
    snprintf(cfg.protocol, sizeof(cfg.protocol), "%s", protocol);
    EXPECT_EQ(nr_aiot_reader_parse_params(&cfg, params), 0);
    cfg.link = RoomLink();
    dir = std::string("/tmp/test_nr_aiot_reader_") + name;
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", dir.c_str());
    EXPECT_EQ(nr_aiot_reader_parse_data_dest(&cfg, data_dest), 0) << data_dest;
    for (int i = 0; i < n_tags; i++) {
      auto t = std::make_unique<Tag>();
      t->id = i;
      t->rng.seed(500 + i);
      t->phase = 0.7 * i;
      aiot_device_upper_t up = {t.get(), Matched, nullptr, nullptr, NextSdu, Draw};
      t->mac = aiot_device_new(&up, AIOT_BPSK);
      t->rx = aiot_r2d_rx_new();
      cfg.node_ids[cfg.n_nodes++] = i;
      tags.push_back(std::move(t));
    }
    char err[256] = "";
    reader = nr_aiot_reader_new(&cfg, &r2d, d2r, err, sizeof(err));
    EXPECT_NE(reader, nullptr) << err;
  }

  ~Room()
  {
    nr_aiot_reader_free(reader);
    nr_aiot_peer_free(peer);
    nr_aiot_d2r_free(d2r);
    nr_aiot_d2r_free(ue_d2r);
    nr_aiot_r2d_free(&r2d);
    nr_aiot_r2d_free(&ue_r2d);
    for (auto &t : tags) {
      aiot_device_free(t->mac);
      aiot_r2d_rx_free(t->rx);
    }
  }

  // One slot of air: the gNB maps the DL slot kAhead ahead, the Tags hear this slot's DL and
  // answer, the gNB receives this slot's UL, and the Reader takes its step. In a split room the UE
  // maps its UL slot ue_ahead ahead, which the Tags hear in the R2D's stead when the UE sends it, or
  // receives this slot's backscatter when the UE receives the D2R; it serves the gNB's Reader before
  // the Reader's step, so that what the Reader sends in one slot is served in the next.
  void Step()
  {
    const int64_t tx = slot + kAhead, tx_at = tx * kSlot + clock;
    const int frame = (int)((tx / 10) % 1024), tx_slot = (int)(tx % 10);
    std::vector<c16_t> grid(kSym * kFft, c16_t{0, 0});
    nr_aiot_r2d_set_slot_time(&r2d, frame, tx_slot, tx_at);
    nr_aiot_r2d_map(&r2d, frame, tx_slot, nullptr, grid.data());
    if (split != Split::kRemoteR2d) {
      dl.resize(std::max<size_t>(dl.size(), tx_at + kSlot));
      Modulate(grid, dl.data() + tx_at);
    } else {
      const int64_t ut = slot + ue_ahead, ut_at = ut * kSlot;
      std::vector<c16_t> ue_grid(kSym * kFft, c16_t{0, 0});
      nr_aiot_r2d_set_slot_time(&ue_r2d, (int)((ut / 10) % 1024), (int)(ut % 10), ut_at + ue_offset);
      nr_aiot_r2d_map(&ue_r2d, (int)((ut / 10) % 1024), (int)(ut % 10), nullptr, ue_grid.data());
      dl.resize(std::max<size_t>(dl.size(), ut_at + kSlot));
      Modulate(ue_grid, dl.data() + ut_at);
    }

    const int64_t t0 = slot * kSlot;
    std::vector<cd_t> y(kSlot);
    for (int i = 0; i < kSlot; i++)
      y[i] = {dl[t0 + i].real() / kAmp, dl[t0 + i].imag() / kAmp};
    for (auto &t : tags) {
      aiot_r2d_frame_t fr[2];
      const int k = aiot_r2d_rx_receive(t->rx, y.data(), kSlot, fr, 2);
      ASSERT_GE(k, 0);
      for (int f = 0; f < k; f++) {
        t->decoded_r2d++;
        aiot_mac_r2d_msg_t msg;
        if (aiot_mac_parse_r2d(fr[f].bits, fr[f].n_bits, &msg) != 0)
          continue;
        aiot_device_send_t s;
        if (aiot_device_on_r2d(t->mac, &msg, &s) != 1)
          continue;
        Tag::D2r d;
        d.at = fr[f].end_sample + aiot_device_send_offset(&s, fr[f].m_chip_symb);
        d.r_sfs = s.params.r_sfs;
        const int len = aiot_frame_samples(s.n_bits, &s.params);
        d.w.resize(len);
        ASSERT_EQ(aiot_d2r_waveform(s.bits, s.n_bits, &s.params, d.w.data(), len), len);
        if (d.at < t0)
          t->late++;
        t->d2r.push_back(std::move(d));
      }
    }

    // the UL: the carrier and each Tag's backscatter, at +1.08 MHz, in counts with noise
    std::vector<std::complex<double>> band(kSlot, std::polar(kCwCounts, 0.3));
    for (auto &t : tags)
      for (auto &d : t->d2r)
        for (int i = 0; i < kSlot; i++) {
          const int64_t j = t0 + i - d.at;
          if (j >= 0 && j < (int64_t)d.w.size())
            band[i] += std::polar(kTagCounts, t->phase) * d.w[j];
        }
    std::normal_distribution<double> g(0, 1.0);
    std::vector<c16_t> c(kSlot);
    for (int i = 0; i < kSlot; i++) {
      const int64_t ph = ((t0 + i) * (int64_t)kShiftHz) % AIOT_SAMPLE_RATE_HZ;
      const std::complex<double> v = band[i] * std::polar(1.0, 2 * M_PI * (double)ph / AIOT_SAMPLE_RATE_HZ);
      c[i] = {(int16_t)std::lround(v.real() + g(gen)), (int16_t)std::lround(v.imag() + g(gen))};
    }
    if (d2r) {
      ASSERT_EQ(nr_aiot_d2r_feed(d2r, t0, c.data(), kSlot), 0);
    }
    if (ue_d2r) {
      nr_aiot_d2r_set_slot_time(ue_d2r, (int)((slot / 10) % 1024), (int)(slot % 10), t0 + ue_offset);
      ASSERT_EQ(nr_aiot_d2r_feed(ue_d2r, t0 + ue_offset, c.data(), kSlot), 0);
    }
    if (peer && serve && slot % serve_every == 0)
      nr_aiot_peer_step(peer);
    nr_aiot_reader_step(reader);
    slot++;
  }

  int Run(int max_slots)
  {
    while (slot < max_slots && !nr_aiot_reader_done(reader))
      Step();
    return (int)slot;
  }

  // The Tags' D2Rs on the air, per (start, R_SFS), before `before`.
  std::map<std::pair<int64_t, int>, int> OnAir(int64_t before) const
  {
    std::map<std::pair<int64_t, int>, int> on_air;
    for (auto &t : tags)
      for (auto &d : t->d2r)
        if (d.at < before)
          on_air[{d.at, d.r_sfs}]++;
    return on_air;
  }

  // Every Tag has an AS ID, and its data reached the Reader.
  void ExpectEveryTagRegistered()
  {
    std::set<int> data_from, all;
    for (auto &t : tags) {
      all.insert(t->id);
      aiot_device_state_t s;
      aiot_device_state(t->mac, &s);
      EXPECT_GE(s.as_id, 0) << "Tag " << t->id << " has no AS ID";
    }
    for (auto &r : Csv("reader_rx.csv"))
      if (r.size() > 3 && r[2] == "data")
        data_from.insert(std::stoi(r[3]));
    EXPECT_EQ(data_from, all);
  }

  std::vector<std::vector<std::string>> Csv(const char *name) const
  {
    std::ifstream f(dir + "/" + name);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      std::vector<std::string> row;
      std::stringstream ss(line);
      std::string cell;
      while (std::getline(ss, cell, ','))
        row.push_back(cell);
      if (!line.empty() && line.back() == ',')
        row.push_back("");
      rows.push_back(row);
    }
    return rows;
  }
};

} // namespace

// Registration over the gNB, for 600 ms of air (registration joins and polls for as long as it
// runs): every Tag gets an AS ID and its data reaches the gNB; no R2D waits for a later slot, and
// every D2R falls in a window the gNB opened.
TEST(NrAiotReader, EveryTagRegistersThroughTheGnb)
{
  Room room("registration", "", 4, "registration");
  ASSERT_NE(room.reader, nullptr);
  room.Run(600);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_EQ(st.too_late, 0u);
  EXPECT_GT(st.r2d_sent, 8u);
  EXPECT_EQ(st.collisions, 0u);
  const nr_aiot_d2r_stats_t ds = nr_aiot_d2r_stats(room.d2r);
  EXPECT_EQ(ds.late, 0u);
  EXPECT_EQ(ds.zero_filled, 0u);
  const double settled_ms = 580; // the answers of the last R2Ds may still be on the air
  std::map<std::pair<int64_t, int>, int> on_air; // D2Rs per (start, R_SFS)
  for (auto &t : room.tags) {
    EXPECT_EQ(t->late, 0) << "Tag " << t->id;
    EXPECT_GT(t->decoded_r2d, 8) << "Tag " << t->id;
    for (auto &d : t->d2r)
      if (d.at < settled_ms * kSlot)
        on_air[{d.at, d.r_sfs}]++;
    aiot_device_state_t s;
    aiot_device_state(t->mac, &s);
    EXPECT_GE(s.as_id, 0) << "Tag " << t->id << " has no AS ID";
  }

  auto rx = room.Csv("reader_rx.csv");
  ASSERT_GE(rx.size(), 2u);
  EXPECT_EQ(rx[0], (std::vector<std::string>{"time_ms", "after", "kind", "node_id", "value", "id", "r_sfs", "slot", "rssi_dbfs"}));
  std::map<std::pair<int64_t, int>, int> heard;
  for (size_t i = 1; i < rx.size(); i++)
    if (rx[i][2] != "collision")
      heard[{std::llround(std::stod(rx[i][0]) * kSlot), std::stoi(rx[i][6])}]++;
  // a D2R alone on its start and R_SFS is heard, at its start; of those that share both, one at most
  int alone = 0, shared = 0;
  for (auto &[key, n] : on_air) {
    if (key.first >= settled_ms * kSlot)
      continue;
    const int got = heard.count(key) ? heard[key] : 0;
    if (n == 1) {
      alone++;
      EXPECT_EQ(got, 1) << "the D2R at " << key.first << " on R_SFS " << key.second;
    } else {
      shared += n;
      EXPECT_LE(got, 1);
    }
  }
  EXPECT_GT(alone, 30);
  for (auto &[key, n] : heard)
    EXPECT_TRUE(on_air.count(key) || key.first >= settled_ms * kSlot) << "heard a D2R no Tag sent at " << key.first;
  std::set<int> data_from;
  for (size_t i = 1; i < rx.size(); i++)
    if (rx[i][2] == "data") {
      data_from.insert(std::stoi(rx[i][3]));
      EXPECT_EQ(rx[i][4], "42");
    }
  EXPECT_EQ(data_from, (std::set<int>{0, 1, 2, 3}));
  auto tx = room.Csv("reader_tx.csv");
  ASSERT_GE(tx.size(), 2u);
  EXPECT_EQ(tx[0], (std::vector<std::string>{"start_ms", "end_ms", "message", "detail"}));
  EXPECT_LE(tx.size() - 1, st.r2d_sent); // the last R2D's answers may still be on their way
  // every R2D starts on a half-subframe, after the one before has ended
  double last_end = -1;
  for (size_t i = 1; i < tx.size(); i++) {
    const double start = std::stod(tx[i][0]), end = std::stod(tx[i][1]);
    EXPECT_NEAR(start * 2 - std::round(start * 2), 0, 1e-9) << tx[i][0];
    EXPECT_GE(start, last_end);
    last_end = end;
  }
}

// A node whose TX slots are not on its RX slots, as a UE's are (they start a timing advance early):
// the Reader plans on the time of each slot the node sends, and every Tag still registers.
TEST(NrAiotReader, PlansOnTheTimeOfTheSlotsTheNodeSends)
{
  const int64_t kClock = -1001;
  Room room("registration", "", 4, "tx_clock", kClock);
  ASSERT_NE(room.reader, nullptr);
  room.Run(600);
  EXPECT_EQ(nr_aiot_reader_stats(room.reader).too_late, 0u);
  EXPECT_EQ(nr_aiot_d2r_stats(room.d2r).late, 0u);
  for (auto &t : room.tags) {
    EXPECT_EQ(t->late, 0) << "Tag " << t->id;
    aiot_device_state_t s;
    aiot_device_state(t->mac, &s);
    EXPECT_GE(s.as_id, 0) << "Tag " << t->id << " has no AS ID";
  }
  std::set<int> data_from;
  for (auto &r : room.Csv("reader_rx.csv"))
    if (r.size() > 3 && r[2] == "data")
      data_from.insert(std::stoi(r[3]));
  EXPECT_EQ(data_from, (std::set<int>{0, 1, 2, 3}));
  // every R2D starts on a half-subframe of the node's TX slots
  auto tx = room.Csv("reader_tx.csv");
  ASSERT_GE(tx.size(), 2u);
  for (size_t i = 1; i < tx.size(); i++) {
    const double half_subframes = (std::stod(tx[i][0]) * kSlot - kClock) / AIOT_HALF_SUBFRAME;
    EXPECT_NEAR(half_subframes, std::round(half_subframes), 1e-6) << tx[i][0];
  }
}

// A protocol with its own table, and collisions: gen2_q writes q.csv as Amber does, and reads every
// Tag within 2 s of air.
TEST(NrAiotReader, Gen2QReadsEveryTagAndKeepsItsTable)
{
  Room room("gen2_q", "", 6, "gen2_q");
  ASSERT_NE(room.reader, nullptr);
  room.Run(2000);
  std::set<int> data_from;
  for (auto &r : room.Csv("reader_rx.csv"))
    if (r.size() > 3 && r[2] == "data")
      data_from.insert(std::stoi(r[3]));
  EXPECT_EQ(data_from, (std::set<int>{0, 1, 2, 3, 4, 5}));
  EXPECT_EQ(nr_aiot_reader_stats(room.reader).too_late, 0u);
  auto q = room.Csv("q.csv");
  ASSERT_GE(q.size(), 2u);
  EXPECT_EQ(q[0][0], "time_s");
}

// The UE of topology 2 sends what its Reader hears to the network: one datagram of Tag data for
// each row of reader_rx.csv that is a message from a Tag, in the same order, with the same values.
TEST(NrAiotReader, SendsEachTagMessageToTheDataDestination)
{
  const int sink = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sink, 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(bind(sink, (sockaddr *)&addr, sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(sink, (sockaddr *)&addr, &len), 0);
  const int big = 4 << 20;
  setsockopt(sink, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  const std::string dest = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));

  std::vector<std::vector<std::string>> rx;
  {
    Room room("registration", "", 4, "data_dest", 0, dest.c_str());
    ASSERT_NE(room.reader, nullptr);
    room.Run(600);
    nr_aiot_reader_free(room.reader); // what is left goes out too
    room.reader = nullptr;
    rx = room.Csv("reader_rx.csv");
  }
  std::vector<aiot_tag_data_t> got;
  uint8_t buf[AIOT_NODE_MSG_MAX_BYTES + 1];
  ssize_t n;
  while ((n = recv(sink, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
    aiot_tag_data_t m;
    ASSERT_EQ(aiot_node_msg_parse_tag_data(buf, (int)n, &m), 0) << n << " bytes";
    got.push_back(m);
  }
  close(sink);

  const std::map<std::string, int> kinds = {{"paging_cbra", AIOT_R2D_PAGING_CBRA},
                                            {"paging_cfa", AIOT_R2D_PAGING_CFA},
                                            {"access_trigger", AIOT_R2D_ACCESS_TRIGGER},
                                            {"random_id_response", AIOT_R2D_RANDOM_ID_RESPONSE},
                                            {"r2d_data", AIOT_R2D_UPPER_LAYER_DATA},
                                            {"nack_feedback", AIOT_R2D_NACK_FEEDBACK}};
  std::vector<std::vector<std::string>> heard;
  for (size_t i = 1; i < rx.size(); i++)
    if (rx[i][2] != "collision")
      heard.push_back(rx[i]);
  ASSERT_GT(heard.size(), 30u);
  ASSERT_EQ(got.size(), heard.size());
  int data = 0;
  for (size_t i = 0; i < got.size(); i++) {
    const aiot_tag_data_t &m = got[i];
    const aiot_reader_answer_t &a = m.answer;
    const std::vector<std::string> &row = heard[i];
    EXPECT_EQ(m.seq, i);
    EXPECT_EQ(a.start, std::llround(std::stod(row[0]) * kSlot)) << row[0];
    EXPECT_EQ(m.after, kinds.at(row[1])) << row[1];
    EXPECT_EQ(row[2], a.kind == AIOT_ANSWER_MSG1 ? "msg1" : "data");
    EXPECT_EQ(std::to_string(a.r_sfs), row[6]);
    EXPECT_EQ(std::to_string(a.slot), row[7]);
    EXPECT_NEAR(10 * std::log10(a.rssi_w / (32767.0 * 32767.0)), std::stod(row[8]), 0.005);
    if (a.kind == AIOT_ANSWER_MSG1) {
      EXPECT_EQ(std::to_string(a.message.random_id), row[5]);
      continue;
    }
    data++;
    ASSERT_EQ(a.message.sdu_len, 3);
    EXPECT_EQ(std::to_string(a.message.sdu[0] << 8 | a.message.sdu[1]), row[3]);
    EXPECT_EQ(std::to_string(a.message.sdu[2]), row[4]);
    EXPECT_EQ(a.as_id >= 0 ? std::to_string(a.as_id) : "", row[5]);
  }
  EXPECT_GT(data, 4);
}

// Nobody listens at the destination: each message is sent or counted as not sent, and every Tag
// still registers.
TEST(NrAiotReader, GoesOnWhenNobodyListensAtTheDataDestination)
{
  Room room("registration", "", 2, "data_dest_lost", 0, "0.0.0.0:9");
  ASSERT_NE(room.reader, nullptr);
  room.Run(300);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_GT(st.answers, 4u);
  EXPECT_EQ(st.data_sent + st.data_unsent, st.answers);
  for (auto &t : room.tags) {
    aiot_device_state_t s;
    aiot_device_state(t->mac, &s);
    EXPECT_GE(s.as_id, 0) << "Tag " << t->id << " has no AS ID";
  }
}

// Topology 3 with the R2D in the UE's UL: the gNB sends the UE each R2D, the UE sends it in its UL
// slot of the same number, and the gNB receives the D2R. Every Tag registers, every R2D reaches the UE
// in time, and the UE sends each one.
TEST(NrAiotReader, SplitEveryTagRegistersWithTheR2dFromTheUe)
{
  Room room("registration", "", 4, "split_r2d", 0, "", Split::kRemoteR2d);
  ASSERT_NE(room.reader, nullptr);
  room.Run(600);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(room.peer);
  EXPECT_GT(st.r2d_sent, 8u);
  EXPECT_EQ(st.too_late, 0u);
  EXPECT_EQ(st.r2d_refused, 0u);
  EXPECT_GE(st.r2d_done + 1, st.r2d_sent); // the last one's may still be on its way
  EXPECT_EQ(ps.r2ds, st.r2d_done);
  EXPECT_EQ(ps.r2d_too_late + ps.r2d_refused, 0u);
  EXPECT_EQ(room.ue_r2d.sent, st.r2d_done);
  EXPECT_GE(st.r2d_ahead_min, 1);
  EXPECT_EQ(st.collisions, 0u);
  for (auto &t : room.tags)
    EXPECT_EQ(t->late, 0) << "Tag " << t->id;
  room.ExpectEveryTagRegistered();
}

// A UE that maps its UL further ahead than the gNB's lead allows gets the first R2Ds too late: the
// one sent before it answered, which says nothing of the lead, and the next, which says by how much.
// The lead grows by that, the R2Ds after it are in time, and every Tag registers.
TEST(NrAiotReader, SplitLeadGrowsUntilTheUeGetsTheR2dInTime)
{
  Room room("registration", "", 4, "split_r2d_late", 0, "", Split::kRemoteR2d, 9);
  ASSERT_NE(room.reader, nullptr);
  room.Run(900);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_EQ(st.r2d_early_late, 1u);
  EXPECT_EQ(st.too_late, 1u);
  EXPECT_EQ(nr_aiot_peer_stats(room.peer).r2d_too_late, 2u);
  EXPECT_GT(st.r2d_sent, 8u);
  room.ExpectEveryTagRegistered();
}

// A UE that answers only from slot 350 on, as one that gets its user plane late: until then the gNB
// sends it one R2D every 100 slots, which reach it together and too late. They neither grow the lead
// nor count in the delay, and every Tag registers once the UE answers.
TEST(NrAiotReader, SplitSendsOneR2dAtATimeUntilTheUeAnswers)
{
  Room room("registration", "", 4, "split_r2d_up", 0, "", Split::kRemoteR2d);
  ASSERT_NE(room.reader, nullptr);
  room.serve = false;
  room.Run(350);
  const uint64_t early = nr_aiot_reader_stats(room.reader).r2d_sent;
  EXPECT_GE(early, 3u);
  EXPECT_LE(early, 4u);
  room.serve = true;
  room.Run(1200);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_EQ(st.r2d_early, early);
  EXPECT_EQ(st.r2d_early_late, early);
  EXPECT_EQ(nr_aiot_peer_stats(room.peer).r2d_too_late, early);
  EXPECT_EQ(st.too_late, 0u);
  EXPECT_GT(st.r2d_done, early + 8);
  EXPECT_GE(st.r2d_ahead_min, 1);
  EXPECT_LT(st.r2d_rtt_max, 3 * kSlot);
  room.ExpectEveryTagRegistered();
}

// Topology 3 with the D2R in the UE's DL: the gNB sends the R2D and the UE each window, in time of
// the cell; the UE opens it on its own clock and sends back each outcome. Every Tag registers, every
// window reaches the UE before its start and comes back, and each D2R is heard at the time it went on
// the air, on the gNB's clock.
TEST(NrAiotReader, SplitEveryTagRegistersWithTheD2rAtTheUe)
{
  Room room("registration", "", 4, "split_d2r", 0, "", Split::kRemoteD2r);
  ASSERT_NE(room.reader, nullptr);
  room.Run(600);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(room.peer);
  EXPECT_GT(st.r2d_sent, 8u);
  EXPECT_EQ(st.too_late, 0u);
  EXPECT_EQ(st.windows_lost, 0u);
  EXPECT_GT(st.windows_remote, 8u);
  EXPECT_LE(st.windows_remote - st.outcomes_remote, 8u); // the last R2D's may still be open
  EXPECT_EQ(ps.windows, st.windows_remote);
  EXPECT_EQ(ps.windows_late + ps.windows_refused, 0u);
  EXPECT_GT(st.window_ahead_min, 0);
  EXPECT_EQ(st.probes, 1u); // answered in the next slot: the protocol starts then
  EXPECT_EQ(ps.probes, 1u);
  EXPECT_EQ(st.outcomes_early, 0u); // no window is opened before the UE answered
  EXPECT_EQ(st.windows_late + st.windows_early_late, 0u);
  EXPECT_EQ(nr_aiot_d2r_stats(room.ue_d2r).late, 0u);
  EXPECT_EQ(st.collisions, 0u);
  const auto on_air = room.OnAir(580 * kSlot);
  int heard = 0;
  for (auto &r : room.Csv("reader_rx.csv"))
    if (r.size() > 6 && r[0] != "time_ms" && r[2] != "collision") {
      const int64_t at = std::llround(std::stod(r[0]) * kSlot);
      if (at < 580 * kSlot) {
        heard++;
        EXPECT_TRUE(on_air.count({at, std::stoi(r[6])})) << "heard a D2R no Tag sent at " << at;
      }
    }
  EXPECT_GT(heard, 30);
  room.ExpectEveryTagRegistered();
}

// A UE that serves the gNB only every 12 slots, longer than the windows' lead: the first windows
// that reach it after their start say by how much, the lead grows by that, and every Tag registers.
TEST(NrAiotReader, SplitLeadGrowsUntilTheWindowsReachTheUeInTime)
{
  Room room("registration", "", 4, "split_d2r_late", 0, "", Split::kRemoteD2r);
  ASSERT_NE(room.reader, nullptr);
  room.serve_every = 12;
  room.Run(1500);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(room.peer);
  EXPECT_GE(st.windows_late, 1u);
  EXPECT_LE(st.windows_late, 3u);
  EXPECT_EQ(ps.windows_late, st.windows_late + st.windows_early_late);
  EXPECT_EQ(st.windows_lost, 0u);
  room.ExpectEveryTagRegistered();
}

// A UE that answers only from slot 350 on, as one that gets its user plane late: until then the gNB
// sends no R2D, only a probe window every 100 slots, so that no Tag answers into a window the UE
// never gets. Once the UE answers one, the protocol starts and every Tag registers.
TEST(NrAiotReader, SplitSendsNoR2dUntilTheUeThatReceivesTheD2rAnswers)
{
  Room room("registration", "", 4, "split_d2r_up", 0, "", Split::kRemoteD2r);
  ASSERT_NE(room.reader, nullptr);
  room.serve = false;
  room.Run(350);
  nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_EQ(st.r2d_sent, 0u);
  EXPECT_EQ(st.windows_remote, 0u);
  EXPECT_EQ(st.probes, 4u); // slots 0, 100, 200 and 300
  room.serve = true;
  room.Run(1200);
  st = nr_aiot_reader_stats(room.reader);
  EXPECT_EQ(st.probes, 4u);
  EXPECT_GT(st.r2d_sent, 8u);
  EXPECT_EQ(st.windows_lost, 0u);
  EXPECT_EQ(st.outcomes_early, 0u);
  room.ExpectEveryTagRegistered();
}

// A UE that stops answering once the protocol has started: each window is given up as empty, a while
// after its end, and the protocol goes on.
TEST(NrAiotReader, SplitGivesUpTheWindowsWhoseOutcomeDoesNotComeBack)
{
  Room room("registration", "", 2, "split_d2r_lost", 0, "", Split::kRemoteD2r);
  ASSERT_NE(room.reader, nullptr);
  room.Run(20);
  const nr_aiot_reader_stats_t before = nr_aiot_reader_stats(room.reader);
  EXPECT_GE(before.r2d_sent, 1u);
  room.serve = false;
  room.Run(1000);
  const nr_aiot_reader_stats_t st = nr_aiot_reader_stats(room.reader);
  EXPECT_GE(st.r2d_sent, before.r2d_sent + 3);
  EXPECT_GE(st.windows_lost, 3u);
}

// The UE serves only what it has, and answers a window it cannot open with an empty outcome at once.
TEST(NrAiotReader, ThePeerServesOnlyWhatItHas)
{
  char err[256] = "";
  nr_aiot_r2d_t none;
  nr_aiot_r2d_config_t nc = {-1, 0, 0.0};
  ASSERT_EQ(nr_aiot_r2d_init(&nc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &none), 0);
  const nr_aiot_peer_config_t pc = {};
  EXPECT_EQ(nr_aiot_peer_new(&pc, &none, nullptr, err, sizeof(err)), nullptr);
  EXPECT_NE(std::string(err).find("neither"), std::string::npos) << err;
  nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
  nr_aiot_d2r_t *d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
  nr_aiot_peer_t *peer = nr_aiot_peer_new(&pc, &none, d2r, err, sizeof(err));
  ASSERT_NE(peer, nullptr) << err;

  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(nr_aiot_peer_port(peer));
  auto send = [&](const uint8_t *b, int n) { ASSERT_EQ(sendto(s, b, n, 0, (sockaddr *)&to, sizeof(to)), n); };
  const uint8_t junk[] = {1, 9, 0, 4};
  send(junk, sizeof(junk));
  aiot_node_r2d_t r2d = {};
  r2d.a_bits = 8;
  uint8_t buf[AIOT_NODE_SPLIT_MAX_BYTES];
  send(buf, aiot_node_msg_pack_r2d(&r2d, buf, sizeof(buf))); // the UE has no R2D PRBs
  aiot_node_window_t w = ValidWindow(42);
  send(buf, aiot_node_msg_pack_window(&w, buf, sizeof(buf))); // before the UE has received a slot
  aiot_node_window_t bad = ValidWindow(43);
  bad.expect.params.l_amble = 8; // no preamble the receiver has
  send(buf, aiot_node_msg_pack_window(&bad, buf, sizeof(buf)));
  EXPECT_EQ(StepPeer(peer, 4), 4);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(peer);
  EXPECT_EQ(ps.refused, 2u);
  EXPECT_EQ(ps.windows, 2u);
  EXPECT_EQ(ps.windows_refused, 2u);
  auto o = std::make_unique<aiot_node_outcome_t>();
  for (int tag : {42, 43}) { // each answered at once, empty
    const ssize_t n = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
    ASSERT_EQ(aiot_node_msg_parse_outcome(buf, (int)n, o.get()), 0);
    EXPECT_EQ(o->outcome.expect.tag, tag);
    EXPECT_EQ(o->outcome.n_decoded, 0);
  }
  EXPECT_LT(recv(s, buf, sizeof(buf), MSG_DONTWAIT), 0); // nothing for the junk or the R2D
  close(s);
  nr_aiot_peer_free(peer);
  nr_aiot_d2r_free(d2r);
  nr_aiot_r2d_free(&none);
}

// A UE given the gNB's address serves no one else.
TEST(NrAiotReader, ThePeerServesOnlyTheGnb)
{
  char err[256] = "";
  nr_aiot_r2d_t none;
  nr_aiot_r2d_config_t nc = {-1, 0, 0.0};
  ASSERT_EQ(nr_aiot_r2d_init(&nc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &none), 0);
  nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
  nr_aiot_d2r_t *d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
  const nr_aiot_peer_config_t pc = {.port = 0, .addr = "127.0.0.1", .reader = "127.0.0.2"};
  nr_aiot_peer_t *peer = nr_aiot_peer_new(&pc, &none, d2r, err, sizeof(err));
  ASSERT_NE(peer, nullptr) << err;
  const nr_aiot_peer_config_t bad = {.port = 0, .addr = "", .reader = "gnb"};
  EXPECT_EQ(nr_aiot_peer_new(&bad, &none, d2r, err, sizeof(err)), nullptr);

  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(nr_aiot_peer_port(peer));
  uint8_t buf[AIOT_NODE_SPLIT_MAX_BYTES];
  aiot_node_window_t w = ValidWindow(42);
  const int n = aiot_node_msg_pack_window(&w, buf, sizeof(buf));
  ASSERT_EQ(sendto(s, buf, n, 0, (sockaddr *)&to, sizeof(to)), n); // from 127.0.0.1
  EXPECT_EQ(StepPeer(peer, 1), 1);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(peer);
  EXPECT_EQ(ps.foreign, 1u);
  EXPECT_EQ(ps.windows, 0u);
  EXPECT_LT(recv(s, buf, sizeof(buf), MSG_DONTWAIT), 0);
  close(s);
  nr_aiot_peer_free(peer);
  nr_aiot_d2r_free(d2r);
  nr_aiot_r2d_free(&none);
}

// With more windows open than the UE keeps, the oldest one goes back to the Reader empty at once.
TEST(NrAiotReader, ThePeerAnswersItsOldestWindowWhenTooManyAreOpen)
{
  char err[256] = "";
  nr_aiot_r2d_t none;
  nr_aiot_r2d_config_t nc = {-1, 0, 0.0};
  ASSERT_EQ(nr_aiot_r2d_init(&nc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &none), 0);
  nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
  nr_aiot_d2r_t *d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
  nr_aiot_d2r_set_slot_time(d2r, 0, 0, 0); // the UE has received slot 0.0
  const nr_aiot_peer_config_t pc = {.port = 0, .addr = "127.0.0.1", .reader = "127.0.0.1"};
  nr_aiot_peer_t *peer = nr_aiot_peer_new(&pc, &none, d2r, err, sizeof(err));
  ASSERT_NE(peer, nullptr) << err;

  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(nr_aiot_peer_port(peer));
  uint8_t buf[AIOT_NODE_SPLIT_MAX_BYTES];
  const int kOpen = AIOT_NODE_MAX_WINDOWS + 1;
  for (int i = 0; i < kOpen; i++) {
    aiot_node_window_t w = ValidWindow(100 + i);
    w.at.frame = 10; // ahead of the receiver
    w.at.sample = i;
    const int n = aiot_node_msg_pack_window(&w, buf, sizeof(buf));
    ASSERT_EQ(sendto(s, buf, n, 0, (sockaddr *)&to, sizeof(to)), n);
  }
  EXPECT_EQ(StepPeer(peer, kOpen), kOpen);
  const nr_aiot_peer_stats_t ps = nr_aiot_peer_stats(peer);
  EXPECT_EQ(ps.windows, (uint64_t)kOpen);
  EXPECT_EQ(ps.windows_evicted, 1u);
  EXPECT_EQ(ps.windows_refused, 1u);
  auto o = std::make_unique<aiot_node_outcome_t>();
  const ssize_t n = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
  ASSERT_EQ(aiot_node_msg_parse_outcome(buf, (int)n, o.get()), 0);
  EXPECT_EQ(o->outcome.expect.tag, 100);
  EXPECT_EQ(o->outcome.n_decoded, 0);
  EXPECT_LT(recv(s, buf, sizeof(buf), MSG_DONTWAIT), 0);
  close(s);
  nr_aiot_peer_free(peer);
  nr_aiot_d2r_free(d2r);
  nr_aiot_r2d_free(&none);
}

// A lead past the most the Reader grows it to is refused.
TEST(NrAiotReader, RefusesALeadTooLong)
{
  nr_aiot_r2d_t r2d;
  nr_aiot_r2d_config_t rc = {0, 3, 0.0};
  ASSERT_EQ(nr_aiot_r2d_init(&rc, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &r2d), 0);
  nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
  nr_aiot_d2r_t *d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
  nr_aiot_reader_config_t cfg = {};
  snprintf(cfg.protocol, sizeof(cfg.protocol), "registration");
  cfg.link = RoomLink();
  cfg.node_ids[cfg.n_nodes++] = 0;
  char err[256] = "";
  for (int lead : {-1, NR_AIOT_READER_MAX_LEAD_SLOTS + 1}) {
    cfg.lead_slots = lead;
    EXPECT_EQ(nr_aiot_reader_new(&cfg, &r2d, d2r, err, sizeof(err)), nullptr) << lead;
    EXPECT_NE(std::string(err).find("aiot.lead_slots"), std::string::npos) << err;
  }
  cfg.lead_slots = NR_AIOT_READER_MAX_LEAD_SLOTS;
  nr_aiot_reader_t *r = nr_aiot_reader_new(&cfg, &r2d, d2r, err, sizeof(err));
  EXPECT_NE(r, nullptr) << err;
  EXPECT_TRUE(r2d.clock); // the Reader plans on the node's slots
  nr_aiot_reader_free(r);
  nr_aiot_d2r_free(d2r);
  nr_aiot_r2d_free(&r2d);
}

// The options of this process from now on, as if it had been started with them (a death test's child).
void UseOptions(std::vector<const char *> options)
{
  options.insert(options.begin(), "test_nr_aiot_reader");
  uniqCfg->argc = (int)options.size();
  uniqCfg->argv = (char **)calloc(options.size() + 1, sizeof(char *));
  for (size_t i = 0; i < options.size(); i++)
    uniqCfg->argv[i] = strdup(options[i]);
  uniqCfg->argv_info = (uint32_t *)calloc(options.size() + 1, sizeof(uint32_t));
  dup2(STDERR_FILENO, STDOUT_FILENO); // the log, for the death test to match
}

// Without an aiot section, the node has no R2D, D2R, Reader nor peer.
TEST(NrAiotConfig, NoSectionNoAiot)
{
  nr_aiot_r2d_config_t r2d;
  nr_aiot_d2r_config_t d2r;
  nr_aiot_config_phy(&r2d, &d2r);
  EXPECT_LT(r2d.prb_start, 0);
  EXPECT_LT(d2r.prb_start, 0);
  nr_aiot_reader_t *reader = (nr_aiot_reader_t *)&r2d;
  nr_aiot_peer_t *peer = (nr_aiot_peer_t *)&d2r;
  nr_aiot_ue_from_config(nullptr, nullptr, &reader, &peer);
  EXPECT_EQ(reader, nullptr);
  EXPECT_EQ(peer, nullptr);
  EXPECT_EQ(nr_aiot_reader_from_config(nullptr, nullptr), nullptr);
}

// A section that does not fit stops the node with the reason, not an assertion.
TEST(NrAiotConfig, StopsTheNodeOnASectionThatDoesNotFit)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  nr_aiot_r2d_config_t r2d;
  nr_aiot_d2r_config_t d2r;
  nr_aiot_reader_t *reader;
  nr_aiot_peer_t *peer;
  EXPECT_EXIT(
      {
        UseOptions({"--aiot.r2d_prb_start", "0", "--aiot.r2d_n_rb", "0"});
        nr_aiot_config_phy(&r2d, &d2r);
      },
      testing::ExitedWithCode(EXIT_FAILURE),
      "give the R2D at least one PRB");
  EXPECT_EXIT(
      {
        UseOptions({"--aiot.protocol", "registration", "--aiot.nodes", "0", "--aiot.serve_port", "38392"});
        nr_aiot_ue_from_config(nullptr, nullptr, &reader, &peer);
      },
      testing::ExitedWithCode(EXIT_FAILURE),
      "either a Reader or the UE of a gNB's split Reader");
  EXPECT_EXIT(
      {
        UseOptions({"--aiot.serve_port", "38392", "--aiot.serve_reader", "gnb"});
        nr_aiot_ue_from_config(nullptr, nullptr, &reader, &peer);
      },
      testing::ExitedWithCode(EXIT_FAILURE),
      "A-IoT peer");
  EXPECT_EXIT(
      {
        UseOptions({"--aiot.protocol", "registration", "--aiot.nodes", "3-1"});
        nr_aiot_reader_from_config(nullptr, nullptr);
      },
      testing::ExitedWithCode(EXIT_FAILURE),
      "must list the Tags");
}

// A Reader with both ends of one of its links, or neither, is refused.
TEST(NrAiotReader, ASplitReaderHasEachLinkOnce)
{
  nr_aiot_r2d_t with_prbs, clock_only;
  nr_aiot_r2d_config_t a = {0, 3, 0.0}, b = {-1, 0, 0.0};
  ASSERT_EQ(nr_aiot_r2d_init(&a, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &with_prbs), 0);
  ASSERT_EQ(nr_aiot_r2d_init(&b, 10, kSym, kFft, kFft * 160 / 2048, kFft * 144 / 2048, kPrb, kAmp, &clock_only), 0);
  nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
  nr_aiot_d2r_t *d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
  auto refused = [&](nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *rx, const char *remote_r2d, const char *remote_d2r) {
    nr_aiot_reader_config_t cfg = {};
    snprintf(cfg.protocol, sizeof(cfg.protocol), "registration");
    cfg.link = RoomLink();
    cfg.node_ids[cfg.n_nodes++] = 0;
    EXPECT_EQ(nr_aiot_reader_parse_address(cfg.remote_r2d, remote_r2d), 0);
    EXPECT_EQ(nr_aiot_reader_parse_address(cfg.remote_d2r, remote_d2r), 0);
    char err[256] = "";
    nr_aiot_reader_t *r = nr_aiot_reader_new(&cfg, r2d, rx, err, sizeof(err));
    nr_aiot_reader_free(r);
    return r == nullptr && err[0];
  };
  EXPECT_FALSE(refused(&with_prbs, d2r, "", ""));
  EXPECT_FALSE(refused(&clock_only, d2r, "127.0.0.1:9", ""));
  EXPECT_FALSE(refused(&with_prbs, nullptr, "", "127.0.0.1:9"));
  EXPECT_TRUE(refused(&with_prbs, d2r, "127.0.0.1:9", "")); // R2D PRBs and a UE for the R2D
  EXPECT_TRUE(refused(&with_prbs, d2r, "", "127.0.0.1:9")); // a D2R receiver and a UE for the D2R
  EXPECT_TRUE(refused(&clock_only, d2r, "", "")); // no R2D at all
  EXPECT_TRUE(refused(&with_prbs, nullptr, "", "")); // no D2R at all
  nr_aiot_d2r_free(d2r);
  nr_aiot_r2d_free(&with_prbs);
  nr_aiot_r2d_free(&clock_only);
}

TEST(NrAiotReader, ParsesItsSettings)
{
  nr_aiot_reader_config_t c = {};
  EXPECT_EQ(nr_aiot_reader_parse_params(&c, "q0=4, max_rounds=20"), 0);
  ASSERT_EQ(c.n_params, 2);
  EXPECT_STREQ(c.params[1].name, "max_rounds");
  EXPECT_EQ(c.params[1].value, 20);
  EXPECT_EQ(nr_aiot_reader_parse_params(&c, "q0"), -1);
  EXPECT_EQ(nr_aiot_reader_parse_nodes(&c, "0-3,7"), 0);
  EXPECT_EQ(c.n_nodes, 5);
  EXPECT_EQ(c.node_ids[4], 7);
  EXPECT_EQ(nr_aiot_reader_parse_nodes(&c, "3-1"), -1);
  EXPECT_EQ(nr_aiot_reader_parse_r_sfs(&c, "8,1,4"), 0);
  EXPECT_EQ(c.link.n_r_sfs, 3);
  EXPECT_EQ(c.link.r_sfs[0], 1);
  EXPECT_EQ(c.link.r_sfs[2], 8);
  EXPECT_EQ(nr_aiot_reader_parse_t_bit(&c, "1/8"), 0);
  EXPECT_EQ(c.link.t_bit_num, 1);
  EXPECT_EQ(c.link.t_bit_den, 8);
  EXPECT_EQ(nr_aiot_reader_parse_t_bit(&c, "2"), 0);
  EXPECT_EQ(c.link.t_bit_num, 2);
  EXPECT_EQ(c.link.t_bit_den, 1);
  EXPECT_EQ(nr_aiot_reader_parse_t_bit(&c, "x"), -1);
  EXPECT_EQ(nr_aiot_reader_parse_data_dest(&c, "10.0.1.1:38391"), 0);
  EXPECT_STREQ(c.data_dest, "10.0.1.1:38391");
  EXPECT_EQ(nr_aiot_reader_parse_data_dest(&c, ""), 0);
  EXPECT_STREQ(c.data_dest, "");
  for (const char *bad :
       {"10.0.1.1", "10.0.1.1:", "10.0.1.1:0", "10.0.1.1:65536", "10.0.1:38391", "host:1", ":38391", "10.0.1.1:1x"})
    EXPECT_EQ(nr_aiot_reader_parse_data_dest(&c, bad), -1) << bad;
  EXPECT_EQ(nr_aiot_reader_parse_address(c.remote_r2d, "10.0.1.2:38392"), 0);
  EXPECT_STREQ(c.remote_r2d, "10.0.1.2:38392");
  EXPECT_EQ(nr_aiot_reader_parse_address(c.remote_d2r, "10.0.1.2"), -1);
  EXPECT_STREQ(c.remote_d2r, "");
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv); // first: it takes its own options out of argv
  logInit();
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  return RUN_ALL_TESTS();
}
