/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file amber_host.h
 * \brief Common parts of amber-reader and amber-tag: their configuration, the rfsim device that
 * carries one simulated millisecond of signal per block, and the Amber session, which runs in
 * embedded Python (Amber's MAC is Python; its PHY is libaiot_phy.so, openair1/PHY/AIOT).
 */

#ifndef AMBER_HOST_H
#define AMBER_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include "common/platform_types.h"
#include "common_lib.h"

/*! \brief bytes in one rfsim block (one simulated millisecond): 5888 c16 samples, the 5760 of one
 * millisecond at 5.76 Msps and 128 that carry control between the Reader and the Tags. Through
 * rfsim_hub the blocks are air only, 5760 samples (amber.block_bytes 23040); see README.md. */
#define AMBER_DEFAULT_BLOCK_BYTES 23552

typedef struct {
  char *scenario; ///< path of the Amber scenario module
  char *output_dir; ///< directory the session writes its CSV logs to
  int block_bytes; ///< size of one message block, a multiple of sizeof(c16_t)
  int node_id; ///< node this program runs (amber-tag only)
} amber_params_t;

typedef struct {
  openair0_device_t device;
  openair0_config_t config;
  int block_samples;
  c16_t *block; ///< one block of samples; the session reads and writes it as bytes
} amber_radio_t;

typedef struct amber_session_s amber_session_t;

/*! \brief Read the "amber" configuration section; with_node_id also requires amber.node_id */
void amber_read_params(amber_params_t *params, bool with_node_id);

/*! \brief Load the rfsimulator device for one antenna, sized so one block is one millisecond,
 * and start it as server or client, as rfsimulator.serveraddr says. On plain rfsim the Reader is
 * the server (--rfsimulator.serveraddr server) and each Tag a client; through rfsim_hub, with
 * air-only blocks, all of them are clients of the hub. */
void amber_radio_start(amber_radio_t *radio, int block_bytes);

/*! \brief Read nsamps samples into radio->block, from sample offset of the block on; returns
 * the timestamp of the first one */
openair0_timestamp_t amber_radio_read(amber_radio_t *radio, int offset, int nsamps);

/*! \brief Write radio->block as one block starting at timestamp ts */
void amber_radio_write(amber_radio_t *radio, openair0_timestamp_t ts, int flags);

void amber_radio_stop(amber_radio_t *radio);

/*! \brief Start Python and create the Reader's session (amber.session.ReaderSession) */
amber_session_t *amber_session_create_reader(const amber_params_t *params);

/*! \brief Start Python and create a Tag's session (amber.session.TagSession) */
amber_session_t *amber_session_create_tag(const amber_params_t *params);

/*! \brief Reader: the block to send to the Tags for the next millisecond */
void amber_session_next_block(amber_session_t *session, uint8_t *out);

/*! \brief Reader: hand over the block received from the Tags, the sum of their signals; returns
 * true once the run is over */
bool amber_session_on_block(amber_session_t *session, const uint8_t *block);

/*! \brief Tag: hand over the block received from the Reader and get the block to answer with;
 * returns true once the run is over (the answer must still be sent) */
bool amber_session_reply(amber_session_t *session, const uint8_t *in, uint8_t *out);

/*! \brief Write the session's logs, release it and stop Python */
void amber_session_end(amber_session_t *session);

#endif
