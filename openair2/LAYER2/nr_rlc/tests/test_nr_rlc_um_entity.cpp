/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "gtest/gtest.h"
extern "C" {
#include "common/platform_types.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_entity_um.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_entity.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;
}
#include <string>
#include <vector>

static std::vector<std::string> delivered;
static void deliver_sdu(void *deliver_sdu_data, nr_rlc_entity_t *entity, char *buf, int size)
{
  UNUSED(deliver_sdu_data);
  UNUSED(entity);
  delivered.emplace_back(buf, size);
}

static nr_rlc_entity_t *new_um(int sn_field_length)
{
  return new_nr_rlc_entity_um(10000, 10000, deliver_sdu, NULL, 35, sn_field_length);
}

/* Moves the TX entity's PDUs to the RX entity, offering 'size' bytes per call,
 * as the MAC does with what is left of a transport block. */
static void transfer(nr_rlc_entity_t *tx, nr_rlc_entity_t *rx, const std::vector<int> &sizes)
{
  char pdu[200];
  for (int size : sizes) {
    int n = tx->generate_pdu(tx, pdu, size);
    if (n > 0)
      rx->recv_pdu(rx, pdu, n);
  }
}

/* With a 12-bit SN, a whole SDU has a 1-byte header and its first segment a
 * 2-byte one. Offered 2 bytes, the entity cannot send a segment: it must keep
 * the SDU for the next call, not drop it. */
TEST(nr_rlc_um_entity, keeps_sdu_when_room_is_too_small_for_a_segment)
{
  nr_rlc_entity_t *tx = new_um(12);
  nr_rlc_entity_t *rx = new_um(12);
  delivered.clear();

  std::string a(30, 'a'), b(30, 'b');
  tx->recv_sdu(tx, (char *)a.data(), a.size(), 0);
  tx->recv_sdu(tx, (char *)b.data(), b.size(), 1);

  /* 'a' fits whole in 31 bytes; then 2 bytes, too few for a segment of 'b' */
  char pdu[200];
  EXPECT_EQ(tx->generate_pdu(tx, pdu, 31), 31);
  rx->recv_pdu(rx, pdu, 31);
  EXPECT_EQ(tx->generate_pdu(tx, pdu, 2), 0) << "no room for a segment: nothing to send";
  EXPECT_EQ(tx->buffer_status(tx, 1000).tx_size, 31) << "'b' still waits, with its 1-byte header";

  transfer(tx, rx, {10, 10, 10, 10, 10});
  ASSERT_EQ(delivered.size(), 2u);
  EXPECT_EQ(delivered[0], a);
  EXPECT_EQ(delivered[1], b);

  tx->delete_entity(tx);
  rx->delete_entity(rx);
}

/* Every SDU arrives, whatever sizes the MAC offers, with both SN lengths. */
TEST(nr_rlc_um_entity, delivers_every_sdu_for_any_offered_size)
{
  for (int sn : {6, 12}) {
    for (int step = 1; step <= 12; step++) {
      nr_rlc_entity_t *tx = new_um(sn);
      nr_rlc_entity_t *rx = new_um(sn);
      delivered.clear();
      std::vector<std::string> sent;
      for (int k = 0; k < 4; k++) {
        sent.emplace_back(40 + k, 'a' + k);
        tx->recv_sdu(tx, (char *)sent.back().data(), sent.back().size(), k);
      }
      std::vector<int> sizes;
      for (int i = 0; i < 400; i++)
        sizes.push_back(1 + (i * step) % 13);
      transfer(tx, rx, sizes);
      EXPECT_EQ(delivered, sent) << "SN " << sn << " bits, sizes stepping by " << step;
      EXPECT_EQ(tx->buffer_status(tx, 1000).tx_size, 0);
      tx->delete_entity(tx);
      rx->delete_entity(rx);
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
