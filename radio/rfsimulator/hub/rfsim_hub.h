/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file rfsim_hub.h
 * \brief One shared air for several rfsim nodes.
 *
 * Every node (gNB, UE, Reader, Tag, ...) is a normal rfsim client of the hub, on its own port. Each
 * receiver gets the sum of the transmitters linked to it. A link has a gain, a delay, a frequency
 * shift and, if set, a pass band. Nodes without a link between them do not hear each other.
 *
 * The room (hub_room_*) holds what each node has sent and mixes what each node receives. It does
 * no I/O, so it can be tested on its own. hub_serve() connects it to the nodes' sockets.
 */

#ifndef RFSIM_HUB_H
#define RFSIM_HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/platform_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_NAME_LEN 32
/// A link's pass band is a linear-phase FIR of 2 HUB_PASS_HALF + 1 taps (Kaiser window, 60 dB). Its
/// edges fall from full gain to 60 dB down over about sample rate / 53, centred on each edge, and it
/// adds no delay: the receiver waits for HUB_PASS_HALF more samples of the transmitter instead.
#define HUB_PASS_HALF 96

typedef struct {
  char name[HUB_NAME_LEN];
  int port;
  /// samples the node writes ahead of the samples it has read
  int64_t lookahead;
  /// the node's first write is already lookahead samples ahead of its first read (OAI's gNB and UE):
  /// the hub counts samples [0, lookahead) of this node as silence
  bool lead;
  /// the node's receiver noise: Gaussian, this rms on I and on Q, in counts, added to what it
  /// receives; 0: none. It depends only on the node and the sample's time, so every run is the same.
  double noise_counts;
} hub_node_conf_t;

typedef struct {
  /// indexes into hub_conf_t.nodes
  int tx;
  int rx;
  double gain_dB;
  /// samples
  int64_t delay;
  /// the transmitter's signal lands this far from the receiver's centre frequency
  double shift_Hz;
  /// only the band pass_width_Hz wide around pass_Hz, measured like shift_Hz from the receiver's
  /// centre, reaches the receiver (see HUB_PASS_HALF); 0: the whole band
  double pass_Hz;
  double pass_width_Hz;
} hub_link_conf_t;

typedef struct {
  double sample_rate;
  /// the hub sends each node blocks of this many samples
  int64_t quantum;
  /// samples kept per transmitter, a power of two
  int64_t history;
  int num_nodes;
  hub_node_conf_t *nodes;
  int num_links;
  hub_link_conf_t *links;
} hub_conf_t;

typedef struct hub_room_s hub_room_t;

/*! \brief Checks the configuration and creates the room.
 * \return NULL, with the reason in why, when the room cannot run: a link names an unknown node, a
 * node hears nobody, two nodes share a name or a port, a value is out of range, the history is too
 * short, or a cycle of links has too little lookahead and would wait on itself.
 */
hub_room_t *hub_room_create(const hub_conf_t *conf, char *why, size_t why_len);
void hub_room_free(hub_room_t *room);
const hub_conf_t *hub_room_conf(const hub_room_t *room);

/*! \brief Stores n samples that node sent from timestamp on, scaled by gain_dB. A gap since the
 * node's last samples is silence. Samples before what the node already sent are ignored, as an
 * rfsim receiver ignores samples in its past.
 */
void hub_room_write(hub_room_t *room, int node, uint64_t timestamp, const c16_t *samples, int64_t n, float gain_dB);

/*! \brief Mixes the next block for node, when every transmitter it hears has sent far enough.
 * \return true and the block of hub_conf_t.quantum samples in out, starting at *timestamp.
 */
bool hub_room_next_block(hub_room_t *room, int node, uint64_t *timestamp, c16_t *out);

/// The node has left: it sends nothing more, and nobody waits for it.
void hub_room_leave(hub_room_t *room, int node);
bool hub_room_has_left(const hub_room_t *room, int node);
/// true when every transmitter the node hears has left
bool hub_room_hears_nobody(const hub_room_t *room, int node);
/// true when the node is so far ahead of a receiver that its history would be overwritten
bool hub_room_must_wait(const hub_room_t *room, int node);
/// samples [0, written) of the node are stored
int64_t hub_room_written(const hub_room_t *room, int node);
/// samples [0, sent) have been mixed for the node
int64_t hub_room_sent(const hub_room_t *room, int node);

/*! \brief Accepts one rfsim client on each node's port, starts the room at sample 0 once every node
 * is connected, and carries the samples until every node has left.
 * \return 0, or -1 when a port cannot be listened on, or nothing moved for idle_timeout_s seconds
 * after the start.
 */
int hub_serve(hub_room_t *room, double idle_timeout_s);

#ifdef __cplusplus
}
#endif

#endif
