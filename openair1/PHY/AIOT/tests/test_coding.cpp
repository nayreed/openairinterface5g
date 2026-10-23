/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// aiot_params.c and aiot_coding.c against the definitions of 38.291, 38.391 and 38.212, written out
// again here in their simplest form.

#include <gtest/gtest.h>
#include <random>
#include <vector>
#include "aiot_phy.h"

namespace {

std::vector<uint8_t> RandomBits(std::mt19937 &gen, int n)
{
  std::vector<uint8_t> b(n);
  for (auto &x : b)
    x = gen() & 1;
  return b;
}

// 38.391 Table 6.2.1.6-1: bit duration (in tau), largest R_SFS, scale S of the Interval Bits
struct BitDuration {
  int num, den, max_r_sfs, scale;
};
const BitDuration kBitDurations[] = {
    {2, 1, 128, 1},
    {1, 1, 64, 2},
    {1, 2, 32, 4},
    {1, 4, 16, 8},
    {1, 8, 8, 16},
    {1, 16, 4, 32},
    {1, 32, 2, 64},
    {1, 96, 1, 192},
};
const int kIntervalBits[4] = {48, 96, 168, 240};

// 38.212 5.1: the CRC of bits with generator poly (the bits below D^L), zero start, no final XOR
std::vector<uint8_t> ReferenceCrc(const std::vector<uint8_t> &bits, int L, uint32_t poly)
{
  uint32_t reg = 0;
  for (uint8_t b : bits) {
    uint32_t msb = (reg >> (L - 1)) & 1;
    reg = (reg << 1) & ((1u << L) - 1);
    if (msb ^ b)
      reg ^= poly;
  }
  std::vector<uint8_t> p(L);
  for (int i = 0; i < L; i++)
    p[i] = (reg >> (L - 1 - i)) & 1;
  return p;
}
const uint32_t kCrc6 = 0x21; // D^6 + D^5 + 1
const uint32_t kCrc16 = 0x1021; // D^16 + D^12 + D^5 + 1

// 36.212 5.1.3.1: tail-biting convolutional code, the shift register starting with the last 6 bits
std::vector<uint8_t> ReferenceTbcc(const std::vector<uint8_t> &c)
{
  const int g[3] = {0133, 0171, 0165};
  const int k = static_cast<int>(c.size());
  std::vector<uint8_t> e(3 * k);
  for (int n = 0; n < k; n++)
    for (int i = 0; i < 3; i++) {
      int d = 0;
      for (int l = 0; l <= 6; l++)
        if ((g[i] >> (6 - l)) & 1)
          d ^= c[((n - l) % k + k) % k];
      e[3 * n + i] = d;
    }
  return e;
}

std::vector<double> Soft(const std::vector<uint8_t> &bits)
{
  std::vector<double> s(bits.size());
  for (size_t i = 0; i < bits.size(); i++)
    s[i] = bits[i] ? -1.0 : 1.0;
  return s;
}

} // namespace

TEST(Params, MSequencesAreMaximalLength)
{
  for (int len : {7, 31}) {
    std::vector<uint8_t> s(len);
    ASSERT_EQ(aiot_m_sequence(len, s.data()), len);
    // the periodic autocorrelation of an m-sequence is -1 at every non-zero shift
    for (int shift = 1; shift < len; shift++) {
      int acc = 0;
      for (int n = 0; n < len; n++)
        acc += (s[n] ? -1 : 1) * (s[(n + shift) % len] ? -1 : 1);
      EXPECT_EQ(acc, -1) << "length " << len << " shift " << shift;
    }
  }
  uint8_t s[31];
  EXPECT_EQ(aiot_m_sequence(15, s), AIOT_EINVAL);
}

TEST(Params, ShiftsAndIntervalsOfEachBitDuration)
{
  for (const auto &d : kBitDurations) {
    int r[8], ib[4];
    int nr = aiot_allowed_r_sfs(d.num, d.den, r);
    ASSERT_GT(nr, 0);
    EXPECT_EQ(r[nr - 1], d.max_r_sfs);
    for (int i = 0; i < nr; i++)
      EXPECT_EQ(r[i], 1 << i);
    ASSERT_EQ(aiot_i_bit_values(d.num, d.den, ib), 4);
    for (int i = 0; i < 4; i++)
      EXPECT_EQ(ib[i], d.scale * kIntervalBits[i]);
  }
  int r[8];
  EXPECT_EQ(aiot_allowed_r_sfs(1, 3, r), AIOT_EINVAL);
  EXPECT_EQ(aiot_allowed_r_sfs(0, 1, r), AIOT_EINVAL);
}

TEST(Params, FrameLengthOfEveryAllowedCombination)
{
  int checked = 0;
  for (const auto &d : kBitDurations)
    for (int r_sfs = 1; r_sfs <= d.max_r_sfs; r_sfs *= 2)
      for (int r_block : {1, 2})
        for (int r_code : {0, 1})
          for (int l_amble : {7, 31})
            for (int i : {0, 3})
              for (int i_add : {0, 1})
                for (aiot_mod_t mod : {AIOT_OOK, AIOT_BPSK}) {
                  aiot_d2r_params_t p = {d.num, d.den, r_sfs, r_block, r_code, l_amble, d.scale * kIntervalBits[i], i_add, mod};
                  const int bit = AIOT_TAU * d.num / d.den;
                  // a chip that is not a whole number of samples cannot be sent at 5.76 Msps
                  const bool whole = AIOT_TAU * d.num % d.den == 0 && bit % (2 * r_sfs) == 0;
                  EXPECT_EQ(aiot_d2r_params_check(&p), whole ? 0 : AIOT_EINVAL);
                  if (!whole)
                    continue;
                  EXPECT_EQ(aiot_bit_samples(&p), bit);
                  EXPECT_EQ(aiot_chip_samples(&p), bit / (2 * r_sfs));
                  for (int a : {1, 24, 25, 96, 1000}) {
                    // 38.291 6.1.2 and 6.1.3: CRC, repetition, coding, then the preamble and one
                    // midamble per I_bit bits, rounded up with the additional midamble
                    const int e = (a + (a > 24 ? 16 : 6)) * r_block * (r_code ? 3 : 1);
                    const int mid = i_add ? (e + p.i_bit - 1) / p.i_bit : e / p.i_bit;
                    EXPECT_EQ(aiot_frame_bits(a, &p), e + (1 + mid) * l_amble);
                    EXPECT_EQ(aiot_frame_samples(a, &p), (e + (1 + mid) * l_amble) * bit);
                  }
                  EXPECT_EQ(aiot_t_d2r_min(&p), std::max(3 * bit / (2 * r_sfs), AIOT_SAMPLE_RATE_HZ / 100000));
                  checked++;
                }
  EXPECT_GT(checked, 1000);
}

TEST(Params, ResponseTimes)
{
  // 38.291 7.1.2: T_R->D = T_offset (Table 7.1.2-3) plus the decoding time of Tables 7.1.2-1 and 7.1.2-2
  const aiot_d2r_params_t long_chip = {2, 1, 1, 1, 1, 31, 48, 0, AIOT_BPSK}; // T_chip = tau
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_DATA, 12, NULL, 0, 0, 0), (10 + 2) * AIOT_TAU);
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_DATA, 40, NULL, 0, 0, 0), (10 + 4) * AIOT_TAU);
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_DATA, 100, NULL, 0, 0, 0), (10 + 8) * AIOT_TAU);
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_MSG2, 12, NULL, 0, 0, 0), 10 * AIOT_TAU);
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_MSG1, 2, NULL, 0, 0, 0), 10 * AIOT_TAU);
  // Msg1 in an earlier access occasion: 5/4 (T_offset + the D2R's chips)
  EXPECT_EQ(aiot_t_r_to_d(&long_chip, 6, AIOT_MSG1, 2, NULL, 0, 1, 100), 5 * (10 + 100) * AIOT_TAU / 4);
  aiot_d2r_params_t uncoded = long_chip;
  uncoded.r_code = 0;
  EXPECT_EQ(aiot_t_r_to_d(&uncoded, 6, AIOT_DATA, 100, NULL, 0, 0, 0), 10 * AIOT_TAU);

  const aiot_d2r_params_t chip_32 = {1, 16, 1, 1, 1, 31, 1536, 0, AIOT_BPSK}; // T_chip = tau / 32
  EXPECT_EQ(aiot_t_r_to_d(&chip_32, 6, AIOT_MSG2, 12, NULL, 0, 0, 0), (1 + 1) * AIOT_TAU);
  const aiot_d2r_params_t short_chip = {1, 32, 2, 1, 0, 31, 3072, 0, AIOT_BPSK}; // T_chip = tau / 128
  EXPECT_EQ(aiot_t_r_to_d(&short_chip, 2, AIOT_DATA, 12, NULL, 0, 0, 0), AIOT_TAU);
  EXPECT_EQ(aiot_t_r_to_d(&short_chip, 6, AIOT_DATA, 12, NULL, 0, 0, 0), AIOT_TAU / 4);
  // Msg2 with several candidate shifts: T_chip' is the longest chip among them
  const int candidates[2] = {2, 1};
  EXPECT_EQ(aiot_t_r_to_d(&short_chip, 6, AIOT_MSG2, 12, candidates, 2, 0, 0), AIOT_TAU / 4);
  EXPECT_EQ(aiot_t_r_to_d(&chip_32, 6, AIOT_MSG2, 12, candidates, 2, 0, 0), (1 + 1) * AIOT_TAU);

  // 38.191 9.2.1: Te = ceil(T_SFO / 10)
  EXPECT_EQ(aiot_timing_tolerance(100), 10);
  EXPECT_EQ(aiot_timing_tolerance(101), 11);
}

TEST(Params, RejectsWhatTheSpecDoesNotAllow)
{
  aiot_d2r_params_t ok = {1, 8, 4, 1, 1, 31, 768, 0, AIOT_BPSK};
  ASSERT_EQ(aiot_d2r_params_check(&ok), 0);
  aiot_d2r_params_t p = ok;
  p.r_sfs = 16; // larger than 1/8 tau allows
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
  p = ok, p.r_sfs = 3;
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
  p = ok, p.i_bit = 48; // not scaled by S = 16
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
  p = ok, p.l_amble = 15;
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
  p = ok, p.r_block = 3;
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
  p = ok, p.t_bit_num = 3, p.t_bit_den = 8;
  EXPECT_EQ(aiot_d2r_params_check(&p), AIOT_EINVAL);
}

TEST(Coding, CrcIsTheSpecCrc)
{
  std::mt19937 gen(1);
  for (int a = 1; a <= 1000; a += (a < 50 ? 1 : 37)) {
    std::vector<uint8_t> bits = RandomBits(gen, a);
    const int L = a > 24 ? 16 : 6;
    EXPECT_EQ(aiot_crc_length(a), L);
    std::vector<uint8_t> parity(L);
    ASSERT_EQ(aiot_crc(bits.data(), a, L, parity.data()), L);
    EXPECT_EQ(parity, ReferenceCrc(bits, L, L == 16 ? kCrc16 : kCrc6)) << "A = " << a;

    std::vector<uint8_t> word(a + L);
    ASSERT_EQ(aiot_attach_crc(bits.data(), a, word.data()), a + L);
    int a_back = -1;
    const int b = a + L;
    if (b >= 31 && b <= 40) {
      // the receiver cannot tell L from B there
      EXPECT_EQ(aiot_check_crc(word.data(), b, &a_back), AIOT_EINVAL);
      continue;
    }
    EXPECT_EQ(aiot_check_crc(word.data(), b, &a_back), 1);
    EXPECT_EQ(a_back, a);
    for (int flip : {0, b / 2, b - 1}) {
      word[flip] ^= 1;
      EXPECT_EQ(aiot_check_crc(word.data(), b, &a_back), 0) << "A = " << a << ", bit " << flip;
      word[flip] ^= 1;
    }
  }
  uint8_t p[16];
  EXPECT_EQ(aiot_crc(p, 0, 8, p), AIOT_EINVAL);
}

TEST(Coding, TbccIsTheSpecCode)
{
  std::mt19937 gen(2);
  for (int k = 1; k <= 300; k += (k < 20 ? 1 : 23)) {
    std::vector<uint8_t> c = RandomBits(gen, k);
    std::vector<uint8_t> e(3 * k);
    aiot_tbcc_encode(c.data(), k, e.data());
    EXPECT_EQ(e, ReferenceTbcc(c)) << "K = " << k;
  }
}

TEST(Coding, TbccDecodesThroughErrors)
{
  std::mt19937 gen(3);
  for (int k = 1; k <= 1000; k += (k < 20 ? 1 : 61)) {
    std::vector<uint8_t> c = RandomBits(gen, k);
    std::vector<uint8_t> e(3 * k);
    aiot_tbcc_encode(c.data(), k, e.data());
    std::vector<double> soft = Soft(e);
    std::vector<uint8_t> out(k);
    ASSERT_EQ(aiot_tbcc_decode(soft.data(), 3 * k, out.data()), k);
    EXPECT_EQ(out, c) << "K = " << k;
    if (k < 40)
      continue;
    // one wrong coded bit in every 40, and every other value weak: well inside what the code corrects
    for (int i = 0; i < 3 * k; i++)
      soft[i] *= i % 40 == 7 ? -1.0 : (i % 2 ? 0.4 : 1.0);
    ASSERT_EQ(aiot_tbcc_decode(soft.data(), 3 * k, out.data()), k);
    EXPECT_EQ(out, c) << "K = " << k << " with errors";
  }
  double soft[4] = {1, 1, 1, 1};
  uint8_t out[2];
  EXPECT_EQ(aiot_tbcc_decode(soft, 4, out), AIOT_EINVAL);
}

TEST(Coding, Repetition)
{
  const uint8_t bits[5] = {1, 0, 0, 1, 1};
  uint8_t out[10];
  ASSERT_EQ(aiot_repeat_block(bits, 5, 2, out), 10);
  for (int i = 0; i < 10; i++)
    EXPECT_EQ(out[i], bits[i % 5]);
  const double soft[6] = {1, -2, 3, 0.5, 0.5, -1};
  double sum[3];
  ASSERT_EQ(aiot_combine_repetitions(soft, 6, 2, sum), 3);
  EXPECT_DOUBLE_EQ(sum[0], 1.5);
  EXPECT_DOUBLE_EQ(sum[1], -1.5);
  EXPECT_DOUBLE_EQ(sum[2], 2.0);
  EXPECT_EQ(aiot_combine_repetitions(soft, 5, 2, sum), AIOT_EINVAL);
  EXPECT_EQ(aiot_repeat_block(bits, 5, 0, out), AIOT_EINVAL);
}
