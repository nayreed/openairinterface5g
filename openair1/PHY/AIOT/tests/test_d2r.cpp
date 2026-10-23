/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// aiot_d2r.c: the device's D2R frame against 38.291 6.1, and the Reader's receiver on frames that
// devices send, alone or together, under noise and the Reader's own carrier leak.

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "aiot_phy.h"

namespace {

std::vector<uint8_t> RandomBits(std::mt19937_64 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

// thermal noise over 5.76 MHz with a 6 dB noise figure
const double kNoiseW = 1.380649e-23 * 290 * AIOT_SAMPLE_RATE_HZ * std::pow(10, 0.6);

double DbmToAmplitude(double dbm)
{
  return std::sqrt(1e-3 * std::pow(10, dbm / 10));
}

using RxPtr = std::unique_ptr<aiot_d2r_rx_t, void (*)(aiot_d2r_rx_t *)>;

// Adds a device's backscatter of tb, at power dbm and phase ph, starting at sample at.
void AddFrame(std::vector<cd_t> &y, int at, const std::vector<uint8_t> &tb, const aiot_d2r_params_t &p, double dbm, double ph)
{
  const int len = aiot_frame_samples((int)tb.size(), &p);
  std::vector<double> w(len);
  ASSERT_EQ(aiot_d2r_waveform(tb.data(), (int)tb.size(), &p, w.data(), len), len);
  if ((int)y.size() < at + len)
    y.resize(at + len, cd_t{0, 0});
  const double amp = DbmToAmplitude(dbm);
  for (int i = 0; i < len; i++) {
    y[at + i].r += amp * std::cos(ph) * w[i];
    y[at + i].i += amp * std::sin(ph) * w[i];
  }
}

void AddNoiseAndLeak(std::vector<cd_t> &y, std::mt19937_64 &gen, double leak_dbm)
{
  std::normal_distribution<double> g(0, std::sqrt(kNoiseW / 2));
  const double leak = leak_dbm > -300 ? DbmToAmplitude(leak_dbm) : 0;
  const double ph = 2 * M_PI * std::uniform_real_distribution<double>(0, 1)(gen);
  for (auto &s : y) {
    s.r += g(gen) + leak * std::cos(ph);
    s.i += g(gen) + leak * std::sin(ph);
  }
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

// Feeds y to the receiver one millisecond at a time and returns the outcomes.
std::vector<aiot_d2r_outcome_t> Receive(aiot_d2r_rx_t *rx, const std::vector<cd_t> &y)
{
  std::vector<aiot_d2r_outcome_t> all;
  std::unique_ptr<aiot_d2r_outcome_t[]> out(new aiot_d2r_outcome_t[4]);
  for (size_t at = 0; at < y.size(); at += AIOT_SAMPLES_PER_MS) {
    int len = (int)std::min<size_t>(AIOT_SAMPLES_PER_MS, y.size() - at);
    int k = aiot_d2r_rx_receive(rx, y.data() + at, len, out.get(), 4);
    EXPECT_GE(k, 0);
    for (int i = 0; i < k; i++)
      all.push_back(out[i]);
  }
  return all;
}

// A spread of what 38.391 allows: every bit duration, both modulations, with and without coding
// and repetition, both amble lengths, each midamble interval.
const int kIntervals[4] = {48, 96, 168, 240}; // 38.391 Table 6.2.1.6-1, before the scale S

std::vector<aiot_d2r_params_t> Combinations()
{
  const int durations[8][3] = {{2, 1, 1}, {1, 1, 2}, {1, 2, 4}, {1, 4, 8}, {1, 8, 16}, {1, 16, 32}, {1, 32, 64}, {1, 96, 192}};
  std::vector<aiot_d2r_params_t> all;
  int k = 0;
  for (const auto &d : durations) {
    int r[8];
    const int nr = aiot_allowed_r_sfs(d[0], d[1], r);
    for (int ir = 0; ir < nr; ir += 3) {
      aiot_d2r_params_t p = {d[0],
                             d[1],
                             r[ir],
                             1 + (k & 1),
                             (k >> 1) & 1,
                             (k & 4) ? 7 : 31,
                             d[2] * kIntervals[(k >> 3) & 3],
                             (k >> 4) & 1,
                             ((k / 3) & 1) ? AIOT_OOK : AIOT_BPSK};
      k += 5;
      if (aiot_d2r_params_check(&p) == 0)
        all.push_back(p);
    }
  }
  return all;
}

} // namespace

// 38.291 6.1.2: CRC, then block repetition, then the TBCC
TEST(D2RTx, CodedBits)
{
  std::mt19937_64 gen(1);
  for (int r_block : {1, 2})
    for (int r_code : {0, 1})
      for (int a : {1, 30, 300}) {
        aiot_d2r_params_t p = {1, 8, 1, r_block, r_code, 31, 768, 0, AIOT_BPSK};
        std::vector<uint8_t> tb = RandomBits(gen, a);
        std::vector<uint8_t> word(a + 16), rep(2 * (a + 16)), want(6 * (a + 16)), e(6 * (a + 16));
        const int b = aiot_attach_crc(tb.data(), a, word.data());
        aiot_repeat_block(word.data(), b, r_block, rep.data());
        int n = b * r_block;
        if (r_code) {
          aiot_tbcc_encode(rep.data(), n, want.data());
          n *= 3;
        } else {
          std::copy(rep.begin(), rep.begin() + n, want.begin());
        }
        ASSERT_EQ(aiot_d2r_coded_bits(tb.data(), a, &p, e.data(), (int)e.size()), n);
        EXPECT_TRUE(std::equal(e.begin(), e.begin() + n, want.begin()));
      }
}

// 38.291 6.1.3 and 6.1.4: the preamble first, the coded bits in order between the midambles, every
// amble an m-sequence, and each bit sent as 2 R_SFS chips.
TEST(D2RTx, FrameAndChips)
{
  std::mt19937_64 gen(2);
  for (const aiot_d2r_params_t &p : Combinations())
    for (int a : {8, 96}) {
      std::vector<uint8_t> tb = RandomBits(gen, a);
      std::vector<uint8_t> e(6 * (a + 16));
      const int n_e = aiot_d2r_coded_bits(tb.data(), a, &p, e.data(), (int)e.size());
      const int n_v = aiot_frame_bits(a, &p);
      std::vector<uint8_t> v(n_v);
      std::vector<int> lay(n_v);
      ASSERT_EQ(aiot_d2r_assemble(tb.data(), a, &p, v.data(), n_v), n_v);
      ASSERT_EQ(aiot_d2r_amble_layout(n_e, &p, lay.data(), n_v), n_v);
      uint8_t amble[31];
      aiot_m_sequence(p.l_amble, amble);
      // 38.291 6.1.3, written out: V = E + (1 + floor(E / I_bit)) l_amble, or with ceil when I_add;
      // the preamble at v_k, a midamble at v_{k + K (l_amble + I_bit)} for K = 1 .. floor(E / I_bit),
      // the additional midamble at v_{k + V - l_amble}; the coded bits fill the rest in order
      const int l = p.l_amble, n_mid = n_e / p.i_bit;
      const int v_len = n_e + (1 + (p.i_add ? (n_e + p.i_bit - 1) / p.i_bit : n_mid)) * l;
      ASSERT_EQ(n_v, v_len);
      std::vector<int> want(v_len, -1);
      for (int k = 0; k < l; k++)
        want[k] = k;
      for (int K = 1; K <= n_mid; K++)
        for (int k = 0; k < l; k++)
          want[k + K * (l + p.i_bit)] = k;
      if (p.i_add)
        for (int k = 0; k < l; k++)
          want[k + v_len - l] = k;
      EXPECT_EQ(std::count(want.begin(), want.end(), -1), n_e);
      EXPECT_EQ(lay, want);
      int ie = 0;
      for (int i = 0; i < n_v; i++)
        EXPECT_EQ(v[i], lay[i] < 0 ? e[ie++] : amble[lay[i]]) << "bit " << i;
      EXPECT_EQ(ie, n_e);

      std::vector<double> w(aiot_frame_samples(a, &p));
      ASSERT_EQ(aiot_d2r_waveform(tb.data(), a, &p, w.data(), (int)w.size()), (int)w.size());
      const int per_bit = 2 * p.r_sfs, cs = aiot_chip_samples(&p);
      for (int i = 0; i < n_v; i += 1 + n_v / 50)
        for (int c = 0; c < per_bit; c++) {
          // 8.4: a 0 is (1, 0) (OOK) or (+1, -1) (BPSK) per pair of chips, a 1 the other way round
          const int first = (c & 1) == v[i];
          const double want = p.modulation == AIOT_OOK ? first : 2 * first - 1;
          for (int s = 0; s < cs; s++)
            ASSERT_EQ(w[((size_t)i * per_bit + c) * cs + s], want);
        }
    }
}

TEST(D2RTx, Errors)
{
  aiot_d2r_params_t good = {1, 16, 2, 1, 0, 7, 1536, 0, AIOT_BPSK};
  uint8_t tb[8] = {1, 0, 1, 1, 0, 0, 1, 0}, e[64], v[256];
  double w[4096];
  int lay[256];
  ASSERT_EQ(aiot_d2r_params_check(&good), 0);
  EXPECT_EQ(aiot_d2r_coded_bits(tb, 8, &good, e, 64), 14);
  aiot_d2r_params_t bad = good;
  bad.r_sfs = 8; // not allowed with 1/16 tau
  EXPECT_EQ(aiot_d2r_coded_bits(tb, 8, &bad, e, 64), AIOT_EINVAL);
  // the layout uses only L_amble, I_bit and I_add, so the shift does not matter to it
  EXPECT_EQ(aiot_d2r_amble_layout(14, &bad, lay, 256), 21);
  EXPECT_EQ(aiot_d2r_assemble(tb, 8, &bad, v, 256), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_sfs_chips(tb, 8, &bad, w, 4096), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_waveform(tb, 8, &bad, w, 4096), AIOT_EINVAL);
  bad = good;
  bad.i_bit = 100;
  EXPECT_EQ(aiot_d2r_waveform(tb, 8, &bad, w, 4096), AIOT_EINVAL);
  bad = good;
  bad.l_amble = 15;
  EXPECT_EQ(aiot_d2r_assemble(tb, 8, &bad, v, 256), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_amble_layout(14, &bad, lay, 256), AIOT_EINVAL);
  bad.l_amble = 7;
  bad.i_bit = 0;
  EXPECT_EQ(aiot_d2r_amble_layout(14, &bad, lay, 256), AIOT_EINVAL);
  bad = good;
  bad.r_block = 3;
  EXPECT_EQ(aiot_d2r_assemble(tb, 8, &bad, v, 256), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_coded_bits(tb, -1, &good, e, 64), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_coded_bits(tb, AIOT_MAX_TB_BITS + 1, &good, e, 64), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_coded_bits(tb, 8, &good, e, 13), AIOT_ESPACE);
}

// One device answers somewhere in the window, with the Reader's carrier leaking into its receiver
// 40 dB above the answer: the Reader decodes the bits, the shift and the start.
TEST(D2RRx, ReceivesEveryCombination)
{
  std::mt19937_64 gen(3);
  std::uniform_real_distribution<double> u(0, 1);
  ASSERT_GE(Combinations().size(), 12u);
  for (const aiot_d2r_params_t &p : Combinations())
    for (int a : {16, 96}) {
      std::vector<uint8_t> tb = RandomBits(gen, a);
      const int te = 4 * aiot_chip_samples(&p) + 20, expected = 3000 + te;
      const int start = expected + (int)(gen() % (2 * te + 1)) - te;
      std::vector<cd_t> y(start, cd_t{0, 0});
      AddFrame(y, start, tb, p, -80, 2 * M_PI * u(gen));
      y.resize(y.size() + te + 2 * AIOT_SAMPLES_PER_MS, cd_t{0, 0});
      AddNoiseAndLeak(y, gen, -40);
      RxPtr rx(aiot_d2r_rx_new(kNoiseW, 0), aiot_d2r_rx_free);
      aiot_d2r_expect_t e = Expect(expected, te, a, p);
      ASSERT_EQ(aiot_d2r_rx_expect(rx.get(), &e), 0);
      std::vector<aiot_d2r_outcome_t> out = Receive(rx.get(), y);
      ASSERT_EQ(out.size(), 1u);
      ASSERT_EQ(out[0].n_decoded, 1) << "T_bit " << p.t_bit_num << "/" << p.t_bit_den << ", R_SFS " << p.r_sfs << ", A " << a;
      const aiot_d2r_result_t &r = out[0].decoded[0];
      EXPECT_EQ(r.n_bits, a);
      EXPECT_TRUE(std::equal(tb.begin(), tb.end(), r.bits));
      EXPECT_EQ(r.r_sfs, p.r_sfs);
      EXPECT_EQ(r.start, start);
      EXPECT_EQ(out[0].collided, 0);
    }
}

// The Reader allowed several shifts (paging for random access): it finds the answer on its own.
TEST(D2RRx, FindsTheShiftAmongThoseAllowed)
{
  std::mt19937_64 gen(4);
  const std::vector<int> shifts = {1, 2, 4, 8};
  for (int r_sfs : shifts) {
    aiot_d2r_params_t p = {1, 8, r_sfs, 1, 1, 31, 768, 0, AIOT_BPSK};
    std::vector<uint8_t> tb = RandomBits(gen, 16);
    std::vector<cd_t> y(4000, cd_t{0, 0});
    AddFrame(y, 4000, tb, p, -85, 1.0);
    y.resize(y.size() + 2 * AIOT_SAMPLES_PER_MS, cd_t{0, 0});
    AddNoiseAndLeak(y, gen, -1000);
    RxPtr rx(aiot_d2r_rx_new(kNoiseW, 1), aiot_d2r_rx_free);
    p.r_sfs = 1;
    aiot_d2r_expect_t e = Expect(4000, 100, 16, p, shifts);
    ASSERT_EQ(aiot_d2r_rx_expect(rx.get(), &e), 0);
    std::vector<aiot_d2r_outcome_t> out = Receive(rx.get(), y);
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].n_decoded, 1) << "R_SFS " << r_sfs;
    EXPECT_EQ(out[0].decoded[0].r_sfs, r_sfs);
    EXPECT_TRUE(std::equal(tb.begin(), tb.end(), out[0].decoded[0].bits));
  }
}

TEST(D2RRx, FindsNothingInNoise)
{
  std::mt19937_64 gen(5);
  const aiot_d2r_params_t p = {1, 8, 1, 1, 1, 31, 768, 0, AIOT_BPSK};
  int decoded = 0, collided = 0;
  for (int t = 0; t < 50; t++) {
    std::vector<cd_t> y(3 * AIOT_SAMPLES_PER_MS, cd_t{0, 0});
    AddNoiseAndLeak(y, gen, -40);
    RxPtr rx(aiot_d2r_rx_new(kNoiseW, 1), aiot_d2r_rx_free);
    aiot_d2r_expect_t e = Expect(1000, 200, 16, p, {1, 2, 4, 8});
    ASSERT_EQ(aiot_d2r_rx_expect(rx.get(), &e), 0);
    for (const auto &o : Receive(rx.get(), y)) {
      decoded += o.n_decoded;
      collided += o.collided;
    }
  }
  EXPECT_EQ(decoded, 0);
  EXPECT_LE(collided, 1);
}

TEST(D2RRx, Errors)
{
  RxPtr rx(aiot_d2r_rx_new(1.0, 1), aiot_d2r_rx_free);
  aiot_d2r_expect_t e = Expect(1000, 10, 8, {1, 16, 2, 1, 0, 7, 1536, 0, AIOT_BPSK});
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &e), 0);
  aiot_d2r_expect_t bad = e;
  bad.n_r_sfs = 2;
  bad.r_sfs[0] = 1;
  bad.r_sfs[1] = 8; // not allowed with 1/16 tau
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);
  bad = e;
  bad.n_r_sfs = 9;
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);
  bad = e;
  bad.te = -1;
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);
  bad = e;
  bad.a_bits = AIOT_MAX_TB_BITS + 1;
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);
  bad = e;
  bad.params.l_amble = 8;
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);
  bad = e;
  bad.params.i_bit = 7;
  EXPECT_EQ(aiot_d2r_rx_expect(rx.get(), &bad), AIOT_EINVAL);

  // a window that completes with no room for the outcome leaves the receiver untouched
  std::vector<cd_t> y(5000, cd_t{0.0, 0.0});
  std::unique_ptr<aiot_d2r_outcome_t[]> out(new aiot_d2r_outcome_t[1]);
  EXPECT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), 5000, out.get(), 0), AIOT_ESPACE);
  EXPECT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), 5000, out.get(), 1), 1);
  EXPECT_EQ(out[0].n_decoded, 0);
  EXPECT_EQ(out[0].collided, 0);
  EXPECT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), 100, out.get(), 1), 0);
  EXPECT_EQ(aiot_d2r_rx_receive(nullptr, y.data(), 100, out.get(), 1), AIOT_EINVAL);
  EXPECT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), -1, out.get(), 1), AIOT_EINVAL);
}

// Two devices on different shifts, each strong and alone on its shift: both decode, and what
// cancelling one leaves on the other's shift is not counted as a collision.
TEST(D2RRx, CancellationResidueIsNotACollision)
{
  std::mt19937_64 gen(11);
  std::uniform_real_distribution<double> u(0, 1);
  const int shifts[4] = {1, 2, 4, 8}, a = 40, te = 200, start = 2000, trials = 100;
  int decoded = 0, collided = 0;
  for (int t = 0; t < trials; t++) {
    int r0 = shifts[gen() % 4], r1;
    do
      r1 = shifts[gen() % 4];
    while (r1 == r0);
    std::vector<cd_t> y;
    for (int r : {r0, r1}) {
      aiot_d2r_params_t p = {1, 8, r, 1, 1, 31, 768, 0, AIOT_BPSK};
      AddFrame(y, start + (int)(gen() % (te + 1)) - te / 2, RandomBits(gen, a), p, -60 + 30 * u(gen), 2 * M_PI * u(gen));
    }
    y.resize(y.size() + 3000, cd_t{0, 0});
    AddNoiseAndLeak(y, gen, -1000);
    RxPtr rx(aiot_d2r_rx_new(kNoiseW, 1), aiot_d2r_rx_free);
    aiot_d2r_expect_t e = Expect(start, te, a, {1, 8, 1, 1, 1, 31, 768, 0, AIOT_BPSK}, {1, 2, 4, 8});
    ASSERT_EQ(aiot_d2r_rx_expect(rx.get(), &e), 0);
    aiot_d2r_outcome_t out[2];
    ASSERT_EQ(aiot_d2r_rx_receive(rx.get(), y.data(), (int)y.size(), out, 2), 1);
    decoded += out[0].n_decoded;
    collided += out[0].collided;
  }
  EXPECT_EQ(decoded, 2 * trials);
  EXPECT_LE(collided, 2);
}
