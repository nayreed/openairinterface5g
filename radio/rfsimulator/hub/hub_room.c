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
#include "common/utils/utils.h"

/// samples per flag of hub_node_t.nonzero
#define GROUP 64
/// the pass band's Kaiser window: 60 dB stop band (beta = 0.1102 (60 - 8.7))
#define PASS_KAISER_BETA 5.653
#define PASS_TAPS (2 * HUB_PASS_HALF + 1)

typedef struct {
  int tx;
  int rx;
  float gain;
  int64_t delay;
  /// cycles per sample; 0 for no shift
  double shift;
  /// the pass band, on the transmitter's side of the shift; NULL: none
  cf_t *taps;
  /// samples of the transmitter's future the link needs: HUB_PASS_HALF with a pass band, else 0
  int64_t ahead;
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

/* A receiver can mix samples up to t only once each transmitter it hears has sent up to t - delay
 * (+ HUB_PASS_HALF through a pass band), and a transmitter sends at most its lookahead past what it
 * has received. Following these waits around a cycle of links, the needed position drops by the sum
 * of (lookahead + delay - HUB_PASS_HALF for a pass band) along the cycle. If that sum is below one
 * block, the cycle waits on itself. Returns the smallest sum over all cycles, or INT64_MAX when there
 * is no cycle, and a node on the smallest cycle in *on_cycle.
 *
 * A link through a pass band can weigh less than nothing, so this is Floyd-Warshall, not Dijkstra:
 * w[i][j] is the lightest walk from i to j, and w[i][i] the lightest cycle through i. A cycle below 0
 * is reported as soon as it is found, before the walks around it run away. */
static int64_t smallest_cycle(const hub_room_t *room, int *on_cycle)
{
  const int n = room->conf.num_nodes;
  int64_t(*w)[n] = malloc_or_fail(sizeof(int64_t[n][n]));
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      w[i][j] = INT64_MAX;
  for (int u = 0; u < n; u++)
    for (int k = 0; k < room->nodes[u].num_out; k++) {
      const hub_link_t *l = &room->links[room->nodes[u].out[k]];
      w[u][l->rx] = cmin(w[u][l->rx], room->conf.nodes[u].lookahead + l->delay - l->ahead);
    }
  int64_t best = INT64_MAX;
  for (int k = 0; k < n && best >= 0; k++)
    for (int i = 0; i < n && best >= 0; i++) {
      if (w[i][k] == INT64_MAX)
        continue;
      for (int j = 0; j < n; j++)
        if (w[k][j] != INT64_MAX && w[i][k] + w[k][j] < w[i][j])
          w[i][j] = w[i][k] + w[k][j];
      if (w[i][i] < 0) {
        best = w[i][i];
        *on_cycle = i;
      }
    }
  for (int i = 0; i < n && best >= 0; i++)
    if (w[i][i] < best) {
      best = w[i][i];
      *on_cycle = i;
    }
  free(w);
  return best;
}

static double bessel_i0(double x)
{
  double sum = 1, term = 1;
  for (int k = 1; k < 50 && term > 1e-12 * sum; k++) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}

/* The taps of a pass band width wide (cycles per sample) around centre: a windowed sinc low-pass,
 * whose gain is 1 in the band, moved to centre. */
static cf_t *pass_taps(double centre, double width)
{
  cf_t *h = malloc_or_fail(PASS_TAPS * sizeof(*h));
  double lp[PASS_TAPS], sum = 0;
  for (int j = 0; j < PASS_TAPS; j++) {
    const int n = j - HUB_PASS_HALF;
    const double r = (double)n / HUB_PASS_HALF;
    const double sinc = n == 0 ? width : sin(M_PI * width * n) / (M_PI * n);
    lp[j] = sinc * bessel_i0(PASS_KAISER_BETA * sqrt(1 - r * r)) / bessel_i0(PASS_KAISER_BETA);
    sum += lp[j];
  }
  for (int j = 0; j < PASS_TAPS; j++) {
    const double a = 2 * M_PI * centre * (j - HUB_PASS_HALF);
    h[j] = (cf_t){(float)(lp[j] / sum * cos(a)), (float)(lp[j] / sum * sin(a))};
  }
  return h;
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
    const hub_node_conf_t *n = &c->nodes[i];
    if (n->port < 1 || n->port > UINT16_MAX)
      return refuse(why, why_len, "node %s: port %d is not a TCP port", n->name, n->port);
    if (n->lookahead < 0)
      return refuse(why, why_len, "node %s: lookahead must not be negative", n->name);
    if (!(n->noise_counts >= 0))
      return refuse(why, why_len, "node %s: noise_counts must not be negative", n->name);
    for (int j = 0; j < i; j++) {
      if (strcmp(n->name, c->nodes[j].name) == 0)
        return refuse(why, why_len, "two nodes are named %s", n->name);
      if (n->port == c->nodes[j].port)
        return refuse(why, why_len, "nodes %s and %s share port %d", c->nodes[j].name, n->name, n->port);
    }
  }
  for (int l = 0; l < c->num_links; l++) {
    const hub_link_conf_t *k = &c->links[l];
    if (k->tx < 0 || k->tx >= c->num_nodes || k->rx < 0 || k->rx >= c->num_nodes)
      return refuse(why, why_len, "link %d names an unknown node", l);
    if (k->delay < 0)
      return refuse(why, why_len, "link %d: delay must not be negative", l);
    if (k->shift_Hz != 0.0 && !(c->sample_rate > 0))
      return refuse(why, why_len, "link %d has a frequency shift: set the sample rate", l);
    if (k->pass_width_Hz < 0 || (k->pass_width_Hz > 0 && !(c->sample_rate > 0)))
      return refuse(why, why_len, "link %d: a pass band needs a width above 0 and the sample rate", l);
    if (k->pass_width_Hz >= c->sample_rate && k->pass_width_Hz > 0)
      return refuse(why, why_len, "link %d: the pass band is as wide as the sample rate: leave it out", l);
  }
  return true;
}

hub_room_t *hub_room_create(const hub_conf_t *conf, char *why, size_t why_len)
{
  if (!check_conf(conf, why, why_len))
    return NULL;
  hub_room_t *room = calloc_or_fail(1, sizeof(*room));
  room->conf = *conf;
  room->conf.nodes = malloc_or_fail(conf->num_nodes * sizeof(*conf->nodes));
  memcpy(room->conf.nodes, conf->nodes, conf->num_nodes * sizeof(*conf->nodes));
  room->conf.links = malloc_or_fail((conf->num_links + 1) * sizeof(*conf->links));
  memcpy(room->conf.links, conf->links, conf->num_links * sizeof(*conf->links));
  room->links = calloc_or_fail(conf->num_links + 1, sizeof(*room->links));
  room->nodes = calloc_or_fail(conf->num_nodes, sizeof(*room->nodes));
  room->acc = malloc_or_fail(conf->quantum * sizeof(*room->acc));
  for (int l = 0; l < conf->num_links; l++) {
    const hub_link_conf_t *k = &conf->links[l];
    room->links[l] = (hub_link_t){
        .tx = k->tx,
        .rx = k->rx,
        .gain = powf(10.0f, (float)k->gain_dB / 20.0f),
        .delay = k->delay,
        .shift = k->shift_Hz == 0.0 ? 0.0 : k->shift_Hz / conf->sample_rate,
    };
    if (k->pass_width_Hz > 0) {
      /* the band before the shift: the filter runs on the transmitter's samples */
      room->links[l].taps = pass_taps((k->pass_Hz - k->shift_Hz) / conf->sample_rate, k->pass_width_Hz / conf->sample_rate);
      room->links[l].ahead = HUB_PASS_HALF;
    }
  }

  bool ok = true;
  for (int i = 0; i < conf->num_nodes && ok; i++) {
    hub_node_t *n = &room->nodes[i];
    n->in = malloc_or_fail((conf->num_links + 1) * sizeof(int));
    n->out = malloc_or_fail((conf->num_links + 1) * sizeof(int));
    int64_t longest_delay = 0;
    for (int l = 0; l < conf->num_links; l++) {
      if (room->links[l].rx == i)
        n->in[n->num_in++] = l;
      if (room->links[l].tx == i) {
        n->out[n->num_out++] = l;
        /* a pass band also reads HUB_PASS_HALF samples further back */
        if (room->links[l].delay + room->links[l].ahead > longest_delay)
          longest_delay = room->links[l].delay + room->links[l].ahead;
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
      ok = refuse(why,
                  why_len,
                  "history %" PRId64 " is too short for node %s: it needs at least %" PRId64,
                  conf->history,
                  conf->nodes[i].name,
                  need);
      break;
    }
    n->history = calloc_or_fail(conf->history, sizeof(*n->history));
    n->nonzero = calloc_or_fail(conf->history / GROUP, 1);
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
  for (int l = 0; l < room->conf.num_links; l++)
    free(room->links[l].taps);
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
  return (int16_t)(r > INT16_MAX ? INT16_MAX : r < INT16_MIN ? INT16_MIN : r);
}

void hub_room_write(hub_room_t *room, int node, uint64_t timestamp, const c16_t *samples, int64_t n, float gain_dB)
{
  hub_node_t *nd = &room->nodes[node];
  const int64_t mask = room->conf.history - 1;
  int64_t ts = (int64_t)timestamp;
  int64_t gap_start = cmax(nd->written, ts - room->conf.history);
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

/* The link's frequency shift, from the receiver's sample rx_time on: (cr, ci) turns by (dr, di) per
 * sample. A run is at most GROUP samples, so the recurrence keeps its precision. */
typedef struct {
  double cr, ci, dr, di;
} rotator_t;

static rotator_t rotator_at(double shift, int64_t rx_time)
{
  double cycles = shift * (double)rx_time;
  cycles -= floor(cycles);
  return (rotator_t){cos(2 * M_PI * cycles), sin(2 * M_PI * cycles), cos(2 * M_PI * shift), sin(2 * M_PI * shift)};
}

/* acc += g x, turned by the rotator, which then moves on one sample */
static inline void rotator_add(rotator_t *r, float g, double xr, double xi, cf_t *acc)
{
  acc->r += g * (float)(xr * r->cr - xi * r->ci);
  acc->i += g * (float)(xr * r->ci + xi * r->cr);
  const double t = r->cr * r->dr - r->ci * r->di;
  r->ci = r->cr * r->di + r->ci * r->dr;
  r->cr = t;
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
  rotator_t rot = rotator_at(l->shift, rx_time);
  for (int64_t m = 0; m < run; m++)
    rotator_add(&rot, g, src[m].r, src[m].i, &acc[m]);
}

/* As add_run(), through the link's pass band: each sample is the filter's output at the
 * transmitter's sample, from HUB_PASS_HALF samples before it to HUB_PASS_HALF after (before sample 0:
 * silence). */
static void add_run_filtered(const hub_room_t *room, const hub_link_t *l, int64_t start, int64_t run, int64_t rx_time, cf_t *acc)
{
  const c16_t *h = room->nodes[l->tx].history;
  const int64_t mask = room->conf.history - 1;
  rotator_t rot = rotator_at(l->shift, rx_time);
  /* PASS_TAPS multiplies per sample: a few links through a band at 5.76 Msps fit a core */
  for (int64_t m = 0; m < run; m++) {
    const int64_t newest = start + m + HUB_PASS_HALF;
    float vr = 0, vi = 0;
    for (int j = 0; j < PASS_TAPS && newest - j >= 0; j++) {
      const c16_t x = h[(newest - j) & mask];
      vr += l->taps[j].r * x.r - l->taps[j].i * x.i;
      vi += l->taps[j].r * x.i + l->taps[j].i * x.r;
    }
    rotator_add(&rot, l->gain, vr, vi, &acc[m]);
  }
}

/* true when the transmitter's samples first to last (inclusive) hold anything but silence */
static bool any_nonzero(const hub_room_t *room, const hub_node_t *tx, int64_t first, int64_t last)
{
  if (last < 0)
    return false;
  if (first < 0)
    first = 0;
  const int64_t mask = room->conf.history - 1;
  for (int64_t g = first / GROUP; g <= last / GROUP; g++)
    if (tx->nonzero[((g * GROUP) & mask) / GROUP])
      return true;
  return false;
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
    int64_t r = room->nodes[l->tx].written + l->delay - l->ahead;
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
      if (l->taps) {
        if (any_nonzero(room, tx, start + i - HUB_PASS_HALF, start + i + run - 1 + HUB_PASS_HALF))
          add_run_filtered(room, l, start + i, run, nd->sent + i, room->acc + i);
      } else if (start + i >= 0 && tx->nonzero[pos / GROUP]) {
        add_run(room, l, start + i, run, nd->sent + i, room->acc + i);
      }
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
    int64_t o = room->nodes[l->rx].sent - l->delay - l->ahead;
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
