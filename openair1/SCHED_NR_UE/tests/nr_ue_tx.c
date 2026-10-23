/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* A UE's TX procedure for the tests: see nr_ue_tx.h. */

#include "nr_ue_tx.h"

#include <stdlib.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/phy_extern_nr_ue.h"
#include "PHY/INIT/nr_phy_init.h"
#include "SCHED_NR_UE/defs.h"
#include "SCHED_NR_UE/fapi_nr_ue_l1.h"
#include "openair2/NR_UE_PHY_INTERFACE/NR_IF_Module.h"
#include "openair2/LAYER2/NR_MAC_UE/mac_proto.h"
#include "common/utils/assertions.h"
#include "executables/nr-uesoftmodem.h"
#include "executables/softmodem-common.h"
#include "executables/thread-common.h"
#include "radio/COMMON/common_lib.h"

/* What the UE's PHY libraries take from the softmodem and its MAC, which the test does not link (as in
 * the UE's physical simulators). The test calls none of these functions. */
PHY_VARS_NR_UE ***nrPHY_vars_UE_g;
static nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
openair0_config_t openair0_cfg_g[MAX_CARDS];
THREAD_STRUCT thread_struct;
uint64_t downlink_frequency[MAX_NUM_CCs][4];
int64_t uplink_frequency_offset[MAX_NUM_CCs][4];
fapi_nr_ul_config_request_pdu_t *fapiLockIterator(fapi_nr_ul_config_request_t *ul_config, frame_t frame_tx, int slot_tx)
{
  abort();
}
void release_ul_config(fapi_nr_ul_config_request_pdu_t *pdu, bool clearIt)
{
  abort();
}
void update_harq_status(NR_UE_MAC_INST_t *mac, uint8_t harq_pid, int cw_idx, uint8_t ack_nack)
{
  abort();
}

#define NR_BAND 28
#define DL_POINT_A_KHZ 758000
#define UL_POINT_A_KHZ 703000
#define MAX_TDD_PERIOD 20
#define OFDM_OFFSET_DIVISOR 8 /* the UE's default, --offset-divisor */

struct nr_ue_tx {
  PHY_VARS_NR_UE ue;
  PHY_VARS_NR_UE *cc[1]; /* nrPHY_vars_UE_g[0], for the configuration request */
  PHY_VARS_NR_UE **inst[1];
  nr_phy_config_t config;
  fapi_nr_max_tdd_periodicity_t tdd[MAX_TDD_PERIOD];
};

struct nr_ue_tx_slot {
  UE_nr_rxtx_proc_t proc;
  nr_phy_data_tx_t phy_data;
};

nr_ue_tx_t *nr_ue_tx_new(const nr_aiot_r2d_config_t *r2d)
{
  nr_ue_tx_t *t = calloc_or_fail(1, sizeof(*t));
  PHY_VARS_NR_UE *ue = &t->ue;
  t->cc[0] = ue;
  t->inst[0] = t->cc;
  nrPHY_vars_UE_g = t->inst;

  fapi_nr_config_request_t *cfg = &t->config.config_req;
  cfg->ssb_config.scs_common = 0;
  cfg->carrier_config.dl_grid_size[0] = NR_UE_TX_N_RB;
  cfg->carrier_config.ul_grid_size[0] = NR_UE_TX_N_RB;
  cfg->carrier_config.dl_frequency = DL_POINT_A_KHZ;
  cfg->carrier_config.uplink_frequency = UL_POINT_A_KHZ;
  cfg->cell_config.frame_duplex_type = FDD;
  ue->frame_parms.threequarter_fs = 1;
  ue->frame_parms.ofdm_offset_divisor = OFDM_OFFSET_DIVISOR;
  nr_init_frame_parms_ue(&ue->frame_parms, cfg, NR_BAND);
  ue->aiot_r2d_cfg = *r2d;
  init_nr_ue_signal(ue, 1);
  nr_ue_phy_config_request(&t->config);
  return t;
}

void nr_ue_tx_free(nr_ue_tx_t *t)
{
  term_nr_ue_signal(&t->ue);
  nrPHY_vars_UE_g = NULL;
  free(t);
}

nr_aiot_r2d_t *nr_ue_tx_r2d(nr_ue_tx_t *t)
{
  return &t->ue.aiot_r2d;
}

int nr_ue_tx_amp(void)
{
  return AMP;
}

void nr_ue_tx_set_tdd(nr_ue_tx_t *t, int period, const int *num_ul)
{
  AssertFatal(period > 0 && period <= MAX_TDD_PERIOD, "TDD period %d\n", period);
  fapi_nr_config_request_t *cfg = &t->config.config_req;
  cfg->cell_config.frame_duplex_type = TDD;
  cfg->tdd_table.tdd_period_in_slots = period;
  cfg->tdd_table.max_tdd_periodicity_list = t->tdd;
  for (int i = 0; i < period; i++)
    t->tdd[i].num_dl_ul_symbols_list = (fapi_nr_num_dl_ul_symbols_t){NR_SYMBOLS_PER_SLOT - num_ul[i], num_ul[i]};
  nr_ue_phy_config_request(&t->config);
}

nr_ue_tx_slot_t *nr_ue_tx_take(nr_ue_tx_t *t, int frame, int slot)
{
  nr_ue_tx_slot_t *s = calloc_or_fail(1, sizeof(*s));
  s->proc = (UE_nr_rxtx_proc_t){
      .frame_tx = frame,
      .nr_slot_tx = slot,
      .timestamp_tx = get_samples_slot_timestamp(&t->ue.frame_parms, slot) + (int64_t)frame * t->ue.frame_parms.samples_per_frame};
  nr_ue_aiot_r2d_take(&t->ue, &s->proc, &s->phy_data);
  return s;
}

int nr_ue_tx_send(nr_ue_tx_t *t, nr_ue_tx_slot_t *s, c16_t *out)
{
  c16_t *txp[1] = {out};
  phy_procedures_nrUE_TX(&t->ue, &s->proc, &s->phy_data, txp);
  const int n = get_samples_per_slot(s->proc.nr_slot_tx, &t->ue.frame_parms);
  free(s);
  return n;
}
