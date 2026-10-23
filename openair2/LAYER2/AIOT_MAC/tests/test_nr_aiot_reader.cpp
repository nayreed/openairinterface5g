/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// nr_aiot_reader.c: the gNB as an A-IoT Reader, closed over an air in samples. This is the room's
// topology 1 in one process: the gNB maps each DL slot 4 slots ahead of the air (as sl_ahead does),
// OFDM sends it, and each Tag decodes the R2D with its own receiver and runs the device MAC. The
// D2R each Tag sends is backscatter of the carrier, at the centre of UL PRBs 15-21, in int16 counts
// with noise, and the gNB's D2R receiver gets it slot by slot.

#include <gtest/gtest.h>
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
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}

namespace {

const int kSym = 14, kFft = AIOT_N_FFT, kPrb = 25, kAmp = 519, kSlot = AIOT_SAMPLES_PER_MS, kAhead = 4;
const double kShiftHz = 1080000.0, kCwCounts = 400, kTagCounts = 20;

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

struct Room {
  nr_aiot_r2d_t r2d;
  nr_aiot_d2r_t *d2r = nullptr;
  nr_aiot_reader_t *reader = nullptr;
  std::vector<std::unique_ptr<Tag>> tags;
  std::mt19937_64 gen{11};
  std::vector<std::complex<double>> dl; // the air's DL from sample 0
  std::string dir;
  int64_t slot = 0; // the air's slot: the UL the gNB receives next, the DL the Tags hear next

  Room(const char *protocol, const char *params, int n_tags, const char *name)
  {
    nr_aiot_r2d_config_t rc = {0, 3, 0.0};
    EXPECT_EQ(nr_aiot_r2d_init(&r2d, &rc, 10, kSym, kFft, kPrb, kAmp), 0);
    nr_aiot_d2r_config_t dc = {15, 7, 1.0, 1};
    d2r = nr_aiot_d2r_new(&dc, kPrb, AIOT_SCS_HZ, AIOT_SAMPLE_RATE_HZ);
    EXPECT_NE(d2r, nullptr);
    nr_aiot_reader_config_t cfg = {};
    snprintf(cfg.protocol, sizeof(cfg.protocol), "%s", protocol);
    EXPECT_EQ(nr_aiot_reader_parse_params(&cfg, params), 0);
    cfg.link = RoomLink();
    dir = std::string("/tmp/test_nr_aiot_reader_") + name;
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", dir.c_str());
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
    nr_aiot_d2r_free(d2r);
    nr_aiot_r2d_free(&r2d);
    for (auto &t : tags) {
      aiot_device_free(t->mac);
      aiot_r2d_rx_free(t->rx);
    }
  }

  // One slot of air: the gNB maps the DL slot kAhead ahead, the Tags hear this slot's DL and
  // answer, the gNB receives this slot's UL, and the Reader takes its step.
  void Step()
  {
    const int64_t tx = slot + kAhead;
    std::vector<c16_t> grid(kSym * kFft, c16_t{0, 0});
    nr_aiot_r2d_map(&r2d, (int)((tx / 10) % 1024), (int)(tx % 10), grid.data(), nullptr);
    dl.resize((tx + 1) * kSlot);
    Modulate(grid, dl.data() + tx * kSlot);

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
    ASSERT_EQ(nr_aiot_d2r_feed(d2r, t0, c.data(), kSlot), 0);
    nr_aiot_reader_step(reader);
    slot++;
  }

  int Run(int max_slots)
  {
    while (slot < max_slots && !nr_aiot_reader_done(reader))
      Step();
    return (int)slot;
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
}

int main(int argc, char **argv)
{
  logInit();
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
