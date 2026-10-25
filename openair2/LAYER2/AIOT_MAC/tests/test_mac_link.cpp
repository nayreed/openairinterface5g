/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* The link settings and the Scheduling Info built from them. The expected values are those of
 * Amber's LinkConfig and d2r_start_offset for the same settings. */

#include <gtest/gtest.h>
#include <vector>

#include "aiot_mac.h"

namespace {

aiot_link_config_t link(int num, int den, std::vector<int> r_sfs, int x, int interval_cp, int i_add)
{
  aiot_link_config_t c = {};
  c.m_chip_symb = 6;
  c.t_bit_num = num;
  c.t_bit_den = den;
  c.n_r_sfs = r_sfs.size();
  for (size_t i = 0; i < r_sfs.size(); i++)
    c.r_sfs[i] = r_sfs[i];
  c.x = x;
  c.r_code = 1;
  c.r_block = 1;
  c.l_amble = 31;
  c.interval_cp = interval_cp;
  c.i_add = i_add;
  c.d2r_tbs = 5;
  return c;
}

std::vector<int> sfs(const aiot_sched_info_t &s)
{
  return std::vector<int>(s.r_sfs, s.r_sfs + s.n_sfs);
}

int offset(const aiot_sched_info_t &s, aiot_d2r_msg_t kind, int tbs, bool candidates, int slot)
{
  aiot_d2r_params_t p;
  EXPECT_EQ(aiot_mac_to_d2r_params(&s, s.r_sfs[0], AIOT_BPSK, &p), 0);
  const int chips = aiot_frame_bits(8 * tbs, &p) * 2 * p.r_sfs;
  return aiot_t_r_to_d(&p, 6, kind, tbs, candidates ? s.r_sfs : nullptr, candidates ? s.n_sfs : 0, slot > 0, chips);
}

} // namespace

TEST(Link, EighthTauFourShifts)
{
  const aiot_link_config_t c = link(1, 8, {8, 1, 4, 2}, 1, 0, 0);
  ASSERT_EQ(aiot_link_config_check(&c), 0);
  EXPECT_EQ(aiot_link_max_entries(&c), 4);
  EXPECT_EQ(aiot_link_n_rb(&c), 1);

  aiot_sched_info_t s, r, f, u;
  ASSERT_EQ(aiot_link_cbra(&c, nullptr, 0, 0, &s), 0);
  EXPECT_EQ(sfs(s), (std::vector<int>{1, 2, 4, 8}));
  EXPECT_EQ(s.x, 1);
  EXPECT_EQ(s.i_bit, 768);
  ASSERT_EQ(aiot_link_ridr(&c, 3, 0, &r), 0);
  EXPECT_EQ(sfs(r), (std::vector<int>{1, 2, 4}));
  EXPECT_EQ(r.d2r_tbs, 5);
  ASSERT_EQ(aiot_link_cfa(&c, 0, 0, &f), 0);
  EXPECT_EQ(sfs(f), (std::vector<int>{1}));
  EXPECT_EQ(f.d2r_tbs, 5);
  ASSERT_EQ(aiot_link_unicast(&c, 8, 9, &u), 0);
  EXPECT_EQ(sfs(u), (std::vector<int>{8}));
  EXPECT_EQ(u.d2r_tbs, 9);

  const int off = offset(s, AIOT_MSG1, 2, true, 0);
  EXPECT_EQ(off, 3840);
  EXPECT_EQ(aiot_timing_tolerance(off), 384);
  EXPECT_EQ(offset(r, AIOT_MSG2, 5, true, 0), 3840);
  EXPECT_EQ(offset(f, AIOT_CFA, 5, false, 0), 3840);
  EXPECT_EQ(offset(f, AIOT_DATA, 5, false, 0), 5376);
}

TEST(Link, HalfTauTwoSlots)
{
  const aiot_link_config_t c = link(1, 2, {2, 1}, 2, 2, 1);
  ASSERT_EQ(aiot_link_config_check(&c), 0);
  EXPECT_EQ(aiot_link_max_entries(&c), 6);
  aiot_sched_info_t s, r;
  ASSERT_EQ(aiot_link_cbra(&c, nullptr, 0, 0, &s), 0);
  EXPECT_EQ(sfs(s), (std::vector<int>{1, 2}));
  EXPECT_EQ(s.x, 2);
  EXPECT_EQ(s.i_bit, 672);
  EXPECT_EQ(offset(s, AIOT_MSG1, 2, true, 0), 3840);
  EXPECT_EQ(offset(s, AIOT_MSG1, 2, true, 1), 66240);
  ASSERT_EQ(aiot_link_ridr(&c, 3, 0, &r), 0);
  EXPECT_EQ(sfs(r), (std::vector<int>{1, 2, 4}));
  EXPECT_EQ(offset(r, AIOT_MSG2, 5, true, 0), 3840);
}

TEST(Link, TwoTau)
{
  const aiot_link_config_t c = link(2, 1, {1}, 1, 3, 0);
  ASSERT_EQ(aiot_link_config_check(&c), 0);
  EXPECT_EQ(aiot_link_max_entries(&c), 8);
  aiot_sched_info_t s, f;
  ASSERT_EQ(aiot_link_cbra(&c, nullptr, 0, 0, &s), 0);
  EXPECT_EQ(s.i_bit, 240);
  const int off = offset(s, AIOT_MSG1, 2, true, 0);
  EXPECT_EQ(off, 7680);
  EXPECT_EQ(aiot_timing_tolerance(off), 768);
  ASSERT_EQ(aiot_link_cfa(&c, 0, 0, &f), 0);
  EXPECT_EQ(offset(f, AIOT_CFA, 5, false, 0), 7680);
  EXPECT_EQ(offset(f, AIOT_DATA, 5, false, 0), 9216);
}

TEST(Link, RejectsBadSettings)
{
  aiot_link_config_t c = link(1, 8, {1, 2}, 1, 0, 0);
  ASSERT_EQ(aiot_link_config_check(&c), 0);
  aiot_link_config_t bad = c;
  bad.m_chip_symb = 5;
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  bad = c;
  bad.interval_cp = 4;
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  bad = c;
  bad.d2r_tbs = 0;
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  bad = c;
  bad.d2r_tbs = AIOT_MAC_MAX_TBS + 1;
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  bad = c;
  bad.r_sfs[1] = 1; /* the same value twice */
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  bad = link(1, 8, {1, 2, 1}, 1, 0, 0); /* twice, not side by side */
  EXPECT_LT(aiot_link_config_check(&bad), 0);
  for (int n : {-1, AIOT_MAX_SHIFTS + 1}) {
    bad = c;
    bad.n_r_sfs = n;
    EXPECT_LT(aiot_link_config_check(&bad), 0) << n;
  }
  bad = c;
  bad.x = 3;
  EXPECT_LT(aiot_link_config_check(&bad), 0);

  aiot_sched_info_t s;
  EXPECT_LT(aiot_link_ridr(&c, 0, 0, &s), 0);
  EXPECT_LT(aiot_link_ridr(&c, aiot_link_max_entries(&c) + 1, 0, &s), 0);
}

TEST(Link, PagingIdIsTheNodeIdMsbFirst)
{
  uint8_t bits[16];
  aiot_paging_id_of_node(0x8001, bits);
  EXPECT_EQ(bits[0], 1);
  for (int i = 1; i < 15; i++)
    EXPECT_EQ(bits[i], 0);
  EXPECT_EQ(bits[15], 1);
}
