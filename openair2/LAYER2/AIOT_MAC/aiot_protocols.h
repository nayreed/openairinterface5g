/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_protocols.h
 * \brief Inside the A-IoT MAC library: how the Reader runs a protocol.
 *
 * A protocol is a Reader policy over 38.391 messages. Each time the Reader needs its next R2D it
 * calls the protocol's step() with what the last one brought back, and step() writes the next
 * message. A message marked access is a CBRA paging or an Access Trigger whose Msg1s the Reader
 * answers itself with one Random ID Response, which brings each answered device's data; step()
 * then gets both: h1, what the paging or trigger brought, and h2, what the Random ID Response
 * brought (NULL when no Msg1 was decoded). */

#ifndef AIOT_PROTOCOLS_H
#define AIOT_PROTOCOLS_H

#include "aiot_mac.h"

#define AIOT_PROTOCOL_MAX_PARAMS 8
#define AIOT_PROTOCOL_STATE_BYTES 8192
#define AIOT_PROTOCOL_WAIT 2

typedef struct {
  aiot_mac_r2d_msg_t msg;
  int access;
} aiot_policy_msg_t;

/* What a protocol sees of the Reader (ReaderContext). */
typedef struct {
  const aiot_link_config_t *cfg;
  const int *node_ids;
  int n_nodes;
  double now_ms; /* the Reader's clock: samples / 5760 */
  int *tid; /* the last Transaction ID given */
  /* the protocol's own table (q.csv, adaptation.csv) */
  void (*add_row)(void *reader, const double *row);
  void *reader;
} aiot_policy_ctx_t;

typedef struct {
  const char *name;
  int n_params;
  const char *param_names[AIOT_PROTOCOL_MAX_PARAMS];
  double param_defaults[AIOT_PROTOCOL_MAX_PARAMS];
  int param_is_int[AIOT_PROTOCOL_MAX_PARAMS];
  /* the protocol's table, if it keeps one */
  const char *table;
  int n_cols;
  const char *columns[8];
  /* checks the parameters beyond their types: 0, or AIOT_EINVAL with why in err */
  int (*check)(const double *p, char *err, int err_len);
  /* the next message, given what the last brought (h1 and h2 NULL at the start): 1, 0 when the
   * protocol has ended, or AIOT_PROTOCOL_WAIT when it sends only what the caller pushes
   * (aiot_reader_push). state is AIOT_PROTOCOL_STATE_BYTES of zeros at the start. */
  int (*step)(void *state,
              const double *p,
              aiot_policy_ctx_t *ctx,
              const aiot_reader_heard_t *h1,
              const aiot_reader_heard_t *h2,
              aiot_policy_msg_t *next);
} aiot_protocol_t;

const aiot_protocol_t *aiot_protocol_find(const char *name);

#endif
