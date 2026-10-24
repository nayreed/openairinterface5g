/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// aiot_r2d.c: the Reader's R2D transmitter against 38.291 6.2, and the device's receiver on what
// the transmitter sends.

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <random>
#include <vector>
#include "aiot_phy.h"

namespace {

const int kM[4] = {2, 6, 12, 24};
const uint8_t kSip[8] = {1, 1, 0, 0, 1, 0, 0, 0};
const uint8_t kCap[4] = {1, 0, 1, 0};

std::vector<uint8_t> RandomBits(std::mt19937 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

std::vector<uint8_t> Chips(const std::vector<uint8_t> &tb, int m)
{
  std::vector<uint8_t> chips(4 * (int)tb.size() + 200);
  int n = aiot_r2d_chips(tb.data(), (int)tb.size(), m, chips.data(), (int)chips.size());
  EXPECT_GT(n, 0);
  chips.resize(std::max(n, 0));
  return chips;
}

using RxPtr = std::unique_ptr<aiot_r2d_rx_t, void (*)(aiot_r2d_rx_t *)>;
RxPtr NewRx()
{
  return RxPtr(aiot_r2d_rx_new(), aiot_r2d_rx_free);
}

// Feeds y to rx in pieces of block samples and returns every frame found.
std::vector<aiot_r2d_frame_t> Receive(aiot_r2d_rx_t *rx, const std::vector<cd_t> &y, int block)
{
  std::vector<aiot_r2d_frame_t> found;
  std::vector<aiot_r2d_frame_t> fr(4);
  for (size_t p = 0; p < y.size(); p += block) {
    int len = (int)std::min<size_t>(block, y.size() - p);
    int k = aiot_r2d_rx_receive(rx, y.data() + p, len, fr.data(), (int)fr.size());
    EXPECT_GE(k, 0);
    for (int i = 0; i < k; i++)
      found.push_back(fr[i]);
  }
  return found;
}

// The R2D transmission of tb, placed at sample at of a stream of total samples.
void Place(std::vector<cd_t> &y, int at, const std::vector<uint8_t> &tb, int m, int n_rb)
{
  std::vector<cd_t> x(400000);
  int n = aiot_r2d_transmission(tb.data(), (int)tb.size(), m, n_rb, x.data(), (int)x.size());
  ASSERT_GT(n, 0);
  if ((int)y.size() < at + n)
    y.resize(at + n, cd_t{0, 0});
  for (int i = 0; i < n; i++) {
    y[at + i].r += x[i].r;
    y[at + i].i += x[i].i;
  }
}

void AddNoise(std::vector<cd_t> &y, double noise_w, std::mt19937 &gen)
{
  std::normal_distribution<double> g(0, std::sqrt(noise_w / 2));
  for (auto &s : y) {
    s.r += g(gen);
    s.i += g(gen);
  }
}

// A carrier from another node: an unbroken tone power_db above an "on" chip, offset_hz from the R2D.
void AddCarrier(std::vector<cd_t> &y, double power_db, double offset_hz)
{
  const double a = std::pow(10.0, power_db / 20), w = 2 * M_PI * offset_hz / AIOT_SAMPLE_RATE_HZ;
  for (size_t n = 0; n < y.size(); n++) {
    y[n].r += a * std::cos(w * (double)n);
    y[n].i += a * std::sin(w * (double)n);
  }
}

} // namespace

TEST(R2DTx, Manchester)
{
  const uint8_t bits[3] = {0, 1, 1};
  uint8_t out[6];
  ASSERT_EQ(aiot_manchester(bits, 3, out), 6);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 6), (std::vector<uint8_t>{1, 0, 0, 1, 0, 1}));
}

// 38.291 6.2.2 to 6.2.5: SIP, CAP, Manchester(TB and CRC), postamble, padding to whole symbols,
// and with M = 24 the last two chips of each symbol forced to 1.
TEST(R2DTx, ChipsFollowTheSpec)
{
  std::mt19937 gen(1);
  for (int m : kM)
    for (int a : {0, 1, 24, 25, 96, 1000}) {
      std::vector<uint8_t> tb = RandomBits(gen, a);
      std::vector<uint8_t> chips = Chips(tb, m);
      ASSERT_GE(chips.size(), 12u);
      EXPECT_TRUE(std::equal(kSip, kSip + 8, chips.begin()));
      EXPECT_TRUE(std::equal(kCap, kCap + 4, chips.begin() + 8));
      EXPECT_EQ((chips.size() - 8) % m, 0u) << "M = " << m;
      EXPECT_EQ(aiot_r2d_n_symbols((int)chips.size(), m), 2 + (int)(chips.size() - 8) / m);

      std::vector<uint8_t> body;
      for (size_t c = 12; c < chips.size(); c++) {
        if (m == 24 && (c - 8) % 24 >= 22) {
          EXPECT_EQ(chips[c], 1) << "forced chip " << c;
          continue;
        }
        body.push_back(chips[c]);
      }
      std::vector<uint8_t> word(a + 16);
      int b = aiot_attach_crc(tb.data(), a, word.data());
      std::vector<uint8_t> want(2 * b);
      aiot_manchester(word.data(), b, want.data());
      want.insert(want.end(), {1, 1, 1, 1});
      ASSERT_GE(body.size(), want.size());
      EXPECT_TRUE(std::equal(want.begin(), want.end(), body.begin())) << "M = " << m << ", A = " << a;
      EXPECT_TRUE(std::all_of(body.begin() + want.size(), body.end(), [](uint8_t c) { return c == 0; })) << "padding";
      // the smallest N_pad: less than one symbol's worth of free chips
      EXPECT_LT(body.size() - want.size(), (size_t)(m == 24 ? 22 : m)) << "M = " << m << ", A = " << a;
    }
}

// 38.291 Table 4.3.3.3-1: the fewest PRBs for each M.
TEST(R2DTx, MinimumPrbs)
{
  const int want[][2] = {{2, 1}, {6, 1}, {12, 2}, {24, 3}};
  for (const auto &w : want)
    EXPECT_EQ(aiot_n_rb_min(w[0]), w[1]) << "M = " << w[0];
}

// The R2D fixed reference channels of 38.194 Annex B.2 (TBS 96, CRC 16, M = 6, 12, 24): 8 SIP
// chips, then the CAP (4), the Manchester-coded 112 bits (224), the postamble (4) and padding.
// B.2 lists 2 padding chips for M = 6 and 8 for M = 12, which 38.291 6.2.5 gives too: 242 and 248
// chips. For M = 24 B.2 lists 8, but 6.2.5 skips the last two chips of every symbol for the PRDCH
// and the postamble, so 232 chips need 11 symbols of 22 and the last symbol ends with 10 padding
// chips and the two forced ones (12): 272 chips. This follows 38.291, the normative mapping.
TEST(R2DTx, FixedReferenceChannelsOf38194)
{
  std::vector<uint8_t> tb(96, 0);
  EXPECT_EQ(Chips(tb, 6).size(), 242u);
  EXPECT_EQ(Chips(tb, 12).size(), 248u);
  EXPECT_EQ(Chips(tb, 24).size(), 272u);
}

// 38.291 6.2.6 and 6.2.7: OFDM symbols with the long CP at l = 0 and 7, every symbol's spectrum
// within the N_RB x 12 subcarriers around DC.
TEST(R2DTx, WaveformIsBandLimitedOfdm)
{
  std::mt19937 gen(2);
  for (int m : kM)
    for (int n_rb : {aiot_n_rb_min(m), 4}) {
      std::vector<uint8_t> chips = Chips(RandomBits(gen, 40), m);
      const int n_sym = aiot_r2d_n_symbols((int)chips.size(), m);
      std::vector<cd_t> x(aiot_r2d_n_samples(n_sym));
      ASSERT_EQ(aiot_r2d_waveform(chips.data(), (int)chips.size(), m, n_rb, x.data(), (int)x.size()), (int)x.size());
      int start = 0;
      for (int l = 0; l < n_sym; l++) {
        const int cp = l % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
        const cd_t *sym = &x[start + cp];
        for (int i = 0; i < cp; i++) {
          EXPECT_NEAR(x[start + i].r, sym[AIOT_N_FFT - cp + i].r, 1e-12);
          EXPECT_NEAR(x[start + i].i, sym[AIOT_N_FFT - cp + i].i, 1e-12);
        }
        double in = 0, out = 0;
        for (int k = 0; k < AIOT_N_FFT; k++) {
          std::complex<double> bin = 0;
          for (int t = 0; t < AIOT_N_FFT; t++)
            bin += std::complex<double>(sym[t].r, sym[t].i) * std::polar(1.0, -2 * M_PI * k * t / AIOT_N_FFT);
          const int f = k < AIOT_N_FFT / 2 ? k : k - AIOT_N_FFT;
          (std::abs(f + 0.5) < n_rb * 6 ? in : out) += std::norm(bin);
        }
        EXPECT_LT(out, 1e-20 * in) << "M = " << m << ", N_RB = " << n_rb << ", symbol " << l;
        start += cp + AIOT_N_FFT;
      }
      EXPECT_EQ(start, (int)x.size());
    }
}

// 6.2.7 in the frequency domain: each symbol's resource elements are the subcarriers q - K/2 of its
// on/off chip waveform, divided by 384, computed here with std::complex; and the waveform's useful
// part is their sum, so a transmitter with its own grid sends the same signal.
TEST(R2DTx, ResourceElementsAreTheWaveformsSubcarriers)
{
  std::mt19937 gen(5);
  const int N = AIOT_N_FFT;
  for (int m : kM)
    for (int n_rb : {aiot_n_rb_min(m), 3}) {
      std::vector<uint8_t> chips = Chips(RandomBits(gen, 40), m);
      const int n_sym = aiot_r2d_n_symbols((int)chips.size(), m);
      const int K = n_rb * AIOT_SC_PER_RB;
      std::vector<cd_t> re(aiot_r2d_n_resource_elements(n_sym, n_rb));
      ASSERT_EQ(re.size(), (size_t)n_sym * K);
      ASSERT_EQ(aiot_r2d_resource_elements(chips.data(), (int)chips.size(), m, n_rb, re.data(), (int)re.size()), (int)re.size());
      std::vector<cd_t> x(aiot_r2d_n_samples(n_sym));
      ASSERT_EQ(aiot_r2d_waveform(chips.data(), (int)chips.size(), m, n_rb, x.data(), (int)x.size()), (int)x.size());
      int start = 0;
      for (int l = 0; l < n_sym; l++) {
        const int per = l < 2 ? 4 : m;
        const uint8_t *c = l < 2 ? &chips[4 * l] : &chips[8 + (l - 2) * m];
        for (int q = 0; q < K; q++) {
          std::complex<double> bin = 0;
          for (int t = 0; t < N; t++)
            if (c[t / (N / per)])
              bin += std::polar(1.0, -2 * M_PI * (q - K / 2) * t / N);
          bin /= N;
          EXPECT_NEAR(re[l * K + q].r, bin.real(), 1e-12) << "M = " << m << ", symbol " << l << ", q = " << q;
          EXPECT_NEAR(re[l * K + q].i, bin.imag(), 1e-12) << "M = " << m << ", symbol " << l << ", q = " << q;
        }
        const int cp = l % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
        for (int t = 0; t < N; t++) {
          std::complex<double> v = 0;
          for (int q = 0; q < K; q++)
            v += std::complex<double>(re[l * K + q].r, re[l * K + q].i) * std::polar(1.0, 2 * M_PI * (q - K / 2) * t / N);
          EXPECT_NEAR(x[start + cp + t].r, v.real(), 1e-9);
          EXPECT_NEAR(x[start + cp + t].i, v.imag(), 1e-9);
        }
        start += cp + N;
      }
    }
}

TEST(R2DTx, ResourceElementsRejectBadArguments)
{
  uint8_t tb[8] = {1, 0, 1, 1, 0, 0, 1, 0};
  uint8_t chips[400];
  std::vector<cd_t> re(4000);
  int nc = aiot_r2d_chips(tb, 8, 6, chips, 400);
  ASSERT_GT(nc, 0);
  const int n = aiot_r2d_n_resource_elements(aiot_r2d_n_symbols(nc, 6), 1);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 6, 1, re.data(), (int)re.size()), n);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 6, 1, re.data(), n - 1), AIOT_ESPACE);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc - 1, 6, 1, re.data(), (int)re.size()), AIOT_EINVAL); // not whole symbols
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 5, 1, re.data(), (int)re.size()), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 6, 0, re.data(), (int)re.size()), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 6, 33, re.data(), (int)re.size()), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_resource_elements(nullptr, nc, 6, 1, re.data(), (int)re.size()), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_resource_elements(chips, nc, 6, 1, nullptr, (int)re.size()), AIOT_EINVAL);
}

TEST(R2DTx, RejectsWhatTheSpecDoesNotAllow)
{
  uint8_t tb[8] = {1, 0, 1, 1, 0, 0, 1, 0};
  uint8_t chips[400];
  cd_t x[20000];
  for (int m : {0, 1, 3, 4, 8, 13, 48, -2})
    EXPECT_EQ(aiot_r2d_chips(tb, 8, m, chips, 400), AIOT_EINVAL) << m;
  EXPECT_EQ(aiot_r2d_chips(tb, -1, 6, chips, 400), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_chips(tb, 8, 6, chips, 20), AIOT_ESPACE);
  int nc = aiot_r2d_chips(tb, 8, 12, chips, 400);
  ASSERT_GT(nc, 0);
  // M = 12 needs 2 PRBs, M = 24 needs 3
  EXPECT_EQ(aiot_r2d_waveform(chips, nc, 12, 1, x, 20000), AIOT_EINVAL);
  EXPECT_GT(aiot_r2d_waveform(chips, nc, 12, 2, x, 20000), 0);
  nc = aiot_r2d_chips(tb, 8, 24, chips, 400);
  EXPECT_EQ(aiot_r2d_waveform(chips, nc, 24, 2, x, 20000), AIOT_EINVAL);
  EXPECT_GT(aiot_r2d_waveform(chips, nc, 24, 3, x, 20000), 0);
  EXPECT_EQ(aiot_r2d_waveform(chips, nc, 24, 0, x, 20000), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_waveform(chips, nc, 24, 33, x, 20000), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_waveform(chips, nc, 5, 4, x, 20000), AIOT_EINVAL);
}

// Every M and bandwidth, short and long blocks, at any offset and any piece size the caller reads.
TEST(R2DRx, ReceivesWhatTheReaderSends)
{
  std::mt19937 gen(3);
  for (int m : kM)
    for (int n_rb : {aiot_n_rb_min(m), 3, 6})
      for (int a : {1, 30, 96, 400}) {
        std::vector<uint8_t> tb = RandomBits(gen, a);
        const int at = 1000 + (int)(gen() % 3000);
        std::vector<cd_t> y(at, cd_t{0, 0});
        Place(y, at, tb, m, n_rb);
        const int end = (int)y.size();
        y.resize(end + 6000, cd_t{0, 0});
        const int block = std::vector<int>{AIOT_SAMPLES_PER_MS, 1000, 811}[gen() % 3];
        RxPtr rx = NewRx();
        std::vector<aiot_r2d_frame_t> found = Receive(rx.get(), y, block);
        ASSERT_EQ(found.size(), 1u) << "M = " << m << ", N_RB = " << n_rb << ", A = " << a;
        EXPECT_EQ(found[0].m_chip_symb, m);
        EXPECT_EQ(found[0].n_bits, a);
        EXPECT_TRUE(std::equal(tb.begin(), tb.end(), found[0].bits));
        EXPECT_EQ(found[0].end_sample, end);
      }
}

// A gNB maps the resource elements onto PRBs of its own carrier (here 25 PRBs, the 300 subcarriers
// centred in a 384-point grid, as OAI lays them out) and starts on a half-subframe. A device
// receives it wherever it sits in the carrier: a shift by whole subcarriers leaves the envelope as
// it is.
TEST(R2DRx, ReceivesTheResourceElementsOnACarriersGrid)
{
  std::mt19937 gen(6);
  const int N = AIOT_N_FFT, n_prb = 25, first = N - n_prb * 6;
  for (int m : kM)
    for (int prb0 : {0, 11, 25 - aiot_n_rb_min(m)}) {
      const int n_rb = aiot_n_rb_min(m), K = n_rb * AIOT_SC_PER_RB;
      std::vector<uint8_t> tb = RandomBits(gen, 96);
      std::vector<uint8_t> chips = Chips(tb, m);
      const int n_sym = aiot_r2d_n_symbols((int)chips.size(), m);
      std::vector<cd_t> re(aiot_r2d_n_resource_elements(n_sym, n_rb));
      ASSERT_GT(aiot_r2d_resource_elements(chips.data(), (int)chips.size(), m, n_rb, re.data(), (int)re.size()), 0);
      const int at = AIOT_HALF_SUBFRAME;
      std::vector<cd_t> y(at, cd_t{0, 0});
      for (int l = 0; l < n_sym; l++) {
        std::vector<std::complex<double>> grid(N, 0);
        for (int q = 0; q < K; q++)
          grid[(first + 12 * prb0 + q) % N] = std::complex<double>(re[l * K + q].r, re[l * K + q].i);
        std::vector<cd_t> u(N);
        for (int t = 0; t < N; t++) {
          std::complex<double> v = 0;
          for (int k = 0; k < N; k++)
            if (grid[k] != 0.0)
              v += grid[k] * std::polar(1.0, 2 * M_PI * k * t / N);
          u[t] = cd_t{v.real(), v.imag()};
        }
        const int cp = l % 7 == 0 ? AIOT_CP_LONG : AIOT_CP_SHORT;
        y.insert(y.end(), u.end() - cp, u.end());
        y.insert(y.end(), u.begin(), u.end());
      }
      const int end = (int)y.size();
      y.resize(end + 6000, cd_t{0, 0});
      RxPtr rx = NewRx();
      std::vector<aiot_r2d_frame_t> found = Receive(rx.get(), y, AIOT_SAMPLES_PER_MS);
      ASSERT_EQ(found.size(), 1u) << "M = " << m << ", PRB " << prb0;
      EXPECT_EQ(found[0].n_bits, 96);
      EXPECT_TRUE(std::equal(tb.begin(), tb.end(), found[0].bits));
      EXPECT_EQ(found[0].end_sample, end);
    }
}

// The R2D fixed reference channels A-FR1-B1-1 to -3 of 38.194 Annex B.2 in noise: at 10 dB SNR (an
// "on" chip, of power 1, over the noise) the device detects every one of a few transmissions back
// to back. 38.191 7.2.2 gives the device's sensitivity as a power (REFSENS, -34 dBm, 90% detected),
// not an SNR: 10 dB is this test's point, the same as aiot_sim check's.
TEST(R2DRx, ReceivesInNoise)
{
  std::mt19937 gen(4);
  const int frc[3][2] = {{6, 1}, {12, 2}, {24, 3}};
  for (const auto &f : frc) {
    std::vector<std::vector<uint8_t>> tbs;
    std::vector<cd_t> y(2000, cd_t{0, 0});
    for (int k = 0; k < 4; k++) {
      tbs.push_back(RandomBits(gen, 96));
      Place(y, (int)y.size(), tbs.back(), f[0], f[1]);
      y.resize(y.size() + 3000, cd_t{0, 0});
    }
    AddNoise(y, std::pow(10.0, -10.0 / 10), gen);
    RxPtr rx = NewRx();
    std::vector<aiot_r2d_frame_t> found = Receive(rx.get(), y, AIOT_SAMPLES_PER_MS);
    ASSERT_EQ(found.size(), tbs.size()) << "M = " << f[0];
    for (size_t k = 0; k < tbs.size(); k++)
      EXPECT_TRUE(std::equal(tbs[k].begin(), tbs[k].end(), found[k].bits)) << "M = " << f[0] << ", frame " << k;
  }
}

TEST(R2DRx, FindsNothingInNoiseOrWhenOff)
{
  std::mt19937 gen(5);
  std::vector<cd_t> y(AIOT_SAMPLES_PER_MS * 200, cd_t{0, 0});
  AddNoise(y, 1.0, gen);
  RxPtr rx = NewRx();
  EXPECT_TRUE(Receive(rx.get(), y, AIOT_SAMPLES_PER_MS).empty());

  std::vector<cd_t> frame(1000, cd_t{0, 0});
  Place(frame, 1000, RandomBits(gen, 40), 6, 1);
  frame.resize(frame.size() + 3000, cd_t{0, 0});
  RxPtr off = NewRx();
  aiot_r2d_rx_set_enabled(off.get(), 0);
  EXPECT_TRUE(Receive(off.get(), frame, AIOT_SAMPLES_PER_MS).empty());
}

// A carrier from another node under the R2D, as from a CW node in the UL band: the device takes
// its threshold from the SIP, so at 20 dB SNR it decodes every frame under a carrier as strong as
// the "on" chips and under one 6 dB stronger, and finds nothing in a carrier and noise alone.
// 38.191 8.4.3 measures the device's sensitivity with a carrier on, 10 dB above REFSENS.
TEST(R2DRx, ReceivesUnderACarrierFromAnotherNode)
{
  std::mt19937 gen(6);
  const double kOffsetHz = 1.92e6;
  for (double carrier_db : {0.0, 6.0}) {
    std::vector<std::vector<uint8_t>> tbs;
    std::vector<cd_t> y(2000, cd_t{0, 0});
    for (int k = 0; k < 4; k++) {
      tbs.push_back(RandomBits(gen, 96));
      Place(y, (int)y.size(), tbs.back(), 6, 1);
      y.resize(y.size() + 3000, cd_t{0, 0});
    }
    AddCarrier(y, carrier_db, kOffsetHz);
    AddNoise(y, std::pow(10.0, -20.0 / 10), gen);
    RxPtr rx = NewRx();
    std::vector<aiot_r2d_frame_t> found = Receive(rx.get(), y, AIOT_SAMPLES_PER_MS);
    ASSERT_EQ(found.size(), tbs.size()) << "carrier " << carrier_db << " dB";
    for (size_t k = 0; k < tbs.size(); k++)
      EXPECT_TRUE(std::equal(tbs[k].begin(), tbs[k].end(), found[k].bits)) << "carrier " << carrier_db << " dB, frame " << k;
  }

  std::vector<cd_t> y(AIOT_SAMPLES_PER_MS * 200, cd_t{0, 0});
  AddCarrier(y, 20.0, kOffsetHz);
  AddNoise(y, 1.0, gen);
  RxPtr rx = NewRx();
  EXPECT_TRUE(Receive(rx.get(), y, AIOT_SAMPLES_PER_MS).empty());
}

TEST(R2DRx, RejectsBadArguments)
{
  RxPtr rx = NewRx();
  ASSERT_NE(rx, nullptr);
  cd_t y[10] = {};
  aiot_r2d_frame_t fr;
  EXPECT_EQ(aiot_r2d_rx_receive(nullptr, y, 10, &fr, 1), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_rx_receive(rx.get(), nullptr, 10, &fr, 1), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_rx_receive(rx.get(), y, -1, &fr, 1), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_rx_receive(rx.get(), y, 10, nullptr, 1), AIOT_EINVAL);
  EXPECT_EQ(aiot_r2d_rx_receive(rx.get(), y, 0, &fr, 1), 0);
  aiot_r2d_rx_free(nullptr);
}
