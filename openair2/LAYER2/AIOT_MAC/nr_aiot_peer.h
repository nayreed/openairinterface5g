/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_peer.h
 * \brief A UE as the far end of a split Ambient IoT Reader (topology 3): the Reader's MAC runs in the
 * gNB (nr_aiot_reader.h, aiot.remote_r2d and aiot.remote_d2r), and the UE sends the R2Ds the gNB gives
 * it, in its UL (nr_aiot_r2d), or opens the D2R windows the gNB gives it on its DL receiver
 * (nr_aiot_d2r) and sends back each window's outcome.
 *
 * The messages are those of aiot_node_msg.h, one UDP datagram each, over the UE's user plane: the UE
 * listens on a port of its own (aiot.serve_port, on aiot.serve_addr) and answers each message to the
 * address it came from. It serves only the gNB's address (aiot.serve_reader); without one it serves
 * any sender, which can then make the UE send R2D: for a lab network only. The messages carry no
 * authentication. Both ends count time in the cell's frames and slots. An R2D for (frame, slot, symbol) goes in
 * the UE's UL slot of that number, as nr_aiot_r2d_schedule() puts it; a window that starts at A-IoT
 * sample s of (frame, slot) opens at that sample of the UE's DL slot of that number
 * (nr_aiot_d2r_cell_time()), and the D2R starts it holds go back counted from the window's start.
 */

#ifndef NR_AIOT_PEER_H
#define NR_AIOT_PEER_H

#include <netinet/in.h>
#include <stdint.h>
#include "PHY/NR_TRANSPORT/nr_aiot_r2d.h"
#include "PHY/NR_TRANSPORT/nr_aiot_d2r.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_aiot_peer nr_aiot_peer_t;

typedef struct {
  int port; /* UDP port to listen on; 0: one the system picks (see nr_aiot_peer_port()) */
  char addr[INET_ADDRSTRLEN]; /* IPv4 address to listen on; "": every address of the node */
  char reader[INET_ADDRSTRLEN]; /* IPv4 address of the gNB, the only sender served; "": any */
} nr_aiot_peer_config_t;

typedef struct {
  uint64_t r2ds; /* R2Ds the Reader gave */
  uint64_t r2d_too_late; /* of those, the ones whose first slot the UE had already sent */
  uint64_t r2d_refused; /* and the ones nr_aiot_r2d_schedule() refused otherwise */
  int64_t r2d_ahead_min, r2d_ahead_sum; /* slots from the slot the UE had sent last to each R2D's first */
  uint64_t probes; /* probe windows the Reader sent, each answered at once */
  uint64_t windows; /* windows the Reader gave */
  uint64_t windows_late; /* of those, the ones that came after the receiver had passed their start */
  uint64_t windows_refused; /* and the ones the UE could not open: answered at once, empty */
  uint64_t windows_evicted; /* of those answered empty, the oldest open ones, when more than 64 were open */
  int64_t window_ahead_min, window_ahead_sum; /* A-IoT samples from the receiver's progress to each start */
  uint64_t outcomes; /* outcomes sent back */
  uint64_t unsent; /* answers the socket did not take */
  uint64_t refused; /* datagrams that are no message this UE serves */
  uint64_t foreign; /* datagrams from another address than the gNB's, dropped */
} nr_aiot_peer_stats_t;

/*! \brief Listens as cfg says, for a Reader that uses r2d (NULL, or one without PRBs: none) and d2r
 * (NULL: none). \return NULL with the reason in err. */
nr_aiot_peer_t *nr_aiot_peer_new(const nr_aiot_peer_config_t *cfg, nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r, char *err, int err_len);
/*! \brief Stops the thread, if any, logs what it served and frees everything. */
void nr_aiot_peer_free(nr_aiot_peer_t *p);
int nr_aiot_peer_port(const nr_aiot_peer_t *p);

/*! \brief One pass: serves the messages that came in, and sends back the outcomes the receiver has decoded.
 * Returns how many it handled. Does not block: the thread calls it at least every millisecond, tests
 * call it. */
int nr_aiot_peer_step(nr_aiot_peer_t *p);
/*! \brief Starts the thread (threadCreate(), which stops the node if it cannot). Returns 0, or -1 when it runs already. */
int nr_aiot_peer_start(nr_aiot_peer_t *p);
nr_aiot_peer_stats_t nr_aiot_peer_stats(nr_aiot_peer_t *p);

#ifdef __cplusplus
}
#endif

#endif
