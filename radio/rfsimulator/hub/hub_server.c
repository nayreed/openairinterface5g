/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file hub_server.c
 * \brief The rfsim side of rfsim_hub: one listening port per node, rfsim's wire format
 * (samplesBlockHeader_t, one gain per stream, then the samples), and the loop that moves samples
 * between the nodes and the room.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "radio/COMMON/common_lib.h"
#include "common/utils/LOG/log.h"
#include "common/utils/utils.h"
#include "rfsim_hub.h"

/// the hub stops mixing for a node while this many blocks wait to be sent to it
#define MAX_QUEUED_BLOCKS 8
#define POLL_TIMEOUT_MS 200
#define SEND_BUFFER_BYTES (16 * 1024 * 1024)

typedef struct {
  int listen_sock;
  int sock;
  // what is being read: the header, then the gains and samples
  samplesBlockHeader_t header;
  bool in_header;
  uint8_t *payload;
  size_t payload_cap;
  size_t need;
  size_t have;
  // what waits to be sent
  uint8_t *out;
  size_t out_len;
  size_t out_off;
  size_t out_cap;
} hub_conn_t;

static void *grow(void *p, size_t size)
{
  p = realloc(p, size);
  AssertFatal(p != NULL, "out of memory for %zu bytes\n", size);
  return p;
}

static double now_s(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

/// The listening socket of a node's port, or -1 with the reason logged.
static int listen_on(int port)
{
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    LOG_E(HW, "rfsim_hub: socket(): %s\n", strerror(errno));
    return -1;
  }
  int one = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
    LOG_W(HW, "rfsim_hub: setsockopt(SO_REUSEADDR) on port %d: %s\n", port, strerror(errno));
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
  // one rfsim client per port
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
    LOG_E(HW, "rfsim_hub: cannot listen on port %d: %s\n", port, strerror(errno));
    close(s);
    return -1;
  }
  return s;
}

/// Makes room for len more bytes in c's send queue and returns where they go.
static uint8_t *queue(hub_conn_t *c, size_t len)
{
  if (c->out_off == c->out_len)
    c->out_len = c->out_off = 0;
  if (c->out_len + len > c->out_cap) {
    c->out_cap = 2 * (c->out_len + len);
    c->out = grow(c->out, c->out_cap);
  }
  uint8_t *p = c->out + c->out_len;
  c->out_len += len;
  return p;
}

/// Queues one single-stream block, as rfsim sends it: header, the stream's gain (0 dB), the samples.
static void queue_block(hub_conn_t *c, uint64_t timestamp, const c16_t *samples, uint32_t n)
{
  samplesBlockHeader_t h = {.size = n, .nbAnt = 1, .timestamp = timestamp};
  const float gain_dB = 0;
  uint8_t *p = queue(c, sizeof(h) + sizeof(gain_dB) + n * sizeof(c16_t));
  memcpy(p, &h, sizeof(h));
  memcpy(p + sizeof(h), &gain_dB, sizeof(gain_dB));
  memcpy(p + sizeof(h) + sizeof(gain_dB), samples, n * sizeof(c16_t));
}

static void drop(hub_room_t *room, hub_conn_t *conns, int i, const char *why)
{
  const hub_conf_t *conf = hub_room_conf(room);
  if (conns[i].sock >= 0)
    close(conns[i].sock);
  conns[i].sock = -1;
  hub_room_leave(room, i);
  LOG_I(HW, "rfsim_hub: node %s left at sample %" PRId64 ": %s\n", conf->nodes[i].name, hub_room_sent(room, i), why);
}

static void flush(hub_room_t *room, hub_conn_t *conns, int i)
{
  hub_conn_t *c = &conns[i];
  while (c->sock >= 0 && c->out_off < c->out_len) {
    ssize_t s = send(c->sock, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (s < 0 && (errno == EAGAIN || errno == EINTR))
      return;
    if (s <= 0) {
      drop(room, conns, i, s == 0 ? "nothing sent" : strerror(errno));
      return;
    }
    c->out_off += s;
  }
}

/// Reads what the node has sent, block by block, until its socket is empty or the room says wait.
static void receive(hub_room_t *room, hub_conn_t *conns, int i)
{
  hub_conn_t *c = &conns[i];
  const hub_conf_t *conf = hub_room_conf(room);
  while (c->sock >= 0) {
    if (c->need == 0) {
      c->in_header = true;
      c->need = sizeof(c->header);
      c->have = 0;
    }
    uint8_t *dst = c->in_header ? (uint8_t *)&c->header + c->have : c->payload + c->have;
    ssize_t r = recv(c->sock, dst, c->need - c->have, MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EINTR))
      return;
    if (r <= 0) {
      drop(room, conns, i, r == 0 ? "connection closed" : strerror(errno));
      return;
    }
    c->have += r;
    if (c->have < c->need)
      continue;
    if (c->in_header) {
      if (c->header.nbAnt != 1) {
        drop(room, conns, i, "rfsim_hub carries one antenna per node");
        return;
      }
      if ((int64_t)c->header.size > conf->history / 2) {
        drop(room, conns, i, "block longer than half the history");
        return;
      }
      size_t len = sizeof(float) + (size_t)c->header.size * sizeof(c16_t);
      if (len > c->payload_cap) {
        c->payload_cap = len;
        c->payload = grow(c->payload, len);
      }
      c->in_header = false;
      c->need = len;
      c->have = 0;
    } else {
      float gain_dB;
      memcpy(&gain_dB, c->payload, sizeof(gain_dB));
      hub_room_write(room, i, c->header.timestamp, (const c16_t *)(c->payload + sizeof(gain_dB)), c->header.size, gain_dB);
      c->need = 0;
      if (hub_room_must_wait(room, i))
        return;
    }
  }
}

/// Mixes every block that can be mixed. Returns the number of blocks mixed.
static int mix(hub_room_t *room, hub_conn_t *conns, c16_t *block)
{
  const hub_conf_t *conf = hub_room_conf(room);
  const size_t block_bytes = sizeof(samplesBlockHeader_t) + sizeof(float) + conf->quantum * sizeof(c16_t);
  int mixed = 0;
  for (int i = 0; i < conf->num_nodes; i++) {
    hub_conn_t *c = &conns[i];
    while (c->sock >= 0 && c->out_len - c->out_off < MAX_QUEUED_BLOCKS * block_bytes) {
      uint64_t ts;
      if (!hub_room_next_block(room, i, &ts, block)) {
        if (hub_room_hears_nobody(room, i) && c->out_off == c->out_len)
          drop(room, conns, i, "every node it hears has left");
        break;
      }
      queue_block(c, ts, block, conf->quantum);
      mixed++;
    }
  }
  return mixed;
}

int hub_serve(hub_room_t *room, double idle_timeout_s)
{
  const hub_conf_t *conf = hub_room_conf(room);
  const int n = conf->num_nodes;
  hub_conn_t *conns = calloc_or_fail(n, sizeof(*conns));
  bool listening = true;
  for (int i = 0; i < n; i++) {
    conns[i].sock = -1;
    conns[i].listen_sock = listen_on(conf->nodes[i].port);
    listening &= conns[i].listen_sock >= 0;
  }
  if (!listening) {
    for (int i = 0; i < n; i++)
      if (conns[i].listen_sock >= 0)
        close(conns[i].listen_sock);
    free(conns);
    return -1;
  }
  struct pollfd *fds = calloc_or_fail(n, sizeof(*fds));
  c16_t *block = malloc_or_fail(conf->quantum * sizeof(c16_t));
  int connected = 0;
  bool started = false;
  double last_move = now_s(), start = 0;
  int ret = 0;
  LOG_I(HW,
        "rfsim_hub: %d nodes, %d links, quantum %" PRId64 " samples: waiting for every node\n",
        n,
        conf->num_links,
        conf->quantum);

  for (;;) {
    int active = 0;
    for (int i = 0; i < n; i++) {
      hub_conn_t *c = &conns[i];
      fds[i] = (struct pollfd){.fd = -1};
      if (c->listen_sock >= 0) {
        fds[i] = (struct pollfd){.fd = c->listen_sock, .events = POLLIN};
      } else if (c->sock >= 0) {
        fds[i] = (struct pollfd){.fd = c->sock, .events = hub_room_must_wait(room, i) ? 0 : POLLIN};
        if (c->out_off < c->out_len)
          fds[i].events |= POLLOUT;
      }
      active += fds[i].fd >= 0;
    }
    if (active == 0)
      break;
    if (poll(fds, n, POLL_TIMEOUT_MS) > 0)
      last_move = now_s();
    if (started && now_s() - last_move > idle_timeout_s) {
      LOG_E(HW, "rfsim_hub: nothing moved for %.0f s\n", idle_timeout_s);
      for (int i = 0; i < n; i++)
        LOG_E(HW,
              "  %s: sent up to sample %" PRId64 ", received up to %" PRId64 "\n",
              conf->nodes[i].name,
              hub_room_written(room, i),
              hub_room_sent(room, i));
      ret = -1;
      break;
    }

    for (int i = 0; i < n; i++) {
      hub_conn_t *c = &conns[i];
      if (!fds[i].revents)
        continue;
      if (c->listen_sock >= 0) {
        c->sock = accept(c->listen_sock, NULL, NULL);
        if (c->sock < 0) {
          LOG_W(HW, "rfsim_hub: accept() on port %d: %s\n", conf->nodes[i].port, strerror(errno));
          continue;
        }
        close(c->listen_sock);
        c->listen_sock = -1;
        int one = 1, buf = SEND_BUFFER_BYTES;
        // as simulator.cpp sets up an rfsim connection; the hub works without them, only slower
        if (setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0
            || setsockopt(c->sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf)) != 0)
          LOG_W(HW, "rfsim_hub: setsockopt() for node %s: %s\n", conf->nodes[i].name, strerror(errno));
        // As an rfsim server does for a new client: one sample that tells the client the current time.
        const c16_t zero = {0};
        queue_block(c, hub_room_sent(room, i), &zero, 1);
        LOG_I(HW, "rfsim_hub: node %s connected on port %d\n", conf->nodes[i].name, conf->nodes[i].port);
        if (++connected == n) {
          started = true;
          start = now_s();
          LOG_I(HW, "rfsim_hub: every node is connected, the room starts at sample 0\n");
        }
        continue;
      }
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
        receive(room, conns, i);
      if (fds[i].revents & POLLOUT)
        flush(room, conns, i);
    }

    if (started)
      while (mix(room, conns, block) > 0)
        ;
    for (int i = 0; i < n; i++)
      flush(room, conns, i);
  }

  int64_t samples = 0;
  for (int i = 0; i < n; i++) {
    if (hub_room_sent(room, i) > samples)
      samples = hub_room_sent(room, i);
    if (conns[i].sock >= 0)
      close(conns[i].sock);
    if (conns[i].listen_sock >= 0)
      close(conns[i].listen_sock);
    free(conns[i].payload);
    free(conns[i].out);
  }
  if (started && conf->sample_rate > 0) {
    double wall = now_s() - start;
    double air = samples / conf->sample_rate;
    LOG_I(HW, "rfsim_hub: carried %.3f s of air in %.3f s (%.2f x real time)\n", air, wall, wall > 0 ? air / wall : 0);
  }
  free(block);
  free(fds);
  free(conns);
  return ret;
}
