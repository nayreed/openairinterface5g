/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_peer.c
 * \brief A UE as the far end of a split Ambient IoT Reader (nr_aiot_peer.h).
 */

#include "nr_aiot_peer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "aiot_node_msg.h"
#include "nr_aiot_udp.h"
#include "common/utils/LOG/log.h"
#include "common/utils/system.h"
#include "common/utils/utils.h"

#define MAX_WINDOWS AIOT_NODE_MAX_WINDOWS /* open at a time */
#define REPORT_SLOTS 1000 /* the peer logs what it served once a second of the UE's slots */

typedef struct {
  int used;
  int64_t local; /* the tag the window has in the UE's receiver */
  int64_t tag; /* the Reader's */
  int ahead;
  struct sockaddr_in from;
} window_t;

struct nr_aiot_peer {
  nr_aiot_r2d_t *r2d; /* NULL: the UE sends no R2D */
  nr_aiot_d2r_t *d2r; /* NULL: the UE receives no D2R */
  int sock;
  int port;
  struct sockaddr_in reader; /* the only sender served; INADDR_ANY: any (a lab network) */
  window_t windows[MAX_WINDOWS];
  int64_t next_local;
  uint8_t *msg; /* AIOT_NODE_SPLIT_MAX_BYTES + 1 */
  aiot_node_r2d_t r2d_msg;
  aiot_node_outcome_t *out;
  nr_aiot_udp_tx_t tx;
  int recv_errno; /* the last receive error */
  int report_slot; /* the slot of the last report, -1: none yet */
  pthread_mutex_t lock; /* step() and stats() */
  nr_aiot_peer_stats_t stats;
  pthread_t thread;
  int running, stop;
};

nr_aiot_peer_t *nr_aiot_peer_new(const nr_aiot_peer_config_t *cfg, nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r, char *err, int err_len)
{
  if (r2d && r2d->cfg.prb_start < 0)
    r2d = NULL;
  if (!r2d && !d2r) {
    snprintf(err,
             err_len,
             "the UE has neither R2D PRBs nor a D2R receiver (aiot.r2d_prb_start, aiot.d2r_prb_start) to serve a Reader with");
    return NULL;
  }
  if (r2d && r2d->slots_per_frame != AIOT_CELL_SLOTS_PER_FRAME) {
    snprintf(err, err_len, "the peer needs 15 kHz slots (here %d slots a frame)", r2d->slots_per_frame);
    return NULL;
  }
  const int port = cfg->port;
  if (port < 0 || port > 65535) {
    snprintf(err, err_len, "port %d is no UDP port (aiot.serve_port)", port);
    return NULL;
  }
  struct sockaddr_in addr;
  if (nr_aiot_udp_parse_host(cfg->addr[0] ? cfg->addr : "0.0.0.0", &addr) != 0) {
    snprintf(err, err_len, "\"%s\" is no IPv4 address to listen on (aiot.serve_addr)", cfg->addr);
    return NULL;
  }
  addr.sin_port = htons((uint16_t)port);
  struct sockaddr_in reader;
  if (nr_aiot_udp_parse_host(cfg->reader[0] ? cfg->reader : "0.0.0.0", &reader) != 0) {
    snprintf(err, err_len, "\"%s\" is no IPv4 address of the gNB (aiot.serve_reader)", cfg->reader);
    return NULL;
  }
  nr_aiot_peer_t *p = calloc_or_fail(1, sizeof(*p));
  p->reader = reader;
  p->r2d = r2d;
  p->d2r = d2r;
  p->report_slot = -1;
  pthread_mutex_init(&p->lock, NULL);
  p->stats.r2d_ahead_min = INT64_MAX;
  p->stats.window_ahead_min = INT64_MAX;
  p->msg = malloc_or_fail(AIOT_NODE_SPLIT_MAX_BYTES + 1);
  p->out = malloc_or_fail(sizeof(*p->out));
  p->sock = nr_aiot_udp_open(&addr, err, err_len);
  socklen_t len = sizeof(addr);
  if (p->sock < 0 || getsockname(p->sock, (struct sockaddr *)&addr, &len) != 0) {
    if (p->sock >= 0)
      snprintf(err, err_len, "cannot read the port of the peer's socket");
    nr_aiot_peer_free(p);
    return NULL;
  }
  p->port = ntohs(addr.sin_port);
  if (p->reader.sin_addr.s_addr == htonl(INADDR_ANY))
    LOG_W(NR_MAC,
          "A-IoT peer: serving any sender on UDP port %d: it can make the UE send R2D and report Tag reads. Give the gNB's "
          "address in aiot.serve_reader outside a lab network\n",
          p->port);
  return p;
}

int nr_aiot_peer_port(const nr_aiot_peer_t *p)
{
  return p->port;
}

static void log_stats(const nr_aiot_peer_t *p, const char *when)
{
  const nr_aiot_peer_stats_t *s = &p->stats;
  if (p->r2d)
    LOG_I(NR_MAC,
          "A-IoT peer%s: %lu R2Ds from the Reader, %lu too late, %lu refused; %ld slots ahead at least, %.1f on average\n",
          when,
          (unsigned long)s->r2ds,
          (unsigned long)s->r2d_too_late,
          (unsigned long)s->r2d_refused,
          s->r2ds ? (long)s->r2d_ahead_min : 0L,
          s->r2ds ? (double)s->r2d_ahead_sum / s->r2ds : 0.0);
  if (p->d2r)
    LOG_I(NR_MAC,
          "A-IoT peer%s: %lu windows from the Reader, %lu late, %lu refused, %lu outcomes back; %.2f ms ahead at least, %.2f on "
          "average\n",
          when,
          (unsigned long)s->windows,
          (unsigned long)s->windows_late,
          (unsigned long)s->windows_refused,
          (unsigned long)s->outcomes,
          s->windows ? (double)s->window_ahead_min / AIOT_SAMPLES_PER_MS : 0.0,
          s->windows ? (double)s->window_ahead_sum / s->windows / AIOT_SAMPLES_PER_MS : 0.0);
  if (s->unsent || s->refused)
    LOG_W(NR_MAC,
          "A-IoT peer%s: %lu answers not sent, %lu datagrams refused\n",
          when,
          (unsigned long)s->unsent,
          (unsigned long)s->refused);
}

void nr_aiot_peer_free(nr_aiot_peer_t *p)
{
  if (!p)
    return;
  if (p->running) {
    pthread_mutex_lock(&p->lock);
    p->stop = 1;
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
  }
  if (p->sock >= 0)
    log_stats(p, " at the end");
  if (p->sock >= 0)
    close(p->sock);
  free(p->msg);
  free(p->out);
  pthread_mutex_destroy(&p->lock);
  free(p);
}

static void answer(nr_aiot_peer_t *p, const struct sockaddr_in *to, int len, const char *what)
{
  nr_aiot_udp_send(p->sock, to, p->msg, len, &p->tx, what);
  p->stats.unsent = p->tx.unsent;
}

static int clamp(int64_t v, int lo, int hi)
{
  return (int)cmax((int64_t)lo, cmin(v, (int64_t)hi));
}

static void stat_add(int64_t v, int64_t *sum, int64_t *min)
{
  *sum += v;
  *min = cmin(*min, v);
}

static void on_r2d(nr_aiot_peer_t *p, const struct sockaddr_in *from, int n)
{
  aiot_node_r2d_t *m = &p->r2d_msg;
  if (!p->r2d || aiot_node_msg_parse_r2d(p->msg, n, m) != 0) {
    p->stats.refused++;
    return;
  }
  const int rc = nr_aiot_r2d_schedule(p->r2d, m->frame, m->slot, m->symbol, m->bits, m->a_bits, m->m_chip_symb, m->n_rb);
  /* after scheduling it, as the UE's TX may take several slots at once: at least this much in hand */
  int64_t ts;
  const int now = nr_aiot_r2d_now(p->r2d, &ts);
  const int ahead = now >= 0 ? nr_aiot_cell_slot_diff(now, m->frame * AIOT_CELL_SLOTS_PER_FRAME + m->slot) : 0;
  nr_aiot_peer_stats_t *s = &p->stats;
  s->r2ds++;
  stat_add(ahead, &s->r2d_ahead_sum, &s->r2d_ahead_min);
  if (rc == NR_AIOT_R2D_ETOOLATE) {
    s->r2d_too_late++;
  } else if (rc < 0) {
    if (aiot_log_due(++s->r2d_refused))
      LOG_E(NR_MAC,
            "A-IoT peer: cannot send the R2D at %d.%d.%d (%d; %lu refused so far)\n",
            m->frame,
            m->slot,
            m->symbol,
            rc,
            (unsigned long)s->r2d_refused);
  }
  LOG_D(NR_MAC, "A-IoT peer: R2D %u at %d.%d.%d, %d slots ahead: %d\n", m->seq, m->frame, m->slot, m->symbol, ahead, rc);
  const aiot_node_r2d_done_t done = {.seq = m->seq,
                                     .result = clamp(rc, INT16_MIN, INT16_MAX),
                                     .ahead = clamp(ahead, INT16_MIN, INT16_MAX)};
  answer(p, from, aiot_node_msg_pack_r2d_done(&done, p->msg, AIOT_NODE_SPLIT_MAX_BYTES), "R2D done");
}

/* An empty outcome at once, for a window the UE cannot open: the Reader's MAC moves on. */
static void answer_empty(nr_aiot_peer_t *p, const struct sockaddr_in *to, int64_t tag)
{
  aiot_node_outcome_t *o = p->out;
  o->ahead = 0;
  memset(&o->outcome.expect, 0, sizeof(o->outcome.expect));
  o->outcome.expect.tag = tag;
  o->outcome.n_decoded = 0;
  o->outcome.collided = 0;
  p->stats.windows_refused++;
  answer(p, to, aiot_node_msg_pack_outcome(o, p->msg, AIOT_NODE_SPLIT_MAX_BYTES), "an empty outcome");
}

static void on_window(nr_aiot_peer_t *p, const struct sockaddr_in *from, int n)
{
  aiot_node_window_t w;
  int64_t tag;
  if (!p->d2r || aiot_node_msg_window_tag(p->msg, n, &tag) != 0) {
    p->stats.refused++;
    return;
  }
  nr_aiot_peer_stats_t *s = &p->stats;
  s->windows++;
  if (aiot_node_msg_parse_window(p->msg, n, &w) != 0) {
    /* a window the receiver cannot open: the Reader's MAC moves on at once, not after its timeout */
    if (aiot_log_due(s->windows_refused + 1))
      LOG_W(NR_MAC, "A-IoT peer: window %ld has parameters the D2R receiver cannot open: answered empty\n", (long)tag);
    answer_empty(p, from, tag);
    return;
  }
  aiot_d2r_expect_t e = w.expect;
  e.start = nr_aiot_d2r_cell_time(p->d2r, w.at.frame, w.at.slot, w.at.sample);
  if (e.start < 0) {
    if (aiot_log_due(s->windows_refused + 1))
      LOG_W(NR_MAC, "A-IoT peer: a window before the UE has received a slot: answered empty\n");
    answer_empty(p, from, w.expect.tag);
    return;
  }
  const int64_t ahead = e.start - nr_aiot_d2r_received_until(p->d2r);
  stat_add(ahead, &s->window_ahead_sum, &s->window_ahead_min);
  if (ahead < 0)
    s->windows_late++;
  window_t *slot = &p->windows[p->next_local % MAX_WINDOWS];
  if (slot->used) {
    /* the oldest window gives its place: the Reader gets it back empty now, not after its timeout */
    if (aiot_log_due(++s->windows_evicted))
      LOG_W(NR_MAC,
            "A-IoT peer: more than %d windows open: window %ld is answered empty (%lu so far)\n",
            MAX_WINDOWS,
            (long)slot->tag,
            (unsigned long)s->windows_evicted);
    answer_empty(p, &slot->from, slot->tag);
  }
  *slot = (window_t){.used = 1,
                     .local = p->next_local++,
                     .tag = w.expect.tag,
                     .ahead = clamp(ahead, INT32_MIN, INT32_MAX),
                     .from = *from};
  e.tag = slot->local;
  if (nr_aiot_d2r_expect(p->d2r, &e) != 0) {
    slot->used = 0;
    LOG_E(NR_MAC, "A-IoT peer: the D2R receiver refused the window at %d.%d.%d\n", w.at.frame, w.at.slot, w.at.sample);
    answer_empty(p, from, w.expect.tag);
    return;
  }
  LOG_D(NR_MAC,
        "A-IoT peer: window %ld at %d.%d.%d, %.2f ms ahead\n",
        (long)w.expect.tag,
        w.at.frame,
        w.at.slot,
        w.at.sample,
        (double)ahead / AIOT_SAMPLES_PER_MS);
}

/* The outcomes the receiver has decoded, each to the Reader that opened its window. */
static int send_outcomes(nr_aiot_peer_t *p)
{
  int sent = 0;
  aiot_d2r_outcome_t *o = &p->out->outcome;
  while (nr_aiot_d2r_pop(p->d2r, o, 1) == 1) {
    window_t *w = &p->windows[o->expect.tag % MAX_WINDOWS];
    if (!w->used || w->local != o->expect.tag)
      continue; /* its slot went to a later window, logged then */
    const int64_t start = o->expect.start;
    for (int i = 0; i < o->n_decoded; i++)
      o->decoded[i].start -= start;
    o->expect.tag = w->tag;
    p->out->ahead = w->ahead;
    w->used = 0;
    p->stats.outcomes++;
    answer(p, &w->from, aiot_node_msg_pack_outcome(p->out, p->msg, AIOT_NODE_SPLIT_MAX_BYTES), "an outcome");
    sent++;
  }
  return sent;
}

static void report(nr_aiot_peer_t *p)
{
  if (!p->r2d)
    return;
  int64_t ts;
  const int now = nr_aiot_r2d_now(p->r2d, &ts);
  if (now < 0)
    return;
  if (p->report_slot < 0) {
    p->report_slot = now;
  } else if (nr_aiot_cell_slot_diff(p->report_slot, now) >= REPORT_SLOTS) {
    p->report_slot = now;
    log_stats(p, "");
  }
}

/* One datagram: a message of the Reader, from the Reader. */
static void on_datagram(void *ctx, const struct sockaddr_in *from, int n)
{
  nr_aiot_peer_t *p = ctx;
  if (p->reader.sin_addr.s_addr != htonl(INADDR_ANY) && from->sin_addr.s_addr != p->reader.sin_addr.s_addr) {
    if (aiot_log_due(++p->stats.foreign)) {
      char host[INET_ADDRSTRLEN] = "?";
      inet_ntop(AF_INET, &from->sin_addr, host, sizeof(host));
      LOG_W(NR_MAC, "A-IoT peer: a datagram from %s, not the gNB, dropped (%lu so far)\n", host, (unsigned long)p->stats.foreign);
    }
    return;
  }
  const int type = aiot_node_msg_type(p->msg, n);
  if (type == AIOT_NODE_MSG_R2D)
    on_r2d(p, from, n);
  else if (type == AIOT_NODE_MSG_WINDOW)
    on_window(p, from, n);
  else
    p->stats.refused++;
}

int nr_aiot_peer_step(nr_aiot_peer_t *p)
{
  pthread_mutex_lock(&p->lock);
  int handled = nr_aiot_udp_drain(p->sock, p->msg, AIOT_NODE_SPLIT_MAX_BYTES + 1, on_datagram, p, &p->recv_errno);
  if (p->d2r)
    handled += send_outcomes(p);
  report(p);
  pthread_mutex_unlock(&p->lock);
  return handled;
}

nr_aiot_peer_stats_t nr_aiot_peer_stats(nr_aiot_peer_t *p)
{
  pthread_mutex_lock(&p->lock);
  nr_aiot_peer_stats_t s = p->stats;
  pthread_mutex_unlock(&p->lock);
  return s;
}

static void *peer_thread(void *arg)
{
  nr_aiot_peer_t *p = arg;
  for (;;) {
    pthread_mutex_lock(&p->lock);
    const int stop = p->stop;
    pthread_mutex_unlock(&p->lock);
    if (stop)
      break;
    nr_aiot_peer_step(p);
    struct pollfd fd = {.fd = p->sock, .events = POLLIN};
    poll(&fd, 1, 1); /* a message, or a millisecond for the outcomes */
  }
  return NULL;
}

int nr_aiot_peer_start(nr_aiot_peer_t *p)
{
  if (p->running)
    return -1;
  threadCreate(&p->thread, peer_thread, p, "aiot_peer", -1, OAI_PRIORITY_RT_LOW);
  p->running = 1;
  return 0;
}
