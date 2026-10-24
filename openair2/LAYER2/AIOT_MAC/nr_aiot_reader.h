/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_reader.h
 * \brief An NR node (a gNB or a UE) as an Ambient IoT Reader: the 38.391 Reader MAC (openair2/LAYER2/AIOT_MAC)
 * and its protocol, driving the node's R2D transmissions (nr_aiot_r2d) and D2R reception (nr_aiot_d2r).
 *
 * It does what Amber's Reader does over its own air, on the node's: it takes the R2D the protocol
 * sends next, puts it on the node's grid from the first half-subframe that the node has not sent yet,
 * opens the D2R windows the MAC asks for, and gives the MAC each window's outcome. Times are A-IoT
 * samples (5.76 Msps) of the D2R receiver: radio timestamps over D (nr_aiot_d2r_decimation()). The
 * node gives the radio timestamp of each slot it sends (nr_aiot_r2d_set_slot_time()), and the Reader
 * plans on those, so the node's TX slots need not start where its RX slots do (a UE's start a timing
 * advance earlier).
 *
 * It runs in its own thread, woken by each slot the D2R receiver takes; building an R2D's resource
 * elements happens there, not in the node's slot processing. What it sent and heard goes to
 * reader_tx.csv and reader_rx.csv in an output folder, with the columns of Amber's Reader, and each
 * Tag's data is logged. With a data destination, each message heard from a Tag also goes there, in
 * one UDP datagram of Tag data (aiot_node_msg.h): a UE that reads the Tags sends them to the network
 * over its own user plane (topology 2).
 *
 * A split Reader (topology 3) runs in a gNB whose R2D or D2R is a UE's (nr_aiot_peer.h), over the
 * gNB's user plane to it. With a remote R2D, the gNB sends the UE each R2D, for a half-subframe of the
 * cell, and plans on as if it had sent it; an R2D the UE gets too late raises the lead, as one of the
 * node's own would. With a remote D2R, the gNB sends the UE each window, at its time in the cell, and
 * gives the MAC each outcome the UE sends back; a window whose outcome does not come back is given up
 * as empty. The node's R2D clock (nr_aiot_r2d_set_slot_time()) runs also without R2D PRBs, and plans
 * every R2D and window. The Reader logs the delays of the split once a second of the cell.
 */

#ifndef NR_AIOT_READER_H
#define NR_AIOT_READER_H

#include <stdint.h>
#include "aiot_mac.h"
#include "PHY/NR_TRANSPORT/nr_aiot_r2d.h"
#include "PHY/NR_TRANSPORT/nr_aiot_d2r.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NR_AIOT_READER_MAX_PARAMS 8
#define NR_AIOT_READER_MAX_NODES 1024

typedef struct nr_aiot_reader_config {
  char protocol[32]; /* "": the node is no Reader */
  char param_names[NR_AIOT_READER_MAX_PARAMS][32];
  aiot_param_t params[NR_AIOT_READER_MAX_PARAMS]; /* names point into param_names */
  int n_params;
  int node_ids[NR_AIOT_READER_MAX_NODES];
  int n_nodes;
  aiot_link_config_t link;
  int lead_slots; /* slots between the slot the node sent last and an R2D's first; doubles when late */
  char output_dir[256]; /* "": no CSV files */
  char data_dest[64]; /* "ipv4:port" for each message heard from a Tag, as Tag data; "": none */
  char remote_r2d[64]; /* "ipv4:port" of the UE that sends the R2D; "": the node sends it */
  char remote_d2r[64]; /* "ipv4:port" of the UE that receives the D2R; "": the node receives it */
} nr_aiot_reader_config_t;

typedef struct nr_aiot_reader nr_aiot_reader_t;

typedef struct {
  uint64_t r2d_sent; /* R2D transmissions put on the grid */
  uint64_t too_late; /* R2Ds that had to wait for a later slot */
  uint64_t answers; /* D2R messages heard */
  uint64_t collisions;
  uint64_t data_sent; /* Tag data datagrams sent to the data destination */
  uint64_t data_unsent; /* and those the socket refused */
  /* a split Reader */
  uint64_t r2d_done; /* R2D done messages back from the remote R2D */
  uint64_t r2d_refused; /* R2Ds it could not send, other than too late */
  uint64_t r2d_early; /* of the R2D done, those of R2Ds sent before the remote R2D first answered */
  uint64_t r2d_early_late; /* and of those, the ones it got too late: too_late counts the others */
  /* the R2Ds sent after the remote R2D first answered: */
  int64_t r2d_ahead_min, r2d_ahead_sum; /* slots it had in hand when each R2D reached it */
  int64_t r2d_rtt_max, r2d_rtt_sum; /* A-IoT samples of the cell from each R2D sent to its R2D done */
  uint64_t windows_remote; /* windows sent to the remote D2R */
  uint64_t outcomes_remote; /* their outcomes back */
  uint64_t windows_lost; /* windows given up as empty, their outcome not back in time */
  uint64_t outcomes_early; /* of the outcomes, those of windows opened before the remote D2R first answered */
  uint64_t windows_early_late; /* and of those, the ones that reached it after their start */
  /* the windows opened after the remote D2R first answered: */
  uint64_t windows_late; /* those that reached it after their start */
  int64_t window_ahead_min, window_ahead_sum; /* A-IoT samples it had in hand when each window reached it */
  int64_t outcome_delay_max, outcome_delay_sum; /* A-IoT samples of the cell from each window's end to its outcome */
  uint64_t peer_unsent; /* messages to a remote end the socket did not take */
  uint64_t peer_refused; /* datagrams that were no answer the Reader waits for */
} nr_aiot_reader_stats_t;

/* Parses "name=value,name=value" into cfg->params. Returns 0 or -1. */
int nr_aiot_reader_parse_params(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses "0,1,2" (and ranges "0-7") into cfg->node_ids. Returns 0 or -1. */
int nr_aiot_reader_parse_nodes(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses a list of R_SFS values "1,2,4,8" into cfg->link. Returns 0 or -1. */
int nr_aiot_reader_parse_r_sfs(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses a bit duration in tau, "2", "1", "1/2" ... "1/96", into cfg->link. Returns 0 or -1. */
int nr_aiot_reader_parse_t_bit(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses a data destination "10.0.1.1:38391" (empty: none) into cfg->data_dest. Returns 0 or -1. */
int nr_aiot_reader_parse_data_dest(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses an address "10.0.1.2:38392" (empty: none) into dst, of 64 bytes, as cfg->remote_r2d and
 * cfg->remote_d2r take it. Returns 0 or -1. */
int nr_aiot_reader_parse_address(char *dst, const char *s);

/* A Reader on the node's R2D transmitter and D2R receiver. r2d must be initialised, with PRBs unless
 * the R2D is remote; d2r is NULL when, and only when, the D2R is remote. Returns NULL with the reason
 * in err. */
nr_aiot_reader_t *nr_aiot_reader_new(const nr_aiot_reader_config_t *cfg,
                                     nr_aiot_r2d_t *r2d,
                                     nr_aiot_d2r_t *d2r,
                                     char *err,
                                     int err_len);
/* Stops the thread, if any, writes what is left and frees everything. */
void nr_aiot_reader_free(nr_aiot_reader_t *r);

/* One pass: gives the MAC the outcomes that came in, and sends the R2Ds that are ready. Returns how
 * many of either it handled. The thread calls it once per slot received; tests call it. */
int nr_aiot_reader_step(nr_aiot_reader_t *r);
/* Starts the thread (threadCreate(), which stops the node if it cannot). Returns 0, or -1 when it runs already. */
int nr_aiot_reader_start(nr_aiot_reader_t *r);
/* 1 once the protocol has ended and every answer is in. */
int nr_aiot_reader_done(nr_aiot_reader_t *r);
nr_aiot_reader_stats_t nr_aiot_reader_stats(nr_aiot_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif
