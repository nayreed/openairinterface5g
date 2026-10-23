/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file hub_room.c
 * \brief The room of rfsim_hub: what each node has sent, and the mix each node receives.
 */

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rfsim_hub.h"

/// samples per flag of hub_node_t.nonzero
#define GROUP 64

typedef struct {
  int tx;
  int rx;
  float gain;
  int64_t delay;
  /// cycles per sample; 0 for no shift
  double shift;
} hub_link_t;

typedef struct {
  /// the last hub_conf_t.history samples the node sent
  c16_t *history;
  /// per group of GROUP samples in history: 0 when every sample stored so far in it is zero
  uint8_t *nonzero;
  int64_t written;
  int64_t sent;
  int *in;
  int num_in;
  int *out;
  int num_out;
  bool left;
} hub_node_t;

struct hub_room_s {
  hub_conf_t conf;
  hub_node_t *nodes;
  hub_link_t *links;
  /// one block of the mix, before rounding
  cf_t *acc;
};

static bool refuse(char *why, size_t why_len, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, why_len, fmt, ap);
  va_end(ap);
  return false;
}

/* A receiver can mix samples up to t only once each transmitter it hears has sent up to t - delay,
 * and a transmitter sends at most its lookahead past what it has received. Following these waits
 * around a cycle of links, the needed position drops by the sum of (lookahead + delay) along the
 * cycle. If that sum is below one block, the cycle waits on itself. Returns the smallest sum over
 * all cycles, or INT64_MAX when there is no cycle, and a node on the smallest cycle in *on_cycle. */
static int64_t smallest_cycle(const hub_room_t *room, int *on_cycle)
{
  const hub_conf_t *c = &room->conf;
  int64_t best = INT64_MAX;
  int64_t *dist = malloc(c->num_nodes * sizeof(*dist));
  bool *done = malloc(c->num_nodes * sizeof(*done));
  for (int s = 0; s < c->num_nodes; s++) {
    for (int i = 0; i < c->num_nodes; i++) {
      dist[i] = INT64_MAX;
      done[i] = false;
    }
    dist[s] = 0;
    for (;;) {
      int u = -1;
      for (int i = 0; i < c->num_nodes; i++)
        if (!done[i] && dist[i] != INT64_MAX && (u < 0 || dist[i] < dist[u]))
          u = i;
      if (u < 0)
        break;
      done[u] = true;
      for (int k = 0; k < room->nodes[u].num_out; k++) {
        const hub_link_t *l = &room->links[room->nodes[u].out[k]];
        int64_t d = dist[u] + c->nodes[u].lookahead + l->delay;
        if (l->rx == s) {
          if (d < best) {
            best = d;
            *on_cycle = s;
          }
        } else if (d < dist[l->rx]) {
          dist[l->rx] = d;
        }
      }
    }
  }
  free(dist);
  free(done);
  return best;
}

static bool check_conf(const hub_conf_t *c, char *why, size_t why_len)
{
  if (c->quantum <= 0)
    return refuse(why, why_len, "quantum must be positive");
  if (c->history <= 0 || (c->history & (c->history - 1)) || c->history % GROUP)
    return refuse(why, why_len, "history must be a power of two of at least %d", GROUP);
  if (c->num_nodes <= 0)
    return refuse(why, why_len, "the room has no node");
  for (int i = 0; i < c->num_nodes; i++) {
    if (c->nodes[i].lookahead < 0)
      return refuse(why, why_len, "node %s: lookahead must not be negative", c->nodes[i].name);
    for (int j = 0; j < i; j++)
      if (c->nodes[i].port == c->nodes[j].port)
        return refuse(why, why_len, "nodes %s and %s share port %d", c->nodes[j].name, c->nodes[i].name, c->nodes[i].port);
  }
  for (int l = 0; l < c->num_links; l++) {
    const hub_link_conf_t *k = &c->links[l];
    if (k->tx < 0 || k->tx >= c->num_nodes || k->rx < 0 || k->rx >= c->num_nodes)
      return refuse(why, why_len, "link %d names an unknown node", l);
    if (k->delay < 0)
      return refuse(why, why_len, "link %d: delay must not be negative", l);
    if (k->shift_Hz != 0.0 && !(c->sample_rate > 0))
      return refuse(why, why_len, "link %d has a frequency shift: set the sample rate", l);
  }
  return true;
}

hub_room_t *hub_room_create(const hub_conf_t *conf, char *why, size_t why_len)
{
  if (!check_conf(conf, why, why_len))
    return NULL;
  hub_room_t *room = calloc(1, sizeof(*room));
  room->conf = *conf;
  room->conf.nodes = malloc(conf->num_nodes * sizeof(*conf->nodes));
  memcpy(room->conf.nodes, conf->nodes, conf->num_nodes * sizeof(*conf->nodes));
  room->conf.links = malloc((conf->num_links + 1) * sizeof(*conf->links));
  memcpy(room->conf.links, conf->links, conf->num_links * sizeof(*conf->links));
  room->links = calloc(conf->num_links + 1, sizeof(*room->links));
  room->nodes = calloc(conf->num_nodes, sizeof(*room->nodes));
  room->acc = malloc(conf->quantum * sizeof(*room->acc));
  for (int l = 0; l < conf->num_links; l++) {
    const hub_link_conf_t *k = &conf->links[l];
    room->links[l] = (hub_link_t){
        .tx = k->tx,
        .rx = k->rx,
        .gain = powf(10.0f, (float)k->gain_dB / 20.0f),
        .delay = k->delay,
        .shift = k->shift_Hz == 0.0 ? 0.0 : k->shift_Hz / conf->sample_rate,
    };
  }

  bool ok = true;
  for (int i = 0; i < conf->num_nodes && ok; i++) {
    hub_node_t *n = &room->nodes[i];
    n->in = malloc((conf->num_links + 1) * sizeof(int));
    n->out = malloc((conf->num_links + 1) * sizeof(int));
    int64_t longest_delay = 0;
    for (int l = 0; l < conf->num_links; l++) {
      if (room->links[l].rx == i)
        n->in[n->num_in++] = l;
      if (room->links[l].tx == i) {
        n->out[n->num_out++] = l;
        if (room->links[l].delay > longest_delay)
          longest_delay = room->links[l].delay;
      }
    }
    if (n->num_in == 0) {
      ok = refuse(why, why_len, "node %s hears nobody: give it a link", conf->nodes[i].name);
      break;
    }
    /* The hub stops reading a node that is half the history ahead of a receiver (hub_room_must_wait).
     * The node must still be able to write its lookahead, the block in flight and the longest delay,
     * or it would be stopped before the receivers can move. */
    int64_t need = 2 * (conf->nodes[i].lookahead + 2 * conf->quantum + longest_delay);
    if (conf->history < need) {
      ok = refuse(why, why_len, "history %" PRId64 " is too short for node %s: it needs at least %" PRId64, conf->history, conf->nodes[i].name, need);
      break;
    }
    n->history = calloc(conf->history, sizeof(*n->history));
    n->nonzero = calloc(conf->history / GROUP, 1);
    if (conf->nodes[i].lead)
      n->written = conf->nodes[i].lookahead;
  }
  if (ok) {
    int node = 0;
    int64_t cycle = smallest_cycle(room, &node);
    if (cycle < conf->quantum)
      ok = refuse(why,
                  why_len,
                  "a cycle of links through node %s has %" PRId64 " samples of lookahead and delay, less than the quantum %" PRId64
                  ": it would wait on itself",
                  conf->nodes[node].name,
                  cycle,
                  conf->quantum);
  }
  if (!ok) {
    hub_room_free(room);
    return NULL;
  }
  return room;
}

void hub_room_free(hub_room_t *room)
{
  if (!room)
    return;
  for (int i = 0; i < room->conf.num_nodes; i++) {
    free(room->nodes[i].history);
    free(room->nodes[i].nonzero);
    free(room->nodes[i].in);
    free(room->nodes[i].out);
  }
  free(room->nodes);
  free(room->links);
  free(room->acc);
  free(room->conf.nodes);
  free(room->conf.links);
  free(room);
}

const hub_conf_t *hub_room_conf(const hub_room_t *room)
{
  return &room->conf;
}

/* Counter-based Gaussian noise: two of them for (node, t), from a hash of both (splitmix64), so the
 * noise does not depend on how the blocks are cut. */
static uint64_t mix64(uint64_t z)
{
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static cf_t gaussian_pair(int node, int64_t t)
{
  const uint64_t h1 = mix64(((uint64_t)node << 48) ^ (uint64_t)t ^ 0x9e3779b97f4a7c15ULL);
  const uint64_t h2 = mix64(h1 + 0x9e3779b97f4a7c15ULL);
  const double u1 = ((h1 >> 11) + 1.0) / 9007199254740993.0; /* (0, 1] */
  const double u2 = (h2 >> 11) / 9007199254740992.0;
  const double r = sqrt(-2.0 * log(u1));
  return (cf_t){(float)(r * cos(2 * M_PI * u2)), (float)(r * sin(2 * M_PI * u2))};
}

static int16_t saturate(float v)
{
  float r = rintf(v);
  return (int16_t)(r > 32767.f ? 32767.f : r < -32768.f ? -32768.f : r);
}

void hub_room_write(hub_room_t *room, int node, uint64_t timestamp, const c16_t *samples, int64_t n, float gain_dB)
{
  hub_node_t *nd = &room->nodes[node];
  const int64_t mask = room->conf.history - 1;
  int64_t ts = (int64_t)timestamp;
  int64_t gap_start = nd->written > ts - room->conf.history ? nd->written : ts - room->conf.history;
  for (int64_t t = gap_start; t < ts; t++) {
    nd->history[t & mask] = (c16_t){0, 0};
    if ((t & (GROUP - 1)) == 0)
      nd->nonzero[(t & mask) / GROUP] = 0;
  }
  if (ts > nd->written)
    nd->written = ts;
  const float g = gain_dB == 0.0f ? 1.0f : powf(10.0f, gain_dB / 20.0f);
  for (int64_t i = nd->written - ts; i < n; i++) {
    int64_t t = ts + i;
    c16_t s = g == 1.0f ? samples[i] : (c16_t){saturate(samples[i].r * g), saturate(samples[i].i * g)};
    nd->history[t & mask] = s;
    uint8_t *flag = &nd->nonzero[(t & mask) / GROUP];
    if ((t & (GROUP - 1)) == 0)
      *flag = 0;
    if (s.r | s.i)
      *flag = 1;
  }
  if (ts + n > nd->written)
    nd->written = ts + n;
}

/* Adds run samples of link l, from the transmitter's sample start on, to acc. The receiver's time of
 * acc[0] is rx_time: a shift rotates by the receiver's time, so it is continuous across blocks. */
static void add_run(const hub_room_t *room, const hub_link_t *l, int64_t start, int64_t run, int64_t rx_time, cf_t *acc)
{
  const c16_t *src = &room->nodes[l->tx].history[start & (room->conf.history - 1)];
  const float g = l->gain;
  if (l->shift == 0.0) {
    for (int64_t m = 0; m < run; m++) {
      acc[m].r += g * src[m].r;
      acc[m].i += g * src[m].i;
    }
    return;
  }
  double cycles = l->shift * (double)rx_time;
  cycles -= floor(cycles);
  double cr = cos(2 * M_PI * cycles), ci = sin(2 * M_PI * cycles);
  const double dr = cos(2 * M_PI * l->shift), di = sin(2 * M_PI * l->shift);
  for (int64_t m = 0; m < run; m++) {
    acc[m].r += g * (float)(src[m].r * cr - src[m].i * ci);
    acc[m].i += g * (float)(src[m].r * ci + src[m].i * cr);
    // a run is at most GROUP samples, so the rotation recurrence keeps its precision
    double t = cr * dr - ci * di;
    ci = cr * di + ci * dr;
    cr = t;
  }
}

bool hub_room_next_block(hub_room_t *room, int node, uint64_t *timestamp, c16_t *out)
{
  hub_node_t *nd = &room->nodes[node];
  const int64_t q = room->conf.quantum;
  int64_t ready = INT64_MAX;
  for (int k = 0; k < nd->num_in; k++) {
    const hub_link_t *l = &room->links[nd->in[k]];
    if (room->nodes[l->tx].left)
      continue;
    int64_t r = room->nodes[l->tx].written + l->delay;
    if (r < ready)
      ready = r;
  }
  if (ready == INT64_MAX || nd->sent + q > ready)
    return false;

  memset(room->acc, 0, q * sizeof(*room->acc));
  // links in a fixed order, so the mix is the same from run to run
  for (int k = 0; k < nd->num_in; k++) {
    const hub_link_t *l = &room->links[nd->in[k]];
    const hub_node_t *tx = &room->nodes[l->tx];
    if (tx->left)
      continue;
    const int64_t start = nd->sent - l->delay;
    for (int64_t i = 0; i < q;) {
      const int64_t pos = (start + i) & (room->conf.history - 1);
      int64_t run = GROUP - (pos & (GROUP - 1));
      if (run > q - i)
        run = q - i;
      if (start + i >= 0 && tx->nonzero[pos / GROUP])
        add_run(room, l, start + i, run, nd->sent + i, room->acc + i);
      i += run;
    }
  }
  const float noise = (float)room->conf.nodes[node].noise_counts;
  if (noise > 0)
    for (int64_t m = 0; m < q; m++) {
      const cf_t w = gaussian_pair(node, nd->sent + m);
      room->acc[m].r += noise * w.r;
      room->acc[m].i += noise * w.i;
    }
  for (int64_t m = 0; m < q; m++)
    out[m] = (c16_t){saturate(room->acc[m].r), saturate(room->acc[m].i)};
  *timestamp = (uint64_t)nd->sent;
  nd->sent += q;
  return true;
}

void hub_room_leave(hub_room_t *room, int node)
{
  room->nodes[node].left = true;
}

bool hub_room_has_left(const hub_room_t *room, int node)
{
  return room->nodes[node].left;
}

bool hub_room_hears_nobody(const hub_room_t *room, int node)
{
  const hub_node_t *nd = &room->nodes[node];
  for (int k = 0; k < nd->num_in; k++)
    if (!room->nodes[room->links[nd->in[k]].tx].left)
      return false;
  return true;
}

bool hub_room_must_wait(const hub_room_t *room, int node)
{
  const hub_node_t *nd = &room->nodes[node];
  int64_t oldest = INT64_MAX;
  for (int k = 0; k < nd->num_out; k++) {
    const hub_link_t *l = &room->links[nd->out[k]];
    if (room->nodes[l->rx].left)
      continue;
    int64_t o = room->nodes[l->rx].sent - l->delay;
    if (o < oldest)
      oldest = o;
  }
  return oldest != INT64_MAX && nd->written - oldest > room->conf.history / 2;
}

int64_t hub_room_written(const hub_room_t *room, int node)
{
  return room->nodes[node].written;
}

int64_t hub_room_sent(const hub_room_t *room, int node)
{
  return room->nodes[node].sent;
}
