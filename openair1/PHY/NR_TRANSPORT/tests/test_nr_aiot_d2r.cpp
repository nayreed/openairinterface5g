/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// nr_aiot_d2r.c: D2R reception of the gNB as an A-IoT Reader, in PRBs of its uplink band. The cell
// is the room's: 25 PRBs at 15 kHz, the D2R in UL PRBs 15 to 21 (PRB 18's centre, 1.08 MHz above
// the carrier), the radio at 5.76 Msps or, with D = 4, 23.04 Msps. Signals are in ADC counts.

#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
extern "C" {
#include "PHY/NR_TRANSPORT/nr_aiot_d2r.h"
#include "PHY/AIOT/aiot_phy.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}

namespace {

const int kPrb = 25, kSlot = AIOT_SAMPLES_PER_MS;
const nr_aiot_d2r_config_t kRoom = {15, 7, 1.0, 1};
const double kShiftHz = 1080000.0;
const double kCwCounts = 400; // the carrier at the gNB
const double kTagCounts = 20; // the D2R's amplitude at the gNB
// T_bit = tau/8, I_bit 768, the TBCC, 31-bit ambles: the room's D2R
aiot_d2r_params_t Params(int r_sfs, aiot_mod_t mod = AIOT_BPSK)
{
  return {1, 8, r_sfs, 1, 1, 31, 768, 0, mod};
}

using D2rPtr = std::unique_ptr<nr_aiot_d2r_t, void (*)(nr_aiot_d2r_t *)>;

D2rPtr Make(const nr_aiot_d2r_config_t &cfg = kRoom, int64_t fs = AIOT_SAMPLE_RATE_HZ)
{
  return D2rPtr(nr_aiot_d2r_new(&cfg, kPrb, AIOT_SCS_HZ, fs), nr_aiot_d2r_free);
}

std::vector<uint8_t> RandomBits(std::mt19937_64 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

aiot_d2r_expect_t Expect(int64_t start, int te, int a_bits, const aiot_d2r_params_t &p, std::vector<int> shifts = {})
{
  aiot_d2r_expect_t e;
  memset(&e, 0, sizeof(e));
  e.start = start;
  e.te = te;
  e.a_bits = a_bits;
  e.params = p;
  e.n_r_sfs = (int)shifts.size();
  std::copy(shifts.begin(), shifts.end(), e.r_sfs);
  return e;
}

// The D2R band at 0 Hz, one value per A-IoT sample: the carrier plus each device's backscatter.
struct Band {
  int64_t a0; // A-IoT sample of y[0]
  std::vector<std::complex<double>> y;
  Band(int64_t a0, int n, double cw_phase) : a0(a0), y(n, std::polar(kCwCounts, cw_phase))
  {
  }
  void Add(int64_t at, const std::vector<uint8_t> &tb, const aiot_d2r_params_t &p, double amp, double ph)
  {
    const int len = aiot_frame_samples((int)tb.size(), &p);
    std::vector<double> w(len);
    ASSERT_EQ(aiot_d2r_waveform(tb.data(), (int)tb.size(), &p, w.data(), len), len);
    ASSERT_LE(at - a0 + len, (int64_t)y.size());
    for (int i = 0; i < len; i++)
      y[at - a0 + i] += std::polar(amp, ph) * w[i];
  }
};

// What the radio delivers from radio timestamp ts0 = a0 D: the band moved up to shift_hz, each
// A-IoT sample held for D radio samples, plus what else arrives (other), the noise, and rounding.
struct Air {
  int64_t ts0;
  std::vector<std::complex<double>> x;
  Air(const Band &b, int d, double shift_hz = kShiftHz) : ts0(b.a0 * d), x(b.y.size() * d)
  {
    const int64_t fs = (int64_t)AIOT_SAMPLE_RATE_HZ * d, f = (int64_t)shift_hz;
    for (size_t t = 0; t < x.size(); t++) {
      int64_t ph = (int64_t)(((__int128)((ts0 + (int64_t)t) % fs) * f) % fs);
      x[t] = b.y[t / d] * std::polar(1.0, 2 * M_PI * (double)ph / (double)fs);
    }
  }
  void AddTone(double hz, double amp, int d)
  {
    const double fs = (double)AIOT_SAMPLE_RATE_HZ * d;
    for (size_t t = 0; t < x.size(); t++)
      x[t] += std::polar(amp, 2 * M_PI * hz * std::fmod((double)(ts0 + (int64_t)t), fs) / fs);
  }
  // A UE's uplink: QPSK on every subcarrier of the PRBs outside 15-21, as 384-point OFDM at
  // 5.76 Msps with a 27-sample CP, scaled to rms counts.
  void AddUplink(std::mt19937_64 &gen, double rms)
  {
    const int n = AIOT_N_FFT, cp = AIOT_CP_SHORT;
    std::vector<std::complex<double>> sym(n), td(n);
    std::vector<std::complex<double>> ue(x.size());
    for (size_t at = 0; at < x.size(); at += n + cp) {
      std::fill(sym.begin(), sym.end(), 0.0);
      for (int k = 0; k < 12 * kPrb; k++) {
        if (k >= 12 * 15 && k < 12 * 22)
          continue;
        int bin = ((k - 6 * kPrb) % n + n) % n;
        sym[bin] = std::complex<double>((gen() & 1) ? 1 : -1, (gen() & 1) ? 1 : -1);
      }
      for (int t = 0; t < n; t++) {
        std::complex<double> s = 0;
        for (int b = 0; b < n; b++)
          s += sym[b] * std::polar(1.0, 2 * M_PI * b * t / n);
        td[t] = s;
      }
      for (int t = 0; t < n + cp && at + t < x.size(); t++)
        ue[at + t] = td[(t - cp + n) % n];
    }
    double p = 0;
    for (auto &s : ue)
      p += std::norm(s);
    const double g = rms / std::sqrt(p / ue.size());
    for (size_t t = 0; t < x.size(); t++)
      x[t] += g * ue[t];
  }
  std::vector<c16_t> Counts(std::mt19937_64 &gen, double noise = 1.0) const
  {
    std::normal_distribution<double> g(0, noise);
    std::vector<c16_t> c(x.size());
    for (size_t t = 0; t < x.size(); t++)
      c[t] = {(int16_t)std::lround(x[t].real() + g(gen)), (int16_t)std::lround(x[t].imag() + g(gen))};
    return c;
  }
};

std::vector<aiot_d2r_outcome_t> Pop(nr_aiot_d2r_t *d)
{
  std::vector<aiot_d2r_outcome_t> all;
  std::unique_ptr<aiot_d2r_outcome_t[]> out(new aiot_d2r_outcome_t[4]);
  int k;
  while ((k = nr_aiot_d2r_pop(d, out.get(), 4)) > 0)
    all.insert(all.end(), out.get(), out.get() + k);
  return all;
}

// Feeds the air slot by slot (slot samples each) and returns every outcome.
std::vector<aiot_d2r_outcome_t> FeedAll(nr_aiot_d2r_t *d, int64_t ts0, const std::vector<c16_t> &c, int slot)
{
  for (size_t at = 0; at < c.size(); at += slot) {
    int n = (int)std::min<size_t>(slot, c.size() - at);
    EXPECT_EQ(nr_aiot_d2r_feed(d, ts0 + (int64_t)at, c.data() + at, n), 0);
  }
  return Pop(d);
}

void ExpectDecoded(const aiot_d2r_outcome_t &o, const std::vector<uint8_t> &tb, int64_t start, int r_sfs, int slack = 0)
{
  ASSERT_EQ(o.n_decoded, 1);
  EXPECT_EQ(o.collided, 0);
  EXPECT_EQ(o.decoded[0].n_bits, (int)tb.size());
  EXPECT_TRUE(std::equal(tb.begin(), tb.end(), o.decoded[0].bits));
  EXPECT_NEAR((double)o.decoded[0].start, (double)start, slack);
  EXPECT_EQ(o.decoded[0].r_sfs, r_sfs);
}

} // namespace

TEST(NrAiotD2R, TheD2RIsAtTheCentreOfItsPrbs)
{
  D2rPtr room = Make();
  ASSERT_TRUE(room);
  EXPECT_EQ(nr_aiot_d2r_shift_hz(room.get()), kShiftHz);
  EXPECT_EQ(nr_aiot_d2r_decimation(room.get()), 1);
  D2rPtr low = Make({0, 3, 1.0, 1}, 4LL * AIOT_SAMPLE_RATE_HZ);
  ASSERT_TRUE(low);
  EXPECT_EQ(nr_aiot_d2r_shift_hz(low.get()), -1980000.0); // PRB 1's centre
  EXPECT_EQ(nr_aiot_d2r_decimation(low.get()), 4);
}

TEST(NrAiotD2R, RefusesWhatDoesNotFit)
{
  EXPECT_FALSE(Make({-1, 7, 1.0, 1}));
  EXPECT_FALSE(Make({20, 6, 1.0, 1})); // PRB 25 is past the carrier
  EXPECT_FALSE(Make({15, 0, 1.0, 1}));
  EXPECT_FALSE(Make({15, 7, 0.0, 1}));
  EXPECT_FALSE(Make(kRoom, 7680000)); // 25 PRBs without three-quarter sampling
  EXPECT_FALSE(Make(kRoom, 2880000));
  EXPECT_FALSE(D2rPtr(nr_aiot_d2r_new(&kRoom, kPrb, 30000, 11520000), nr_aiot_d2r_free)); // 30 kHz
}

// Every R_SFS the room's paging offers, both modulations, one Tag per window: the gNB decodes
// the bits and the start, and decodes exactly what the A-IoT receiver decodes at 0 Hz, as Amber's
// Reader does.
TEST(NrAiotD2R, DecodesATagInTheRoom)
{
  std::mt19937_64 gen(1);
  for (aiot_mod_t mod : {AIOT_BPSK, AIOT_OOK})
    for (int r : {1, 2, 4, 8}) {
      const aiot_d2r_params_t p = Params(r, mod);
      const auto tb = RandomBits(gen, 40);
      const int64_t a0 = 987654321, start = a0 + 7000 + (int64_t)(gen() % 3000);
      const int len = aiot_frame_samples(40, &p);
      Band band(a0, 7000 + 3000 + len + 3 * kSlot, 0.7);
      band.Add(start, tb, p, kTagCounts, 2.1);
      std::vector<c16_t> c = Air(band, 1).Counts(gen);
      const aiot_d2r_expect_t e = Expect(start - 50, 200, 40, p, {1, 2, 4, 8});

      D2rPtr d = Make();
      ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
      auto got = FeedAll(d.get(), a0, c, kSlot);
      ASSERT_EQ(got.size(), 1u) << "R_SFS " << r;
      EXPECT_EQ(got[0].expect.start, e.start);
      ExpectDecoded(got[0], tb, start, r);
      EXPECT_EQ(nr_aiot_d2r_received_until(d.get()), a0 + (int64_t)c.size());

      // the same band at 0 Hz straight into the A-IoT receiver
      std::unique_ptr<aiot_d2r_rx_t, void (*)(aiot_d2r_rx_t *)> rx(aiot_d2r_rx_new(2.0, 1), aiot_d2r_rx_free);
      aiot_d2r_expect_t e0 = e;
      e0.start -= a0;
      ASSERT_EQ(aiot_d2r_rx_expect(rx.get(), &e0), 0);
      std::vector<cd_t> y(band.y.size());
      for (size_t i = 0; i < y.size(); i++)
        y[i] = {band.y[i].real(), band.y[i].imag()};
      std::unique_ptr<aiot_d2r_outcome_t> ref(new aiot_d2r_outcome_t);
      ASSERT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), (int)y.size(), ref.get(), 1), 1);
      ASSERT_EQ(ref->n_decoded, 1);
      EXPECT_EQ(got[0].decoded[0].start, ref->decoded[0].start + a0);
      EXPECT_NEAR(10 * log10(got[0].decoded[0].rssi_w), 10 * log10(ref->decoded[0].rssi_w), 0.5);
    }
}

// The UE's uplink, 20 dB above the D2R, in every other PRB: the gNB still decodes.
TEST(NrAiotD2R, DecodesBesideTheUeUplink)
{
  std::mt19937_64 gen(2);
  for (int r : {1, 8}) {
    const aiot_d2r_params_t p = Params(r);
    const auto tb = RandomBits(gen, 64);
    const int64_t a0 = 5 * kSlot, start = a0 + 9000;
    Band band(a0, 9000 + aiot_frame_samples(64, &p) + 2 * kSlot, 0.0);
    band.Add(start, tb, p, kTagCounts, -1.0);
    Air air(band, 1);
    air.AddUplink(gen, kTagCounts * std::sqrt(0.5) * 10); // the D2R's power is |amp|^2 / 2
    D2rPtr d = Make();
    const aiot_d2r_expect_t e = Expect(start - 100, 200, 64, p, {1, 2, 4, 8});
    ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
    auto got = FeedAll(d.get(), air.ts0, air.Counts(gen), kSlot);
    ASSERT_EQ(got.size(), 1u);
    ExpectDecoded(got[0], tb, start, r);
  }
}

// At 23.04 Msps: each chip held for 4 radio samples, and a tone 40 dB above the D2R that would
// land on the R_SFS 8 subcarrier at 5.76 Msps without the filter.
TEST(NrAiotD2R, DecimatesAndStopsWhatWouldAlias)
{
  std::mt19937_64 gen(3);
  const int d4 = 4;
  const aiot_d2r_params_t p = Params(8);
  const auto tb = RandomBits(gen, 40);
  const int64_t a0 = 123457, start = a0 + 6000;
  Band band(a0, 6000 + aiot_frame_samples(40, &p) + 2 * kSlot, 1.3);
  band.Add(start, tb, p, kTagCounts, 0.4);
  Air air(band, d4);
  air.AddTone(kShiftHz + AIOT_SAMPLE_RATE_HZ + 8 * 60000.0, kTagCounts * 100, d4);
  D2rPtr d = Make(kRoom, (int64_t)d4 * AIOT_SAMPLE_RATE_HZ);
  ASSERT_TRUE(d);
  const aiot_d2r_expect_t e = Expect(start - 100, 200, 40, p, {1, 2, 4, 8});
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
  auto got = FeedAll(d.get(), air.ts0, air.Counts(gen), d4 * kSlot);
  ASSERT_EQ(got.size(), 1u);
  ExpectDecoded(got[0], tb, start, 8, 1);
}

// Slots that never came are received as zeros, a slot that came twice is received once, and
// after more than a second without samples the receiver starts again on the same clock.
TEST(NrAiotD2R, GapsAndRepeats)
{
  std::mt19937_64 gen(4);
  const aiot_d2r_params_t p = Params(2);
  const auto tb = RandomBits(gen, 40);
  const int len = aiot_frame_samples(40, &p);
  const int64_t a0 = 0, start = 8 * kSlot + 100;
  Band band(a0, 8 * kSlot + 100 + len + 2 * kSlot, 0.3);
  band.Add(start, tb, p, kTagCounts, 0.3);
  std::vector<c16_t> c = Air(band, 1).Counts(gen);
  D2rPtr d = Make();
  const aiot_d2r_expect_t e = Expect(start - 50, 100, 40, p, {2});
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
  for (int s = 0; s * kSlot < (int)c.size(); s++) {
    if (s >= 2 && s <= 4)
      continue; // never came
    int n = std::min(kSlot, (int)c.size() - s * kSlot);
    ASSERT_EQ(nr_aiot_d2r_feed(d.get(), (int64_t)s * kSlot, c.data() + s * kSlot, n), 0);
    if (s == 6) { // came twice
      ASSERT_EQ(nr_aiot_d2r_feed(d.get(), (int64_t)s * kSlot, c.data() + s * kSlot, n), 0);
    }
  }
  auto got = Pop(d.get());
  ASSERT_EQ(got.size(), 1u);
  ExpectDecoded(got[0], tb, start, 2);
  EXPECT_EQ(nr_aiot_d2r_stats(d.get()).zero_filled, 3u * kSlot);
  EXPECT_EQ(nr_aiot_d2r_received_until(d.get()), (int64_t)c.size());

  // two seconds later, the same Tag again
  const int64_t later = (int64_t)c.size() + 2LL * AIOT_SAMPLE_RATE_HZ;
  Band band2(later, 1000 + len + 2 * kSlot, 0.3);
  band2.Add(later + 1000, tb, p, kTagCounts, 0.3);
  const aiot_d2r_expect_t e2 = Expect(later + 950, 100, 40, p, {2});
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e2), 0);
  got = FeedAll(d.get(), later, Air(band2, 1).Counts(gen), kSlot);
  ASSERT_EQ(got.size(), 1u);
  ExpectDecoded(got[0], tb, later + 1000, 2);
  EXPECT_LT(nr_aiot_d2r_stats(d.get()).zero_filled, 4u * kSlot);
}

// A gap of more than a second while one window is in the receiver and another is opened: the
// receiver ends the first on zeros and starts again, and both get an outcome.
TEST(NrAiotD2R, ARestartLosesNoWindow)
{
  std::mt19937_64 gen(7);
  const aiot_d2r_params_t p = Params(1);
  Band band(0, kSlot, 0.0);
  std::vector<c16_t> c = Air(band, 1).Counts(gen);
  D2rPtr d = Make();
  const aiot_d2r_expect_t w1 = Expect(1000, 100, 40, p), w2 = Expect(AIOT_SAMPLE_RATE_HZ, 100, 40, p);
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &w1), 0);
  ASSERT_EQ(nr_aiot_d2r_feed(d.get(), 0, c.data(), kSlot), 0);
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &w2), 0);
  ASSERT_EQ(nr_aiot_d2r_feed(d.get(), 2LL * AIOT_SAMPLE_RATE_HZ, c.data(), kSlot), 0);
  auto got = Pop(d.get());
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].expect.start, w1.start);
  EXPECT_EQ(got[1].expect.start, w2.start);
  EXPECT_EQ(nr_aiot_d2r_stats(d.get()).late, 1u); // w2 was opened inside the gap
  EXPECT_LT(nr_aiot_d2r_stats(d.get()).zero_filled, (uint64_t)AIOT_SAMPLE_RATE_HZ);
}

// A window opened after its samples have gone by still gets an outcome, and is counted as late.
TEST(NrAiotD2R, ALateWindowIsCounted)
{
  std::mt19937_64 gen(5);
  const aiot_d2r_params_t p = Params(1);
  Band band(0, 6 * kSlot, 0.0);
  std::vector<c16_t> c = Air(band, 1).Counts(gen);
  D2rPtr d = Make();
  ASSERT_EQ(nr_aiot_d2r_feed(d.get(), 0, c.data(), 3 * kSlot), 0);
  const aiot_d2r_expect_t e = Expect(kSlot, 100, 40, p);
  ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
  for (int s = 3; s < 6; s++)
    ASSERT_EQ(nr_aiot_d2r_feed(d.get(), (int64_t)s * kSlot, c.data() + s * kSlot, kSlot), 0);
  auto got = Pop(d.get());
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].n_decoded, 0);
  EXPECT_EQ(nr_aiot_d2r_stats(d.get()).late, 1u);
  EXPECT_EQ(nr_aiot_d2r_stats(d.get()).windows, 1u);
}

// With the worker: slots pushed, windows opened, and the same outcome as in the caller's thread.
TEST(NrAiotD2R, TheWorkerDecodesTheSame)
{
  std::mt19937_64 gen(6);
  const aiot_d2r_params_t p = Params(4);
  const auto tb = RandomBits(gen, 40);
  const int64_t a0 = 42 * kSlot, start = a0 + 3333;
  Band band(a0, 3333 + aiot_frame_samples(40, &p) + 3 * kSlot, 2.0);
  band.Add(start, tb, p, kTagCounts, 1.0);
  std::vector<c16_t> c = Air(band, 1).Counts(gen);
  const aiot_d2r_expect_t e = Expect(start - 60, 120, 40, p, {1, 2, 4, 8});

  D2rPtr inline_d = Make();
  ASSERT_EQ(nr_aiot_d2r_expect(inline_d.get(), &e), 0);
  auto want = FeedAll(inline_d.get(), a0, c, kSlot);

  ASSERT_EQ(want.size(), 1u);
  for (int wait : {0, 1}) {
    D2rPtr d = Make();
    ASSERT_EQ(nr_aiot_d2r_start(d.get(), wait ? 1 : 4, kSlot, wait), 0);
    EXPECT_EQ(nr_aiot_d2r_feed(d.get(), a0, c.data(), kSlot), AIOT_EINVAL);
    ASSERT_EQ(nr_aiot_d2r_expect(d.get(), &e), 0);
    for (size_t at = 0; at < c.size(); at += kSlot) {
      int n = (int)std::min<size_t>(kSlot, c.size() - at);
      if (wait) {
        ASSERT_EQ(nr_aiot_d2r_push(d.get(), a0 + (int64_t)at, c.data() + at, n), 0); // waits for room
      } else {
        while (nr_aiot_d2r_push(d.get(), a0 + (int64_t)at, c.data() + at, n) == AIOT_ESPACE)
          nr_aiot_d2r_drain(d.get());
      }
    }
    nr_aiot_d2r_drain(d.get());
    auto got = Pop(d.get());
    ASSERT_EQ(got.size(), 1u);
    ExpectDecoded(got[0], tb, start, 4);
    EXPECT_EQ(got[0].decoded[0].start, want[0].decoded[0].start);
    EXPECT_EQ(got[0].decoded[0].rssi_w, want[0].decoded[0].rssi_w);
    if (wait) {
      EXPECT_EQ(nr_aiot_d2r_stats(d.get()).overruns, 0u);
    }
  }
}

int main(int argc, char **argv)
{
  logInit();
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
