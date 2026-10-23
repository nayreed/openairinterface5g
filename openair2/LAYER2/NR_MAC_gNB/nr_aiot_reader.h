/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_reader.h
 * \brief The gNB as an Ambient IoT Reader: the 38.391 Reader MAC (openair2/LAYER2/AIOT_MAC) and its
 * protocol, driving the gNB's R2D transmissions (nr_aiot_r2d) and D2R reception (nr_aiot_d2r).
 *
 * It does what Amber's Reader does over its own air, on the gNB's: it takes the R2D the protocol
 * sends next, puts it on the DL grid from the first half-subframe that the gNB has not sent yet,
 * opens the D2R windows the MAC asks for, and gives the MAC each window's outcome. Times are A-IoT
 * samples (5.76 Msps) of the radio's clock: slot s of the radio's timeline (s = frame * 10 + slot,
 * frames counted on past 1023) starts at sample 5760 s, on the DL and on the UL.
 *
 * It runs in its own thread, woken by each slot the D2R receiver takes; building an R2D's resource
 * elements happens there, not in the gNB's slot processing. What it sent and heard goes to
 * reader_tx.csv and reader_rx.csv in an output folder, with the columns of Amber's Reader, and each
 * Tag's data is logged.
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
  char protocol[32]; /* "": the gNB is no Reader */
  char param_names[NR_AIOT_READER_MAX_PARAMS][32];
  aiot_param_t params[NR_AIOT_READER_MAX_PARAMS]; /* names point into param_names */
  int n_params;
  int node_ids[NR_AIOT_READER_MAX_NODES];
  int n_nodes;
  aiot_link_config_t link;
  int lead_slots; /* slots between the slot the gNB sent last and an R2D's first; doubles when late */
  char output_dir[256]; /* "": no CSV files */
} nr_aiot_reader_config_t;

typedef struct nr_aiot_reader nr_aiot_reader_t;

typedef struct {
  uint64_t r2d_sent; /* R2D transmissions put on the grid */
  uint64_t too_late; /* R2Ds that had to wait for a later slot */
  uint64_t answers; /* D2R messages heard */
  uint64_t collisions;
} nr_aiot_reader_stats_t;

/* Parses "name=value,name=value" into cfg->params. Returns 0 or -1. */
int nr_aiot_reader_parse_params(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses "0,1,2" (and ranges "0-7") into cfg->node_ids. Returns 0 or -1. */
int nr_aiot_reader_parse_nodes(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses a list of R_SFS values "1,2,4,8" into cfg->link. Returns 0 or -1. */
int nr_aiot_reader_parse_r_sfs(nr_aiot_reader_config_t *cfg, const char *s);
/* Parses a bit duration in tau, "2", "1", "1/2" ... "1/96", into cfg->link. Returns 0 or -1. */
int nr_aiot_reader_parse_t_bit(nr_aiot_reader_config_t *cfg, const char *s);

/* A Reader on the gNB's R2D transmitter and D2R receiver. Returns NULL with the reason in err. */
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
/* Starts the thread. Returns 0 or -1. */
int nr_aiot_reader_start(nr_aiot_reader_t *r);
/* 1 once the protocol has ended and every answer is in. */
int nr_aiot_reader_done(nr_aiot_reader_t *r);
nr_aiot_reader_stats_t nr_aiot_reader_stats(nr_aiot_reader_t *r);

/* The Reader of the gNB's "aiot" configuration section (nr_aiot_reader_config.c), started; NULL
 * when aiot.protocol is empty. Stops the gNB with the reason when the section does not fit. */
nr_aiot_reader_t *nr_aiot_reader_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r);

#ifdef __cplusplus
}
#endif

#endif
