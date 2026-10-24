/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_udp.c
 * \brief The UDP sockets of the Ambient IoT Reader and peer (nr_aiot_udp.h).
 */

#include "nr_aiot_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common/utils/LOG/log.h"

int nr_aiot_udp_parse_host(const char *s, struct sockaddr_in *addr)
{
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  return s && inet_pton(AF_INET, s, &addr->sin_addr) == 1 ? 0 : -1;
}

int nr_aiot_udp_parse(const char *s, struct sockaddr_in *addr)
{
  char host[INET_ADDRSTRLEN];
  const char *colon = s ? strrchr(s, ':') : NULL;
  if (!colon || colon == s || colon - s >= (int)sizeof(host))
    return -1;
  memcpy(host, s, colon - s);
  host[colon - s] = 0;
  char *end;
  const long port = strtol(colon + 1, &end, 10);
  if (end == colon + 1 || *end || port < 1 || port > 65535 || nr_aiot_udp_parse_host(host, addr) != 0)
    return -1;
  addr->sin_port = htons((uint16_t)port);
  return 0;
}

bool nr_aiot_udp_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

int nr_aiot_udp_open(const struct sockaddr_in *bind_to, char *err, int err_len)
{
  const int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    snprintf(err, err_len, "no UDP socket: %s", strerror(errno));
    return -1;
  }
  if (bind_to && bind(sock, (const struct sockaddr *)bind_to, sizeof(*bind_to)) != 0) {
    char host[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &bind_to->sin_addr, host, sizeof(host));
    snprintf(err, err_len, "cannot listen on UDP %s:%d: %s", host, ntohs(bind_to->sin_port), strerror(errno));
    close(sock);
    return -1;
  }
  return sock;
}

bool nr_aiot_udp_send(int sock, const struct sockaddr_in *to, const uint8_t *buf, int len, nr_aiot_udp_tx_t *tx, const char *what)
{
  if (len > 0 && sendto(sock, buf, len, MSG_DONTWAIT, (const struct sockaddr *)to, sizeof(*to)) == len)
    return true;
  const int e = len > 0 ? errno : EINVAL;
  if (tx->unsent++ == 0 || e != tx->last_errno) {
    char host[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &to->sin_addr, host, sizeof(host));
    LOG_W(NR_MAC,
          "A-IoT: %s not sent to %s:%d: %s (%lu not sent so far)\n",
          what,
          host,
          ntohs(to->sin_port),
          strerror(e),
          (unsigned long)tx->unsent);
  }
  tx->last_errno = e;
  return false;
}

int nr_aiot_udp_drain(int sock,
                      uint8_t *buf,
                      int cap,
                      void (*on)(void *ctx, const struct sockaddr_in *from, int n),
                      void *ctx,
                      int *last_errno)
{
  int got = 0;
  for (;;) {
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    const ssize_t n = recvfrom(sock, buf, cap, MSG_DONTWAIT, (struct sockaddr *)&from, &len);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != *last_errno)
        LOG_W(NR_MAC, "A-IoT: receive error on UDP: %s\n", strerror(errno));
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        *last_errno = errno;
      return got;
    }
    if (n == 0)
      continue; /* an empty datagram: no message */
    on(ctx, &from, (int)n);
    got++;
  }
}
