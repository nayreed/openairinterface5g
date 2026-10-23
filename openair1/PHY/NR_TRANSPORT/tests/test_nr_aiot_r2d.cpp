/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// nr_aiot_r2d.c: R2D transmissions of the gNB as an A-IoT Reader, on the gNB's downlink grid. The
// cell is the room's: 25 PRBs at 15 kHz in a 384-point symbol (5.76 Msps), TX_AMP of the default
// 36 dB backoff.

#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <random>
#include <vector>
extern "C" {
#include "PHY/NR_TRANSPORT/nr_aiot_r2d.h"
#include "PHY/AIOT/aiot_phy.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}

namespace {

const int kSlots = 10, kSym = 14, kFft = 384, kPrb = 25, kAmp = 519;

struct Cell {
  nr_aiot_r2d_t r2d;
  explicit Cell(int prb_start = 0, int n_rb_max = 3, double offset_db = 0, int carrier = 0)
  {
    nr_aiot_r2d_config_t cfg = {prb_start, n_rb_max, offset_db, carrier};
    EXPECT_EQ(nr_aiot_r2d_init(&r2d, &cfg, kSlots, kSym, kFft, kPrb, kAmp), 0);
  }
  ~Cell() { nr_aiot_r2d_free(&r2d); }
};

std::vector<uint8_t> RandomBits(std::mt19937 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

// One slot's grid, as phy_procedures_gNB_TX has it: the carrier's subcarriers from index 0.
struct Grid {
  std::vector<c16_t> re = std::vector<c16_t>(kSym * kFft, c16_t{0, 0});
  c16_t &at(int sym, int sc) { return re[sym * kFft + sc]; }
};

// The time samples of a slot's grid, as the gNB's OFDM sends them: the carrier centred on DC
// (fftshift_inverse), the inverse DFT, the CP of 30 or 27 samples. Scaled so that a resource element
// of value v gives v in every sample.
void Modulate(Grid &g, std::vector<std::complex<double>> &out)
{
  const int half = kPrb * 6;
  for (int s = 0; s < kSym; s++) {
    std::vector<std::complex<double>> bins(kFft, 0);
    for (int k = 0; k < kPrb * 12; k++) {
      const c16_t v = g.at(s, k);
      bins[k < half ? kFft - half + k : k - half] = std::complex<double>(v.r, v.i);
    }
    std::vector<std::complex<double>> u(kFft, 0);
    for (int k = 0; k < kFft; k++)
      if (bins[k] != 0.0)
        for (int t = 0; t < kFft; t++)
          u[t] += bins[k] * std::polar(1.0, 2 * M_PI * k * t / kFft);
    const int cp = s % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
    out.insert(out.end(), u.end() - cp, u.end());
    out.insert(out.end(), u.begin(), u.end());
  }
}

// The resource elements a transmission must put on the grid: the R2D's, at TX_AMP sqrt(K) times
// 10^(offset/20), rounded. re[l * K + q] is subcarrier q of R2D symbol l.
std::vector<c16_t> Expected(const std::vector<uint8_t> &tb, int m, int n_rb, double offset_db = 0)
{
  std::vector<uint8_t> chips(4 * (tb.size() + 16) + 64);
  const int n_chips = aiot_r2d_chips(tb.data(), tb.size(), m, chips.data(), chips.size());
  EXPECT_GT(n_chips, 0);
  const int n_sym = aiot_r2d_n_symbols(n_chips, m), K = n_rb * 12;
  std::vector<cd_t> re(n_sym * K);
  EXPECT_EQ(aiot_r2d_resource_elements(chips.data(), n_chips, m, n_rb, re.data(), re.size()), n_sym * K);
  const double s = kAmp * pow(10.0, offset_db / 20) * sqrt((double)K);
  std::vector<c16_t> out(re.size());
  for (size_t i = 0; i < re.size(); i++)
    out[i] = c16_t{(int16_t)lround(s * re[i].r), (int16_t)lround(s * re[i].i)};
  return out;
}

} // namespace

// The resource elements land on the configured PRBs, on the symbols from the start symbol on, and
// nowhere else; a transmission longer than the rest of its slot goes on in the next slots.
TEST(NrAiotR2D, WritesItsPrbsAndSymbolsOnly)
{
  std::mt19937 gen(1);
  for (int symbol : {0, 7})
    for (int prb_start : {0, 11, 22}) {
      Cell cell(prb_start, 3);
      std::vector<uint8_t> tb = RandomBits(gen, 96);
      const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 5, 3, symbol, tb.data(), 96, 6, 3);
      ASSERT_GT(n_sym, kSym) << "a 96-bit R2D with M = 6 spans more than one slot";
      const std::vector<c16_t> want = Expected(tb, 6, 3);
      ASSERT_EQ((int)want.size(), n_sym * 36);
      int seen = 0;
      for (int slot = 3; seen < n_sym; slot++) {
        Grid g;
        const int w = nr_aiot_r2d_map(&cell.r2d, 5, slot, g.re.data(), nullptr);
        for (int s = 0; s < kSym; s++) {
          const int l = (slot - 3) * kSym + s - symbol;
          const bool r2d = l >= 0 && l < n_sym;
          for (int k = 0; k < kFft; k++) {
            const bool in = r2d && k >= prb_start * 12 && k < (prb_start + 3) * 12;
            const c16_t v = in ? want[l * 36 + k - prb_start * 12] : c16_t{0, 0};
            EXPECT_EQ(g.at(s, k).r, v.r) << "slot " << slot << ", symbol " << s << ", subcarrier " << k;
            EXPECT_EQ(g.at(s, k).i, v.i);
          }
        }
        seen += w;
      }
      EXPECT_EQ(seen, n_sym);
      EXPECT_EQ(cell.r2d.sent, 1u);
      EXPECT_EQ(cell.r2d.collisions, 0u);
      EXPECT_EQ(cell.r2d.n_pending, 0);
    }
}

// What the gNB sends, through its OFDM, is what a device decodes: the transport block, the R2D
// ending where its last symbol ends. Every M, at the bottom, middle and top of the carrier.
TEST(NrAiotR2D, DeviceDecodesWhatTheGnbSends)
{
  std::mt19937 gen(2);
  for (int m : {2, 6, 12, 24})
    for (int prb_start : {0, 11, kPrb - 3}) {
      const int n_rb = aiot_n_rb_min(m);
      Cell cell(prb_start, 3);
      std::vector<uint8_t> tb = RandomBits(gen, 40);
      const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 0, 1, 7, tb.data(), 40, m, n_rb);
      ASSERT_GT(n_sym, 0);
      std::vector<std::complex<double>> air;
      for (int slot = 0; slot < 1 + (7 + n_sym + kSym - 1) / kSym + 1; slot++) {
        Grid g;
        nr_aiot_r2d_map(&cell.r2d, 0, slot, g.re.data(), nullptr);
        Modulate(g, air);
      }
      // the device's own scale: its envelope detector does not care about the absolute level
      std::vector<cd_t> y(air.size());
      for (size_t i = 0; i < air.size(); i++)
        y[i] = cd_t{air[i].real() / kAmp, air[i].imag() / kAmp};
      aiot_r2d_rx_t *rx = aiot_r2d_rx_new();
      aiot_r2d_frame_t fr[4];
      int found = 0;
      for (size_t p = 0; p < y.size(); p += AIOT_SAMPLES_PER_MS) {
        const int k = aiot_r2d_rx_receive(rx, y.data() + p, AIOT_SAMPLES_PER_MS, fr + found, 4 - found);
        ASSERT_GE(k, 0);
        found += k;
      }
      aiot_r2d_rx_free(rx);
      ASSERT_EQ(found, 1) << "M = " << m << ", PRB " << prb_start;
      EXPECT_EQ(fr[0].n_bits, 40);
      EXPECT_TRUE(std::equal(tb.begin(), tb.end(), fr[0].bits));
      // starts at symbol 7 of slot 1: one slot (5760 samples) and the first half-subframe (2880)
      EXPECT_EQ(fr[0].end_sample, AIOT_SAMPLES_PER_MS + AIOT_HALF_SUBFRAME + aiot_r2d_n_samples(n_sym));
    }
}

// With half its chips on, the R2D's EPRE is the PDSCH's (TX_AMP^2 / 2) plus the configured offset,
// over the R2D's own symbols and PRBs, for every M.
TEST(NrAiotR2D, EpreFollowsTheOffset)
{
  std::mt19937 gen(3);
  for (int m : {2, 6, 12, 24})
    for (double offset : {0.0, -6.0, 3.0}) {
      const int n_rb = aiot_n_rb_min(m), K = n_rb * 12;
      double sum = 0;
      int n = 0;
      for (int t = 0; t < 10; t++) {
        Cell cell(4, 3, offset); // each from frame 0 again
        std::vector<uint8_t> tb = RandomBits(gen, 200);
        const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 0, 0, 0, tb.data(), 200, m, n_rb);
        ASSERT_GT(n_sym, 0);
        for (int slot = 0; cell.r2d.n_pending; slot++) {
          Grid g;
          nr_aiot_r2d_map(&cell.r2d, 0, slot, g.re.data(), nullptr);
          for (int s = 0; s < kSym && slot * kSym + s < n_sym; s++)
            for (int k = 48; k < 48 + K; k++) {
              sum += (double)g.at(s, k).r * g.at(s, k).r + (double)g.at(s, k).i * g.at(s, k).i;
              n++;
            }
        }
      }
      const double epre_db = 10 * log10(sum / n);
      const double pdsch_db = 10 * log10(kAmp * kAmp / 2.0);
      EXPECT_NEAR(epre_db, pdsch_db + offset, 0.5) << "M = " << m << ", offset " << offset;
    }
}

// The phase rotation of the slot's symbols is applied as to the NR channels.
TEST(NrAiotR2D, AppliesTheSymbolRotation)
{
  std::mt19937 gen(4);
  std::vector<uint8_t> tb = RandomBits(gen, 20);
  Cell plain, rotated;
  ASSERT_GT(nr_aiot_r2d_schedule(&plain.r2d, 0, 0, 0, tb.data(), 20, 6, 1), 0);
  ASSERT_GT(nr_aiot_r2d_schedule(&rotated.r2d, 0, 0, 0, tb.data(), 20, 6, 1), 0);
  std::vector<c16_t> rot(kSym);
  for (int s = 0; s < kSym; s++)
    rot[s] = c16_t{(int16_t)lround(32767 * cos(0.3 * s)), (int16_t)lround(32767 * sin(0.3 * s))};
  Grid a, b;
  nr_aiot_r2d_map(&plain.r2d, 0, 0, a.re.data(), nullptr);
  nr_aiot_r2d_map(&rotated.r2d, 0, 0, b.re.data(), rot.data());
  for (int s = 0; s < kSym; s++)
    for (int k = 0; k < kFft; k++) {
      const std::complex<double> want =
          std::complex<double>(a.at(s, k).r, a.at(s, k).i) * std::complex<double>(rot[s].r, rot[s].i) / 32768.0;
      EXPECT_NEAR(b.at(s, k).r, want.real(), 0.5) << s << " " << k;
      EXPECT_NEAR(b.at(s, k).i, want.imag(), 0.5) << s << " " << k;
    }
}

// A transmission must not overlap another in time, must start at symbol 0 or 7, and must fit the
// configured PRBs; a gNB without R2D PRBs schedules nothing.
TEST(NrAiotR2D, RejectsWhatItCannotSend)
{
  std::mt19937 gen(5);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  Cell cell(0, 3);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 10, 0, 0, tb.data(), 96, 6, 1);
  ASSERT_GT(n_sym, 0);
  const int end_slot = n_sym / kSym; // the slot of its last symbol, counted from slot 0
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 10, end_slot, 0, tb.data(), 96, 6, 1), AIOT_EINVAL);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 9, 9, 7, tb.data(), 96, 6, 1), AIOT_EINVAL) << "ends inside the first";
  EXPECT_GT(nr_aiot_r2d_schedule(&cell.r2d, 10, end_slot + 1, 0, tb.data(), 96, 6, 1), 0) << "right after it";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 20, 0, 3, tb.data(), 96, 6, 1), AIOT_EINVAL) << "symbol 3";
  EXPECT_GT(nr_aiot_r2d_schedule(&cell.r2d, 20, 0, 0, tb.data(), 96, 24, 3), 0) << "M = 24 on 3 PRBs";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 40, 0, 0, tb.data(), 96, 12, 1), AIOT_EINVAL) << "M = 12 needs 2 PRBs";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 40, 0, 0, tb.data(), 96, 6, 4), AIOT_EINVAL) << "over the 3 configured";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 40, 0, 0, tb.data(), 0, 6, 1), AIOT_EINVAL);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 40, 0, 0, tb.data(), 1001, 6, 1), AIOT_EINVAL);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 1024, 0, 0, tb.data(), 96, 6, 1), AIOT_EINVAL);

  Cell off(-1, 3);
  EXPECT_EQ(nr_aiot_r2d_schedule(&off.r2d, 0, 0, 0, tb.data(), 96, 6, 1), AIOT_EINVAL);
  Grid g;
  EXPECT_EQ(nr_aiot_r2d_map(&off.r2d, 0, 0, g.re.data(), nullptr), 0);
  nr_aiot_r2d_t zeroed; // a gNB that never read a configuration
  nr_aiot_r2d_config_t none = {};
  EXPECT_EQ(nr_aiot_r2d_init(&zeroed, &none, kSlots, kSym, kFft, kPrb, kAmp), 0);
  EXPECT_EQ(nr_aiot_r2d_schedule(&zeroed, 0, 0, 0, tb.data(), 96, 6, 1), AIOT_EINVAL);
  nr_aiot_r2d_free(&zeroed);

  nr_aiot_r2d_t bad;
  nr_aiot_r2d_config_t cfg = {23, 3, 0};
  EXPECT_EQ(nr_aiot_r2d_init(&bad, &cfg, kSlots, kSym, kFft, kPrb, kAmp), -1) << "PRBs 23 to 25 of 25";
  nr_aiot_r2d_free(&bad);
  cfg = {0, 3, 0};
  EXPECT_EQ(nr_aiot_r2d_init(&bad, &cfg, 20, kSym, kFft, kPrb, kAmp), -1) << "30 kHz";
  nr_aiot_r2d_free(&bad);
}

// A transmission whose first slot was not mapped, or that misses a slot it spans, is not sent at
// all; one that starts later stays pending.
TEST(NrAiotR2D, DropsATransmissionThatMissesASlot)
{
  std::mt19937 gen(6);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  Cell cell(0, 3);
  ASSERT_GT(nr_aiot_r2d_schedule(&cell.r2d, 3, 2, 0, tb.data(), 96, 6, 1), 0);
  Grid g;
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 3, 1, g.re.data(), nullptr), 0) << "not yet";
  EXPECT_EQ(cell.r2d.n_pending, 1);
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 3, 3, g.re.data(), nullptr), 0) << "slot 2 was skipped";
  EXPECT_EQ(cell.r2d.dropped, 1u);
  EXPECT_EQ(cell.r2d.n_pending, 0);

  ASSERT_GT(nr_aiot_r2d_schedule(&cell.r2d, 4, 0, 0, tb.data(), 96, 6, 1), 0);
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 4, 0, g.re.data(), nullptr), kSym);
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 4, 2, g.re.data(), nullptr), 0) << "slot 1 was skipped";
  EXPECT_EQ(cell.r2d.dropped, 2u);
  EXPECT_EQ(cell.r2d.sent, 0u);
}

// Scheduling and mapping across the SFN wrap, 1023 -> 0.
TEST(NrAiotR2D, CrossesTheFrameNumberWrap)
{
  std::mt19937 gen(7);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  Cell cell(0, 3);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 1023, 9, 7, tb.data(), 96, 6, 1);
  ASSERT_GT(n_sym, 7);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 0, 0, 0, tb.data(), 96, 6, 1), AIOT_EINVAL) << "overlaps across the wrap";
  Grid first;
  int seen = nr_aiot_r2d_map(&cell.r2d, 1023, 9, first.re.data(), nullptr);
  EXPECT_EQ(seen, 7);
  for (int f = 0, s = 0; cell.r2d.n_pending; s++, f += s / 10, s %= 10) {
    Grid g;
    seen += nr_aiot_r2d_map(&cell.r2d, f, s, g.re.data(), nullptr);
  }
  EXPECT_EQ(seen, n_sym);
  EXPECT_EQ(cell.r2d.sent, 1u);
  EXPECT_EQ(cell.r2d.collisions, 0u);
}

// A resource element already used by an NR channel counts as a collision; the R2D still goes out.
TEST(NrAiotR2D, CountsCollisionsWithNrChannels)
{
  std::mt19937 gen(8);
  std::vector<uint8_t> tb = RandomBits(gen, 8);
  Cell cell(0, 3);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 0, 0, 0, tb.data(), 8, 6, 1);
  ASSERT_GT(n_sym, 0);
  ASSERT_LE(n_sym, kSym);
  Grid g;
  g.at(0, 5) = c16_t{100, 0};
  g.at(1, 11) = c16_t{0, -3};
  g.at(1, 12) = c16_t{7, 7}; // PRB 1: not the R2D's
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 0, 0, g.re.data(), nullptr), n_sym);
  EXPECT_EQ(cell.r2d.sent, 1u);
  EXPECT_EQ(cell.r2d.collisions, 2u);
  EXPECT_EQ(g.at(1, 12).r, 7);
}

// The same slot mapped again (a repeated TX of it) leaves the transmission as it is.
TEST(NrAiotR2D, MappingASlotAgainChangesNothing)
{
  std::mt19937 gen(9);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  Cell cell(0, 3);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 2, 0, 0, tb.data(), 96, 6, 1);
  ASSERT_GT(n_sym, kSym);
  Grid g;
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 2, 0, g.re.data(), nullptr), kSym);
  Grid again;
  EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 2, 0, again.re.data(), nullptr), 0);
  int seen = kSym;
  for (int slot = 1; cell.r2d.n_pending; slot++) {
    Grid h;
    seen += nr_aiot_r2d_map(&cell.r2d, 2, slot, h.re.data(), nullptr);
  }
  EXPECT_EQ(seen, n_sym);
  EXPECT_EQ(cell.r2d.sent, 1u);
  EXPECT_EQ(cell.r2d.dropped, 0u);
}

// With a TDD pattern, an R2D that spans an uplink or mixed slot is refused.
int DlSlots(const void *, int, int slot)
{
  return slot % 5 < 3; // DDDSU DDDSU
}

TEST(NrAiotR2D, GoesOnlyInDownlinkSlots)
{
  std::mt19937 gen(10);
  std::vector<uint8_t> tb = RandomBits(gen, 40);
  Cell cell(0, 3);
  nr_aiot_r2d_set_dl_slots(&cell.r2d, DlSlots, nullptr);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 0, 0, 0, tb.data(), 40, 6, 1);
  ASSERT_GT(n_sym, kSym);
  ASSERT_LE(n_sym, 3 * kSym);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 1, 2, 0, tb.data(), 40, 6, 1), AIOT_EINVAL) << "runs into slot 3";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 1, 4, 0, tb.data(), 40, 6, 1), AIOT_EINVAL) << "an uplink slot";
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 1023, 9, 0, tb.data(), 40, 6, 1), AIOT_EINVAL);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 1, 5, 0, tb.data(), 40, 6, 1), n_sym);
}

// An EPRE whose resource elements would not fit 16 bits is refused at init, so nothing is clipped.
TEST(NrAiotR2D, RefusesAPowerThatWouldClip)
{
  nr_aiot_r2d_t r2d;
  nr_aiot_r2d_config_t cfg = {0, 3, 0};
  // peak = 519 sqrt(36) 10^(x/20) <= 32767: x <= 20.4 dB
  cfg.power_offset_db = 20.0;
  EXPECT_EQ(nr_aiot_r2d_init(&r2d, &cfg, kSlots, kSym, kFft, kPrb, kAmp), 0);
  nr_aiot_r2d_free(&r2d);
  cfg.power_offset_db = 21.0;
  EXPECT_EQ(nr_aiot_r2d_init(&r2d, &cfg, kSlots, kSym, kFft, kPrb, kAmp), -1);
  nr_aiot_r2d_free(&r2d);
}

// The slot mapped last is the gNB's TX time: an R2D must start after it.
TEST(NrAiotR2D, RefusesASlotAlreadyMapped)
{
  std::mt19937 gen(9);
  std::vector<uint8_t> tb = RandomBits(gen, 96);
  Cell cell(0, 3);
  EXPECT_EQ(nr_aiot_r2d_now(&cell.r2d), -1);
  Grid g;
  nr_aiot_r2d_map(&cell.r2d, 5, 4, g.re.data(), nullptr);
  EXPECT_EQ(nr_aiot_r2d_now(&cell.r2d), 54);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 5, 4, 7, tb.data(), 96, 6, 1), NR_AIOT_R2D_ETOOLATE);
  EXPECT_EQ(nr_aiot_r2d_schedule(&cell.r2d, 5, 1, 0, tb.data(), 96, 6, 1), NR_AIOT_R2D_ETOOLATE);
  EXPECT_GT(nr_aiot_r2d_schedule(&cell.r2d, 5, 5, 0, tb.data(), 96, 6, 1), 0);
  nr_aiot_r2d_map(&cell.r2d, 1023, 9, g.re.data(), nullptr); // across the wrap, 0.0 is next
  EXPECT_GT(nr_aiot_r2d_schedule(&cell.r2d, 0, 0, 7, tb.data(), 96, 6, 1), 0);
}

// With the carrier on, slots without R2D carry one tone at the centre of the reserved PRBs, as
// strong as an "on" chip of an R2D on all of them, that runs on unbroken through every cyclic prefix
// and from slot to slot: the samples are the tone itself, to the rounding of its resource element.
TEST(NrAiotR2D, TheCarrierRunsUnbroken)
{
  for (int n_rb_max : {1, 3})
    for (int prb_start : {0, 11, kPrb - n_rb_max}) {
      Cell cell(prb_start, n_rb_max, 0, 1);
      std::vector<std::complex<double>> air;
      for (int slot = 0; slot < 3; slot++) {
        Grid g;
        EXPECT_EQ(nr_aiot_r2d_map(&cell.r2d, 7, slot, g.re.data(), nullptr), 0);
        for (int s = 0; s < kSym; s++)
          for (int k = 0; k < kFft; k++)
            if (k != prb_start * 12 + 6 * n_rb_max) {
              ASSERT_TRUE(g.at(s, k).r == 0 && g.at(s, k).i == 0) << "only the carrier's subcarrier";
            }
        Modulate(g, air);
      }
      ASSERT_EQ(air.size(), 3u * AIOT_SAMPLES_PER_MS);
      const int k = prb_start * 12 + 6 * n_rb_max - kPrb * 6;
      const double a = kAmp * std::sqrt(12.0 * n_rb_max);
      double worst = 0;
      for (size_t t = 0; t < air.size(); t++)
        worst = std::max(worst, std::abs(air[t] - std::polar(a, 2 * M_PI * k * (double)(t % kFft) / kFft)));
      EXPECT_LT(worst, 1.0) << n_rb_max << " PRBs from " << prb_start << ": the tone breaks";
      EXPECT_EQ(cell.r2d.carrier_blocked, 0u);
    }
}

// An R2D keys the carrier: its symbols carry the R2D alone, the others the carrier, and a device
// still decodes it. A resource element an NR channel already holds is left alone and counted.
TEST(NrAiotR2D, TheR2DKeysTheCarrier)
{
  std::mt19937 gen(10);
  std::vector<uint8_t> tb = RandomBits(gen, 40);
  Cell cell(0, 3, 0, 1);
  const int n_sym = nr_aiot_r2d_schedule(&cell.r2d, 0, 1, 7, tb.data(), 40, 6, 3);
  ASSERT_GT(n_sym, 0);
  const std::vector<c16_t> want = Expected(tb, 6, 3);
  std::vector<std::complex<double>> air;
  const int n_slots = 1 + (7 + n_sym + kSym - 1) / kSym + 1;
  for (int slot = 0; slot < n_slots; slot++) {
    Grid g;
    if (slot == n_slots - 1)
      g.at(2, 18) = c16_t{5, 5}; // an NR channel on the carrier's resource element
    nr_aiot_r2d_map(&cell.r2d, 0, slot, g.re.data(), nullptr);
    for (int s = 0; s < kSym; s++) {
      const int l = (slot - 1) * kSym + s - 7;
      const c16_t v = g.at(s, 18);
      if (l >= 0 && l < n_sym) {
        EXPECT_EQ(v.r, want[l * 36 + 18].r) << "R2D symbol " << l;
        EXPECT_EQ(v.i, want[l * 36 + 18].i);
      } else if (slot == n_slots - 1 && s == 2) {
        EXPECT_EQ(v.r, 5);
      } else {
        EXPECT_EQ(v.r, cell.r2d.carrier_re[s].r) << "slot " << slot << ", symbol " << s;
        EXPECT_EQ(v.i, cell.r2d.carrier_re[s].i);
      }
    }
    Modulate(g, air);
  }
  EXPECT_EQ(cell.r2d.carrier_blocked, 1u);
  std::vector<cd_t> y(air.size());
  for (size_t i = 0; i < air.size(); i++)
    y[i] = cd_t{air[i].real() / kAmp, air[i].imag() / kAmp};
  aiot_r2d_rx_t *rx = aiot_r2d_rx_new();
  aiot_r2d_frame_t fr[4];
  int found = 0;
  for (size_t p = 0; p + AIOT_SAMPLES_PER_MS <= y.size(); p += AIOT_SAMPLES_PER_MS) {
    const int n = aiot_r2d_rx_receive(rx, y.data() + p, AIOT_SAMPLES_PER_MS, fr + found, 4 - found);
    ASSERT_GE(n, 0);
    found += n;
  }
  aiot_r2d_rx_free(rx);
  ASSERT_EQ(found, 1);
  EXPECT_TRUE(std::equal(tb.begin(), tb.end(), fr[0].bits));
  EXPECT_EQ(fr[0].end_sample, AIOT_SAMPLES_PER_MS + AIOT_HALF_SUBFRAME + aiot_r2d_n_samples(n_sym));
}

int main(int argc, char **argv)
{
  logInit();
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
