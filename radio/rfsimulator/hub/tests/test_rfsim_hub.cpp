/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <gtest/gtest.h>
#include <unistd.h>
#include <cmath>
#include <complex>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "common_lib.h"

extern "C" {
#include "common/config/config_userapi.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "openair2/LAYER2/NR_MAC_gNB/mac_config.h"
#include "radio/rfsimulator/hub/rfsim_hub.h"
extern int device_init(openair0_device_t *device, openair0_config_t *openair0_cfg);
configmodule_interface_t *uniqCfg = NULL;
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert_flag)
{
  (void)assert_flag;
  fprintf(stderr, "FATAL: %s at %s:%s:%d\n", s, file, function, line);
  exit(EXIT_FAILURE);
}
// apply_channelmod.c needs this symbol, normally provided by the gNB MAC library.
bool nr_update_sib19(const gnb_sat_position_update_t *sat_position)
{
  (void)sat_position;
  return true;
}
}

namespace {

const int64_t kQuantum = 256;
const int64_t kHistory = 1 << 14;
const double kSampleRate = 1.92e6;

struct Room {
  std::vector<hub_node_conf_t> nodes;
  std::vector<hub_link_conf_t> links;
  hub_conf_t conf = {};
  std::string why;

  int Node(const char *name, int64_t lookahead, bool lead = false, int port = 0)
  {
    hub_node_conf_t n = {};
    snprintf(n.name, sizeof(n.name), "%s", name);
    n.port = port ? port : 30000 + static_cast<int>(nodes.size());
    n.lookahead = lookahead;
    n.lead = lead;
    nodes.push_back(n);
    return static_cast<int>(nodes.size()) - 1;
  }
  void Link(int tx, int rx, double gain_dB = 0, int64_t delay = 0, double shift_Hz = 0)
  {
    links.push_back({tx, rx, gain_dB, delay, shift_Hz});
  }
  hub_room_t *Create(int64_t history = kHistory)
  {
    conf = {kSampleRate, kQuantum, history, static_cast<int>(nodes.size()), nodes.data(), static_cast<int>(links.size()), links.data()};
    char buf[256] = "";
    hub_room_t *room = hub_room_create(&conf, buf, sizeof(buf));
    why = buf;
    return room;
  }
};

// A ramp that differs per node, so a sample from the wrong node or the wrong time shows.
c16_t Pattern(int node, int64_t t)
{
  return {static_cast<int16_t>((t * 7 + node * 1000) % 4000 - 2000), static_cast<int16_t>((t * 3 + node * 500) % 3000 - 1500)};
}

std::vector<c16_t> Block(int node, int64_t start, int64_t n)
{
  std::vector<c16_t> b(n);
  for (int64_t i = 0; i < n; i++)
    b[i] = Pattern(node, start + i);
  return b;
}

std::vector<c16_t> NextBlock(hub_room_t *room, int node, uint64_t expected_ts)
{
  std::vector<c16_t> out(kQuantum);
  uint64_t ts = 0;
  EXPECT_TRUE(hub_room_next_block(room, node, &ts, out.data()));
  EXPECT_EQ(ts, expected_ts);
  return out;
}

void ExpectNear(const c16_t &got, std::complex<double> want, int64_t t)
{
  EXPECT_NEAR(got.r, want.real(), 1.0) << "sample " << t;
  EXPECT_NEAR(got.i, want.imag(), 1.0) << "sample " << t;
}

} // namespace

TEST(HubRoom, RefusesACycleWithoutLookahead)
{
  Room r;
  int reader = r.Node("reader", kQuantum);
  int tag1 = r.Node("tag1", 0);
  int tag2 = r.Node("tag2", 0);
  r.Link(reader, tag1);
  r.Link(reader, tag2);
  r.Link(tag1, reader);
  r.Link(tag2, reader);
  r.Link(tag1, tag2);
  r.Link(tag2, tag1);
  EXPECT_EQ(r.Create(), nullptr);
  EXPECT_NE(r.why.find("cycle"), std::string::npos) << r.why;
}

TEST(HubRoom, AcceptsTheSameCycleWithOneBlockOfDelay)
{
  Room r;
  int reader = r.Node("reader", kQuantum);
  int tag1 = r.Node("tag1", 0);
  int tag2 = r.Node("tag2", 0);
  r.Link(reader, tag1);
  r.Link(reader, tag2);
  r.Link(tag1, reader);
  r.Link(tag2, reader);
  r.Link(tag1, tag2, -60, kQuantum);
  r.Link(tag2, tag1, -60);
  hub_room_t *room = r.Create();
  EXPECT_NE(room, nullptr) << r.why;
  hub_room_free(room);
}

TEST(HubRoom, RefusesRoomsThatCannotRun)
{
  {
    Room r;
    int a = r.Node("a", kQuantum);
    r.Node("deaf", 0);
    r.Link(a, a);
    EXPECT_EQ(r.Create(), nullptr);
    EXPECT_NE(r.why.find("hears nobody"), std::string::npos) << r.why;
  }
  {
    Room r;
    int a = r.Node("a", kQuantum, false, 4000);
    int b = r.Node("b", kQuantum, false, 4000);
    r.Link(a, b);
    r.Link(b, a);
    EXPECT_EQ(r.Create(), nullptr);
    EXPECT_NE(r.why.find("port"), std::string::npos) << r.why;
  }
  {
    Room r;
    int a = r.Node("a", kHistory);
    int b = r.Node("b", kQuantum);
    r.Link(a, b);
    r.Link(b, a);
    EXPECT_EQ(r.Create(), nullptr);
    EXPECT_NE(r.why.find("too short"), std::string::npos) << r.why;
    EXPECT_EQ(r.Create(kHistory + 1), nullptr);
  }
}

// A node's receiver noise: Gaussian, of the rms it asks for on I and on Q, on top of what it hears,
// and the same however the blocks are cut. Another node hears none.
TEST(HubRoom, AddsTheReceiversNoise)
{
  Room r;
  int a = r.Node("a", kQuantum);
  int rx = r.Node("rx", kQuantum);
  r.nodes[rx].noise_counts = 3.0;
  r.Link(a, rx);
  r.Link(rx, a);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;
  const int64_t len = 32 * kQuantum;
  hub_room_write(room, a, 0, Block(a, 0, len).data(), len, 0);
  hub_room_write(room, rx, 0, std::vector<c16_t>(len, c16_t{0, 0}).data(), len, 0);
  double sum_r = 0, sum_i = 0, sq_r = 0, sq_i = 0, cross = 0;
  std::vector<double> noise_r;
  for (int64_t start = 0; start < len; start += kQuantum) {
    std::vector<c16_t> out = NextBlock(room, rx, start);
    for (int64_t i = 0; i < kQuantum; i++) {
      const double nr = out[i].r - Pattern(a, start + i).r, ni = out[i].i - Pattern(a, start + i).i;
      sum_r += nr, sum_i += ni, sq_r += nr * nr, sq_i += ni * ni, cross += nr * ni;
      noise_r.push_back(nr);
    }
    std::vector<c16_t> back = NextBlock(room, a, start);
    for (int64_t i = 0; i < kQuantum; i++)
      ASSERT_TRUE(back[i].r == 0 && back[i].i == 0) << "a has no noise";
  }
  const double n = (double)len;
  EXPECT_NEAR(sum_r / n, 0, 0.15);
  EXPECT_NEAR(sum_i / n, 0, 0.15);
  // the rounding to counts adds 1/12 to each variance
  EXPECT_NEAR(std::sqrt(sq_r / n), std::sqrt(9 + 1.0 / 12), 0.1);
  EXPECT_NEAR(std::sqrt(sq_i / n), std::sqrt(9 + 1.0 / 12), 0.1);
  EXPECT_NEAR(cross / n, 0, 0.4) << "I and Q independent";
  double lag = 0;
  for (size_t i = 1; i < noise_r.size(); i++)
    lag += noise_r[i] * noise_r[i - 1];
  EXPECT_NEAR(lag / n, 0, 0.4) << "white";
  hub_room_free(room);

  // the same room again, read in other blocks' worth of writes: the same noise
  Room r2 = r;
  hub_room_t *again = r2.Create();
  ASSERT_NE(again, nullptr);
  for (int64_t t = 0; t < len; t += 100) {
    const int64_t k = std::min<int64_t>(100, len - t);
    hub_room_write(again, a, t, Block(a, t, k).data(), k, 0);
  }
  hub_room_write(again, rx, 0, std::vector<c16_t>(len, c16_t{0, 0}).data(), len, 0);
  for (int64_t start = 0; start < len; start += kQuantum) {
    std::vector<c16_t> out = NextBlock(again, rx, start);
    for (int64_t i = 0; i < kQuantum; i++)
      ASSERT_EQ(out[i].r - Pattern(a, start + i).r, noise_r[start + i]);
    NextBlock(again, a, start);
  }
  hub_room_free(again);
}

TEST(HubRoom, MixesTheSumWithGainAndDelay)
{
  Room r;
  int a = r.Node("a", kQuantum);
  int b = r.Node("b", kQuantum);
  int rx = r.Node("rx", kQuantum);
  const int64_t delay = 37;
  const double gain_dB = -6;
  r.Link(a, rx, gain_dB, delay);
  r.Link(b, rx);
  r.Link(rx, a);
  r.Link(rx, b);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  const int64_t len = 8 * kQuantum;
  // split writes that do not line up with the hub's blocks
  for (int64_t t = 0; t < len; t += 100) {
    int64_t n = std::min<int64_t>(100, len - t);
    hub_room_write(room, a, t, Block(a, t, n).data(), n, 0);
  }
  hub_room_write(room, b, 0, Block(b, 0, len).data(), len, 0);

  const double g = std::pow(10.0, gain_dB / 20);
  for (int64_t start = 0; start + kQuantum <= len; start += kQuantum) {
    std::vector<c16_t> out = NextBlock(room, rx, start);
    for (int64_t i = 0; i < kQuantum; i++) {
      int64_t t = start + i;
      std::complex<double> want(Pattern(b, t).r, Pattern(b, t).i);
      if (t >= delay)
        want += g * std::complex<double>(Pattern(a, t - delay).r, Pattern(a, t - delay).i);
      ExpectNear(out[i], want, t);
    }
  }
  // the next block needs samples nobody has sent yet
  c16_t out[kQuantum];
  uint64_t ts;
  EXPECT_FALSE(hub_room_next_block(room, rx, &ts, out));
  hub_room_free(room);
}

TEST(HubRoom, ShiftsFrequencyContinuouslyAcrossBlocks)
{
  Room r;
  int tx = r.Node("tx", kQuantum);
  int rx = r.Node("rx", kQuantum);
  const double shift_Hz = 123456.7;
  r.Link(tx, rx, 0, 0, shift_Hz);
  r.Link(rx, tx);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  const int64_t len = 6 * kQuantum;
  std::vector<c16_t> constant(len, c16_t{1000, 0});
  hub_room_write(room, tx, 0, constant.data(), len, 0);
  for (int64_t start = 0; start < len; start += kQuantum) {
    std::vector<c16_t> out = NextBlock(room, rx, start);
    for (int64_t i = 0; i < kQuantum; i++) {
      int64_t t = start + i;
      ExpectNear(out[i], std::polar(1000.0, 2 * M_PI * shift_Hz * t / kSampleRate), t);
    }
  }
  hub_room_free(room);
}

TEST(HubRoom, AppliesTheSendersGainAndSaturates)
{
  Room r;
  int a = r.Node("a", kQuantum);
  int b = r.Node("b", kQuantum);
  int rx = r.Node("rx", kQuantum);
  r.Link(a, rx);
  r.Link(b, rx);
  r.Link(rx, a);
  r.Link(rx, b);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  std::vector<c16_t> loud(kQuantum, c16_t{30000, -30000});
  std::vector<c16_t> other(kQuantum, c16_t{20000, 2000});
  hub_room_write(room, a, 0, loud.data(), kQuantum, 0);
  hub_room_write(room, b, 0, other.data(), kQuantum, -6.0206); // the sender's own gain (one half), as rfsim sends it
  std::vector<c16_t> out = NextBlock(room, rx, 0);
  EXPECT_EQ(out[0].r, 32767);
  EXPECT_EQ(out[0].i, -29000);
  hub_room_free(room);
}

TEST(HubRoom, CountsWhatALeadNodeDoesNotSendAsSilence)
{
  Room r;
  int gnb = r.Node("gnb", 4 * kQuantum, true);
  int ue = r.Node("ue", 2 * kQuantum, true);
  r.Link(gnb, ue);
  r.Link(ue, gnb);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  // nobody has sent anything, yet each node can be given its first block, as an rfsim server
  // gives a new client's first samples as silence
  for (int node : {gnb, ue}) {
    std::vector<c16_t> out = NextBlock(room, node, 0);
    for (const c16_t &s : out)
      EXPECT_EQ(s.r | s.i, 0);
  }
  hub_room_free(room);
}

TEST(HubRoom, AGapIsSilenceAndThePastIsIgnored)
{
  Room r;
  int tx = r.Node("tx", kQuantum);
  int rx = r.Node("rx", kQuantum);
  r.Link(tx, rx);
  r.Link(rx, tx);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  // fill the whole history once, so a gap has old samples to hide
  for (int64_t t = 0; t < kHistory; t += kQuantum) {
    hub_room_write(room, tx, t, Block(tx, t, kQuantum).data(), kQuantum, 0);
    NextBlock(room, rx, t);
  }
  const int64_t gap_end = kHistory + 3 * kQuantum + 10;
  hub_room_write(room, tx, gap_end, Block(tx, gap_end, kQuantum).data(), kQuantum, 0);
  // samples before what tx already sent are dropped
  std::vector<c16_t> late(kQuantum, c16_t{999, 999});
  hub_room_write(room, tx, kHistory, late.data(), kQuantum, 0);
  for (int64_t start = kHistory; start + kQuantum <= gap_end + kQuantum; start += kQuantum) {
    std::vector<c16_t> out = NextBlock(room, rx, start);
    for (int64_t i = 0; i < kQuantum; i++) {
      int64_t t = start + i;
      c16_t want = t < gap_end ? c16_t{0, 0} : Pattern(tx, t);
      EXPECT_EQ(out[i].r, want.r) << "sample " << t;
      EXPECT_EQ(out[i].i, want.i) << "sample " << t;
    }
  }
  hub_room_free(room);
}

TEST(HubRoom, StopsWaitingForANodeThatHasLeft)
{
  Room r;
  int reader = r.Node("reader", kQuantum);
  int tag1 = r.Node("tag1", 0);
  int tag2 = r.Node("tag2", 0);
  r.Link(reader, tag1);
  r.Link(reader, tag2);
  r.Link(tag1, reader);
  r.Link(tag2, reader);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  hub_room_write(room, reader, 0, Block(reader, 0, kQuantum).data(), kQuantum, 0);
  hub_room_write(room, tag1, 0, Block(tag1, 0, kQuantum).data(), kQuantum, 0);
  c16_t out[kQuantum];
  uint64_t ts;
  EXPECT_FALSE(hub_room_next_block(room, reader, &ts, out)) << "tag2 has sent nothing yet";
  hub_room_leave(room, tag2);
  EXPECT_TRUE(hub_room_next_block(room, reader, &ts, out));
  EXPECT_FALSE(hub_room_hears_nobody(room, reader));
  hub_room_leave(room, tag1);
  EXPECT_TRUE(hub_room_hears_nobody(room, reader));
  hub_room_free(room);
}

TEST(HubRoom, HoldsBackANodeThatWouldOverwriteWhatIsStillNeeded)
{
  Room r;
  int tx = r.Node("tx", kQuantum);
  int rx = r.Node("rx", kQuantum);
  r.Link(tx, rx);
  r.Link(rx, tx);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;

  int64_t t = 0;
  for (; !hub_room_must_wait(room, tx); t += kQuantum)
    hub_room_write(room, tx, t, Block(tx, t, kQuantum).data(), kQuantum, 0);
  EXPECT_GT(t, kHistory / 2);
  EXPECT_LE(t, kHistory / 2 + kQuantum);
  NextBlock(room, rx, 0);
  NextBlock(room, rx, kQuantum);
  EXPECT_FALSE(hub_room_must_wait(room, tx));
  hub_room_free(room);
}

namespace {

uint16_t PickPort()
{
  static int salt = 0;
  return static_cast<uint16_t>(24000 + ((getpid() + salt++ * 31) % 10000));
}

configmodule_interface_t *StartClient(openair0_device_t &device, openair0_config_t &config, uint16_t port)
{
  std::vector<std::string> args = {"test_rfsim_hub", "--rfsimulator.serveraddr", "127.0.0.1", "--rfsimulator.serverport", std::to_string(port)};
  std::vector<const char *> argv;
  for (const auto &a : args)
    argv.push_back(a.c_str());
  configmodule_interface_t *cfg = load_configmodule(argv.size(), (char **)argv.data(), CONFIG_ENABLECMDLINEONLY);
  uniqCfg = cfg;
  config.tx_num_channels = 1;
  config.rx_num_channels = 1;
  config.sample_rate = kSampleRate;
  device_init(&device, &config);
  return cfg;
}

} // namespace

// Two stock rfsim clients through a running hub: what one sends reaches the other with the link's
// gain and delay, sample for sample.
TEST(HubServe, CarriesSamplesBetweenRfsimClients)
{
  const int64_t block = kQuantum;
  const int64_t ahead = 2 * block; // each client writes this far ahead of what it has read
  const int64_t delay = 50;
  const int blocks = 40;
  Room r;
  int tx = r.Node("tx", ahead, true, PickPort());
  int rx = r.Node("rx", ahead, true, PickPort());
  r.Link(tx, rx, -6, delay);
  r.Link(rx, tx, -200);
  hub_room_t *room = r.Create();
  ASSERT_NE(room, nullptr) << r.why;
  std::thread hub([room] { EXPECT_EQ(hub_serve(room, 10), 0); });
  usleep(100000);

  openair0_device_t dev[2] = {};
  openair0_config_t cfg[2] = {};
  configmodule_interface_t *cm[2] = {StartClient(dev[0], cfg[0], r.nodes[tx].port), StartClient(dev[1], cfg[1], r.nodes[rx].port)};
  std::vector<std::pair<openair0_timestamp_t, std::vector<c16_t>>> received;
  openair0_timestamp_t tx_first_write = 0;
  std::thread clients[2];
  for (int k = 0; k < 2; k++) {
    clients[k] = std::thread([&, k] {
      ASSERT_EQ(dev[k].trx_start_func(&dev[k]), 0);
      std::vector<c16_t> in(block);
      void *in_p[1] = {in.data()};
      for (int b = 0; b < blocks; b++) {
        openair0_timestamp_t ts;
        ASSERT_EQ(dev[k].trx_read_func(&dev[k], &ts, in_p, block, 1), block);
        if (k == 1)
          received.emplace_back(ts, in);
        if (k == 0 && b == 0)
          tx_first_write = ts + ahead;
        std::vector<c16_t> out = Block(k == 0 ? tx : rx, ts + ahead, block);
        void *out_p[1] = {out.data()};
        dev[k].trx_write_func(&dev[k], ts + ahead, out_p, block, 1, 0);
      }
    });
  }
  for (auto &c : clients)
    c.join();
  for (int k = 0; k < 2; k++) {
    dev[k].trx_end_func(&dev[k]);
    end_configmodule(cm[k]);
  }
  hub.join();
  hub_room_free(room);

  const double g = std::pow(10.0, -6.0 / 20);
  ASSERT_EQ(received.size(), static_cast<size_t>(blocks));
  for (int b = 0; b < blocks; b++) {
    EXPECT_EQ(received[b].first, received[0].first + b * block) << "the reads follow each other";
    for (int64_t i = 0; i < block; i++) {
      int64_t t = received[b].first + i;
      std::complex<double> want = 0;
      if (t - delay >= static_cast<int64_t>(tx_first_write))
        want = g * std::complex<double>(Pattern(tx, t - delay).r, Pattern(tx, t - delay).i);
      ExpectNear(received[b].second[i], want, t);
    }
  }
  EXPECT_LT(received[0].first + blocks * block, tx_first_write + blocks * block) << "the check covered samples tx sent";
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
