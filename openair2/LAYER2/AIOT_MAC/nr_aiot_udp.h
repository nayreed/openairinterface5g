/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_udp.h
 * \brief The UDP sockets of an NR node's Ambient IoT Reader and of a UE's end of a split Reader
 * (nr_aiot_reader.h, nr_aiot_peer.h): one datagram per message of aiot_node_msg.h, never blocking.
 */

#ifndef NR_AIOT_UDP_H
#define NR_AIOT_UDP_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief What a socket did not send: a message it could not take is counted, and logged at the first
 * failure and whenever the error changes. */
typedef struct {
  uint64_t unsent;
  int last_errno;
} nr_aiot_udp_tx_t;

/*! \brief "a.b.c.d:port" into addr. \return 0 or -1. */
int nr_aiot_udp_parse(const char *s, struct sockaddr_in *addr);
/*! \brief "a.b.c.d" into addr, port 0. \return 0 or -1. */
int nr_aiot_udp_parse_host(const char *s, struct sockaddr_in *addr);
/*! \brief Two addresses are the same host and port. */
bool nr_aiot_udp_same(const struct sockaddr_in *a, const struct sockaddr_in *b);

/*! \brief A UDP socket, bound to bind_to when not NULL (port 0: one the system picks).
 * \return the socket, or -1 with the reason in err. */
int nr_aiot_udp_open(const struct sockaddr_in *bind_to, char *err, int err_len);

/*! \brief Sends len bytes of buf to `to` without blocking. len < 0 is a message that could not be
 * packed (its error code): counted as not sent. what names the message in the log.
 * \return true when the socket took it. */
bool nr_aiot_udp_send(int sock, const struct sockaddr_in *to, const uint8_t *buf, int len, nr_aiot_udp_tx_t *tx, const char *what);

/*! \brief Receives every datagram waiting on sock into buf (cap bytes), without blocking, and calls
 * on(ctx, from, n) for each. A receive error other than "nothing waiting" is logged once per errno
 * change, in *last_errno. \return how many datagrams it received. */
int nr_aiot_udp_drain(int sock,
                      uint8_t *buf,
                      int cap,
                      void (*on)(void *ctx, const struct sockaddr_in *from, int n),
                      void *ctx,
                      int *last_errno);

#ifdef __cplusplus
}
#endif

#endif
