/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file amber_host.h
 * \brief Common parts of amber-reader and amber-tag: their configuration, the rfsim device that
 * carries one Amber message per block, and the embedded Python session that runs Amber.
 */

#ifndef AMBER_HOST_H
#define AMBER_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include "common/platform_types.h"
#include "common_lib.h"

/*! \brief bytes in one rfsim block (one simulated millisecond) unless configured otherwise */
#define AMBER_DEFAULT_BLOCK_BYTES 8192
/*! \brief Tags amber-reader can hear at once: as many clients as an rfsim server accepts */
#define AMBER_MAX_TAGS 250

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
  void **tag_blocks; ///< amber-reader only: AMBER_MAX_TAGS blocks, one per connected Tag
} amber_radio_t;

typedef struct amber_session_s amber_session_t;

/*! \brief Read the "amber" configuration section; with_node_id also requires amber.node_id */
void amber_read_params(amber_params_t *params, bool with_node_id);

/*! \brief Load the rfsimulator device for one antenna, sized so one block is one millisecond,
 * and start it as server or client, as rfsimulator.serveraddr says. The Reader must be an rfsim
 * server in msgmode, so it can read each Tag's block separately. */
void amber_radio_start(amber_radio_t *radio, int block_bytes, bool reader);

/*! \brief Read nsamps samples into radio->block; returns the timestamp of the first one */
openair0_timestamp_t amber_radio_read(amber_radio_t *radio, int nsamps);

/*! \brief Reader: read the next block of every connected Tag into radio->tag_blocks, each as
 * that Tag wrote it; returns the number of Tags and sets *ts to the timestamp of the blocks */
int amber_radio_read_tags(amber_radio_t *radio, openair0_timestamp_t *ts);

/*! \brief Write radio->block as one block starting at timestamp ts */
void amber_radio_write(amber_radio_t *radio, openair0_timestamp_t ts, int flags);

void amber_radio_stop(amber_radio_t *radio);

/*! \brief Start Python and create the Reader's session (amber.session.ReaderSession) */
amber_session_t *amber_session_create_reader(const amber_params_t *params);

/*! \brief Start Python and create a Tag's session (amber.session.TagSession) */
amber_session_t *amber_session_create_tag(const amber_params_t *params);

/*! \brief Reader: the block to send to the Tags for the next millisecond */
void amber_session_next_block(amber_session_t *session, uint8_t *out);

/*! \brief Reader: hand over the n blocks received from the Tags, in any order; returns true once
 * the run is over */
bool amber_session_on_blocks(amber_session_t *session, void *const *blocks, int n);

/*! \brief Tag: hand over the block received from the Reader and get the block to answer with;
 * returns true once the run is over (the answer must still be sent) */
bool amber_session_reply(amber_session_t *session, const uint8_t *in, uint8_t *out);

/*! \brief Write the session's logs, release it and stop Python */
void amber_session_end(amber_session_t *session);

#endif
