/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* A UE's TX procedure for the tests, behind a C interface: the UE's PHY headers are C only. */

#ifndef NR_UE_TX_H
#define NR_UE_TX_H

#include "PHY/NR_TRANSPORT/nr_aiot_r2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The room's UE: 25 PRBs at 15 kHz in a 384-point symbol (5.76 Msps, three-quarter sampling), FDD band n28 */
#define NR_UE_TX_N_RB 25

typedef struct nr_ue_tx nr_ue_tx_t;
typedef struct nr_ue_tx_slot nr_ue_tx_slot_t;

/* The UE, with its Ambient IoT R2D from r2d, and FDD from its configuration request. */
nr_ue_tx_t *nr_ue_tx_new(const nr_aiot_r2d_config_t *r2d);
void nr_ue_tx_free(nr_ue_tx_t *t);
nr_aiot_r2d_t *nr_ue_tx_r2d(nr_ue_tx_t *t);
/* A configuration request with TDD, period slots of num_ul[i] uplink symbols (the others downlink). */
void nr_ue_tx_set_tdd(nr_ue_tx_t *t, int period, const int *num_ul);
/* What processSlotTX() does for (frame, slot) before its barrier, in slot order. */
nr_ue_tx_slot_t *nr_ue_tx_take(nr_ue_tx_t *t, int frame, int slot);
/* What it does after the barrier, in any order: phy_procedures_nrUE_TX() for the slot, its samples in
 * out (room for 5760). Frees s. Returns the slot's number of samples. */
int nr_ue_tx_send(nr_ue_tx_t *t, nr_ue_tx_slot_t *s, c16_t *out);

#ifdef __cplusplus
}
#endif

#endif
