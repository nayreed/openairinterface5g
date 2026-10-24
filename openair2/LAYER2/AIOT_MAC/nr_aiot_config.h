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

/*! \brief The R2D and the D2R of the section: their PRBs, power and receiver.
 * \param[out] r2d the R2D's; prb_start is -1 when the node has none
 * \param[out] d2r the D2R's; prb_start is -1 when the node has none
 * Stops the node with the reason (LOG_E, then exit) when they do not fit. */
void nr_aiot_config_phy(nr_aiot_r2d_config_t *r2d, nr_aiot_d2r_config_t *d2r);

/*! \brief A gNB's Reader of the section, started on its R2D and D2R.
 * \return the Reader, or NULL when aiot.protocol is empty. Stops the node with the reason when the
 * section does not fit. */
nr_aiot_reader_t *nr_aiot_reader_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r);

/*! \brief A UE's part of the section, on its R2D and D2R: its own Reader (aiot.protocol), or its end
 * of a gNB's split Reader (aiot.serve_port), started; the section may not ask for both.
 * \param[out] reader the Reader, or NULL
 * \param[out] peer the UE's end of the split, or NULL
 * Stops the node with the reason when the section does not fit. */
void nr_aiot_ue_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r, nr_aiot_reader_t **reader, nr_aiot_peer_t **peer);

#ifdef __cplusplus
}
#endif

#endif
