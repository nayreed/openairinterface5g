/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_config.h
 * \brief The "aiot" section of an NR node's configuration (and its --aiot.* options): where the
 * node's Ambient IoT R2D and D2R are, and its Reader. nr_aiot_config.c has every option.
 */

#ifndef NR_AIOT_CONFIG_H
#define NR_AIOT_CONFIG_H

#include "PHY/NR_TRANSPORT/nr_aiot_r2d.h"
#include "PHY/NR_TRANSPORT/nr_aiot_d2r.h"
#include "nr_aiot_reader.h"
#include "nr_aiot_peer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The R2D and the D2R of the section: their PRBs, power and receiver. prb_start is -1 for one the
 * node does not have. Stops the node with the reason when they do not fit. */
void nr_aiot_config_phy(nr_aiot_r2d_config_t *r2d, nr_aiot_d2r_config_t *d2r);

/* The Reader of the section, started on the node's R2D and D2R; NULL when aiot.protocol is empty.
 * Stops the node with the reason when the section does not fit. */
nr_aiot_reader_t *nr_aiot_reader_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r);

/* A UE's end of a split Reader, started on its R2D and D2R; NULL when aiot.serve_port is 0. Stops the
 * node with the reason when it cannot serve. */
nr_aiot_peer_t *nr_aiot_peer_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r);

#ifdef __cplusplus
}
#endif

#endif
