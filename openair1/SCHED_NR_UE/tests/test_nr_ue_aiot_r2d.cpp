/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// The UE as an A-IoT Reader sends R2D in its uplink: through the UE's own TX procedure
// (phy_procedures_nrUE_TX), its phase rotation and its OFDM. The UE is the room's: 25 PRBs at 15 kHz
// in a 384-point symbol (5.76 Msps), FDD band n28.

#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include <vector>
extern "C" {
#include "nr_ue_tx.h"
#include "PHY/AIOT/aiot_phy.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}

namespace {

const int kSlotsPerFrame = 10, kSym = 14, kSlotSamples = AIOT_SAMPLES_PER_MS; // 15 kHz at 5.76 Msps

struct Ue {
  nr_ue_tx_t *t;
  explicit Ue(int prb_start, int n_rb_max = 3)
  {
    nr_aiot_r2d_config_t cfg = {prb_start, n_rb_max, 0, 0};
    t = nr_ue_tx_new(&cfg);
  }
  ~Ue()
  {
    nr_ue_tx_free(t);
  }
  nr_aiot_r2d_t *r2d()
  {
    return nr_ue_tx_r2d(t);
  }
};

std::vector<uint8_t> RandomBits(std::mt19937 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

// The UE's samples of slots first to last - 1 (counted from slot 0 of frame 0): each slot taken in
// order, as processSlotTX() does before its barrier, then sent in order or in reverse, as the UE's
// TX threads may.
std::vector<c16_t> Send(Ue &ue, int first, int last, bool reverse = false)
{
  std::vector<nr_ue_tx_slot_t *> taken;
  for (int a = first; a < last; a++)
    taken.push_back(nr_ue_tx_take(ue.t, a / kSlotsPerFrame, a % kSlotsPerFrame));
  std::vector<c16_t> air(taken.size() * kSlotSamples, c16_t{0, 0});
  for (size_t k = 0; k < taken.size(); k++) {
    const size_t i = reverse ? taken.size() - 1 - k : k;
    EXPECT_EQ(nr_ue_tx_send(ue.t, taken[i], air.data() + i * kSlotSamples), kSlotSamples);
  }
  return air;
}

std::vector<aiot_r2d_frame_t> Decode(const std::vector<c16_t> &air)
{
  std::vector<cd_t> y(air.size());
  for (size_t i = 0; i < air.size(); i++)
    y[i] = cd_t{(double)air[i].r, (double)air[i].i};
  aiot_r2d_rx_t *rx = aiot_r2d_rx_new();
  std::vector<aiot_r2d_frame_t> fr(4);
  int found = 0;
  for (size_t p = 0; p + kSlotSamples <= y.size(); p += kSlotSamples) {
    const int k = aiot_r2d_rx_receive(rx, y.data() + p, kSlotSamples, fr.data() + found, (int)fr.size() - found);
    EXPECT_GE(k, 0);
    found += std::max(k, 0);
  }
  aiot_r2d_rx_free(rx);
  fr.resize(found);
  return fr;
}

bool Silent(const std::vector<c16_t> &air, size_t from, size_t to)
{
  return std::all_of(air.begin() + from, air.begin() + to, [](c16_t v) { return v.r == 0 && v.i == 0; });
}

} // namespace

// What the UE sends is what a device decodes: the transport block, the R2D ending where its last
// symbol ends, and nothing outside the R2D. Every M, at the bottom, middle and top of the carrier.
TEST(NrUeAiotR2D, DeviceDecodesWhatTheUeSends)
{
  std::mt19937 gen(1);
  for (int m : {2, 6, 12, 24})
    for (int prb_start : {0, 11, NR_UE_TX_N_RB - 3}) {
      Ue ue(prb_start);
      std::vector<uint8_t> tb = RandomBits(gen, 40);
      const int n_sym = nr_aiot_r2d_schedule(ue.r2d(), 0, 1, 7, tb.data(), 40, m, aiot_n_rb_min(m));
      ASSERT_GT(n_sym, 0);
      const int last = 1 + (7 + n_sym + kSym - 1) / kSym;
      const std::vector<c16_t> air = Send(ue, 0, last + 1);
      const std::vector<aiot_r2d_frame_t> fr = Decode(air);
      ASSERT_EQ(fr.size(), 1u) << "M = " << m << ", PRB " << prb_start;
      EXPECT_EQ(fr[0].n_bits, 40);
      EXPECT_TRUE(std::equal(tb.begin(), tb.end(), fr[0].bits));
      // starts at symbol 7 of slot 1: one slot (5760 samples) and the first half-subframe (2880)
      const size_t start = kSlotSamples + AIOT_HALF_SUBFRAME, end = start + aiot_r2d_n_samples(n_sym);
      EXPECT_EQ(fr[0].end_sample, (int64_t)end);
      EXPECT_TRUE(Silent(air, 0, start)) << "the UE sends nothing else";
      EXPECT_TRUE(Silent(air, end, air.size()));
      EXPECT_EQ(ue.r2d()->sent, 1u);
      EXPECT_EQ(ue.r2d()->collisions, 0u);
    }
}

// The UE's TX threads build the slots in parallel, in any order, once each was taken in order: the
// samples are the same as one by one.
TEST(NrUeAiotR2D, SlotsTakenInOrderAreSentInAnyOrder)
{
  std::mt19937 gen(2);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  std::vector<c16_t> air[2];
  for (int reverse : {0, 1}) {
    Ue ue(11);
    const int n_sym = nr_aiot_r2d_schedule(ue.r2d(), 0, 2, 0, tb.data(), 96, 6, 1);
    ASSERT_GT(n_sym, 2 * kSym) << "spans more than two slots";
    air[reverse] = Send(ue, 0, 2 + (n_sym + kSym - 1) / kSym + 1, reverse);
    EXPECT_EQ(ue.r2d()->sent, 1u);
    EXPECT_EQ(ue.r2d()->dropped, 0u);
  }
  ASSERT_EQ(air[0].size(), air[1].size());
  for (size_t i = 0; i < air[0].size(); i++)
    ASSERT_TRUE(air[0][i].r == air[1][i].r && air[0][i].i == air[1][i].i) << "sample " << i;
  const std::vector<aiot_r2d_frame_t> fr = Decode(air[1]);
  ASSERT_EQ(fr.size(), 1u);
  EXPECT_TRUE(std::equal(tb.begin(), tb.end(), fr[0].bits));
}

// With TDD the UE sends R2D only in slots whose symbols are all uplink: one that would span a
// downlink or mixed slot is refused when it is scheduled. With FDD, every slot.
TEST(NrUeAiotR2D, SendsOnlyInUplinkSlots)
{
  // the shortest transport block whose R2D, with M = 24, is longer than half a slot
  std::mt19937 gen(3);
  const int m = 24, n_rb = aiot_n_rb_min(m);
  std::vector<uint8_t> tb, chips(4096);
  int n_sym = 0;
  while (n_sym <= kSym / 2) {
    tb = RandomBits(gen, tb.size() + 8);
    n_sym = aiot_r2d_n_symbols(aiot_r2d_chips(tb.data(), tb.size(), m, chips.data(), chips.size()), m);
  }
  ASSERT_LE(n_sym, kSym) << "fits in one slot";
  const int bits = tb.size();

  Ue ue(0);
  EXPECT_EQ(nr_aiot_r2d_schedule(ue.r2d(), 0, 3, 0, tb.data(), bits, m, n_rb), n_sym) << "FDD: slot 3 is uplink";

  const int dddsu[5] = {0, 0, 0, 4, 14}; // uplink symbols of each slot
  nr_ue_tx_set_tdd(ue.t, 5, dddsu);
  EXPECT_EQ(nr_aiot_r2d_schedule(ue.r2d(), 0, 8, 0, tb.data(), bits, m, n_rb), AIOT_EINVAL) << "slot 8 is mixed";
  EXPECT_EQ(nr_aiot_r2d_schedule(ue.r2d(), 0, 9, 7, tb.data(), bits, m, n_rb), AIOT_EINVAL)
      << "it runs on into slot 0 of frame 1, downlink";
  EXPECT_EQ(nr_aiot_r2d_schedule(ue.r2d(), 0, 9, 0, tb.data(), bits, m, n_rb), n_sym) << "slot 9 is uplink";
  EXPECT_EQ(nr_aiot_r2d_schedule(ue.r2d(), 1, 4, 0, tb.data(), bits, m, n_rb), n_sym) << "and slot 4 of the next frame";
}

int main(int argc, char **argv)
{
  logInit();
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
