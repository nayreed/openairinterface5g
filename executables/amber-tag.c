/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file amber-tag.c
 * \brief Amber Tag: the UE reduced to its configuration, logging and rfsim client. It runs one
 * A-IoT device of an Amber scenario (its energy harvesting, MAC and PHY), one simulated millisecond
 * per rfsim block: read the Reader's block, then answer in the same block.
 */

#include <stdio.h>
#include <stdlib.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/assertions.h"
#include "openair2/AMBER/amber_host.h"

configmodule_interface_t *uniqCfg = NULL;

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  LOG_E(UTIL, "called by %s:%d %s(): exiting: %s\n", file, line, function, s ? s : "no message");
  if (assert)
    abort();
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == NULL || CONFIG_ISFLAGSET(CONFIG_ABORT)) {
    fprintf(stderr, "configuration module init failed\n");
    exit(EXIT_FAILURE);
  }
  logInit();

  amber_params_t params;
  amber_read_params(&params, true);
  amber_session_t *session = amber_session_create_tag(&params);
  amber_radio_t radio;
  amber_radio_start(&radio, params.block_bytes);
  config_check_unknown_cmdlineopt(uniqCfg, CONFIG_CHECKALLSECTIONS);

  // An rfsim server starts a client one sample before its write position, which is at the end of
  // one of the Reader's blocks; rfsim_hub starts every node at sample 0. Read the first sample: if
  // it does not start a block, read on to the next block boundary, so that every read below is
  // exactly one of the Reader's blocks.
  const int block_samples = radio.block_samples;
  openair0_timestamp_t ts = amber_radio_read(&radio, 0, 1);
  int have = 1; // samples of the block at ts already in radio.block
  if (ts % block_samples != 0) {
    ts++;
    const int pad = (block_samples - ts % block_samples) % block_samples;
    if (pad > 0)
      ts = amber_radio_read(&radio, 0, pad) + pad;
    have = 0;
  }

  uint8_t *block = (uint8_t *)radio.block;
  int flags = TX_BURST_START;
  for (bool done = false; !done; ts += block_samples) {
    openair0_timestamp_t rx_ts = amber_radio_read(&radio, have, block_samples - have);
    AssertFatal(rx_ts == ts + have, "read the block at %lu, expected %lu\n", rx_ts - have, ts);
    have = 0;
    done = amber_session_reply(session, block, block);
    amber_radio_write(&radio, ts, flags);
    flags = TX_BURST_MIDDLE;
  }

  amber_session_end(session);
  amber_radio_stop(&radio);
  end_configmodule(uniqCfg);
  printf("Bye.\n");
  return 0;
}
