/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file amber-reader.c
 * \brief Amber Reader: the gNB reduced to its configuration, logging and rfsim device. It runs the
 * A-IoT Reader of an Amber scenario (its MAC and PHY), one simulated millisecond per rfsim block:
 * send the block for this millisecond, then read the Tags' answer to it, the sum of their signals
 * as rfsim gives it. With --amber.role carrier it runs only the scenario's carrier node instead: a
 * room whose carrier comes from a node of its own, through rfsim_hub (amber.iq.IqCarrierSession).
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
  amber_read_params(&params, false);
  amber_session_t *session = amber_session_create_reader(&params);
  amber_radio_t radio;
  amber_radio_start(&radio, params.block_bytes);
  config_check_unknown_cmdlineopt(uniqCfg, CONFIG_CHECKALLSECTIONS);

  uint8_t *block = (uint8_t *)radio.block;
  bool done = false;
  for (openair0_timestamp_t ts = 0; !done; ts += radio.block_samples) {
    amber_session_next_block(session, block);
    amber_radio_write(&radio, ts, TX_BURST_MIDDLE);
    openair0_timestamp_t rx_ts = amber_radio_read(&radio, 0, radio.block_samples);
    AssertFatal(rx_ts == ts, "read the block at %lu, expected %lu\n", rx_ts, ts);
    done = amber_session_on_block(session, block);
  }

  amber_session_end(session);
  amber_radio_stop(&radio);
  end_configmodule(uniqCfg);
  printf("Bye.\n");
  return 0;
}
