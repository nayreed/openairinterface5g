/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac_reader.c
 * \brief The Reader's MAC: runs a protocol, turns each of its messages into an R2D and the D2R
 * windows it opens, and what was decoded in them into what the protocol sees next.
 *
 * For every R2D the Reader opens a D2R window for each answer the message can cause, at T_R->D
 * after the R2D's last chip, searched over +-Te (38.291 7.1.2, 38.191 9.2.1):
 * - CBRA paging and Access Trigger: one per time slot of the last CBRA paging's occasions, a Msg1
 *   (16 bits) on any of its R_SFS values;
 * - Random ID Response: one per entry, that device's data on the entry's R_SFS;
 * - CFA paging and R2D Upper Layer Data Transfer: the data, on the Scheduling Info's R_SFS;
 * - NACK Feedback: none.
 * The next R2D waits for the latest end a D2R can have plus T_D2R_min (38.191 9.3), the time after
 * its D2R a device need not yet monitor R2D.
 *
 * The Reader answers the Msg1s a CBRA paging or Access Trigger brings itself, with one Random ID
 * Response to at most aiot_link_max_entries() of them, in the order they were decoded, each given
 * the next AS ID (1 to 65535, wrapping) and its Frequency Index.
 *
 * Every paging the protocol sends carries SPPI 1 and a 128-bit Security Parameter (38.391 5.2, the
 * RAND_AIOT_n of TS 33.369) from the Reader's own generator (splitmix64, fixed seed, so runs repeat).
 * A pushed message is sent as the caller built it. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aiot_node_msg.h"
#include "aiot_protocols.h"
#include "common/utils/utils.h" /* cmin, cmax */

#define MAX_EXPECTS 16
#define HEARD_QUEUE 16
#define READER_SEED 0x41494F5452414E44ULL /* "AIOTRAND" */

typedef struct {
  aiot_answer_kind_t kind;
  int slot;
  int entry;
  int as_id;
} expect_info_t;

struct aiot_reader {
  aiot_link_config_t cfg;
  const aiot_protocol_t *proto;
  double params[AIOT_PROTOCOL_MAX_PARAMS];
  _Alignas(16) unsigned char state[AIOT_PROTOCOL_STATE_BYTES];
  int *node_ids;
  int n_nodes;
  int tid;
  int next_as_id;
  int64_t free_at, last_d2r_end;
  int has_cbra;
  aiot_sched_info_t cbra;
  int done;
  /* the message to send next */
  int ready;
  aiot_policy_msg_t next;
  int next_is_ridr;
  uint64_t rand_state; /* splitmix64 */
  int fill_sec; /* next was built by the protocol: its paging gets a Security Parameter */
  /* the R2D in flight */
  aiot_reader_heard_t *open;
  int open_access, open_ridr;
  int waiting, n_expects;
  expect_info_t exp[MAX_EXPECTS];
  int64_t seq;
  /* what the paging or Access Trigger brought, while its Random ID Response is out */
  aiot_reader_heard_t *h1;
  /* records */
  aiot_reader_heard_t *queue[HEARD_QUEUE];
  int q_head, q_n;
  double *rows;
  int n_rows, cap_rows;
};

static uint64_t splitmix64(uint64_t *state)
{
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* SPPI 1 and 128 bits (first = MSB) for a paging the protocol built without one. */
static void fill_security(aiot_reader_t *r, aiot_mac_r2d_msg_t *m)
{
  if ((m->kind != AIOT_R2D_PAGING_CBRA && m->kind != AIOT_R2D_PAGING_CFA) || m->has_security_param)
    return;
  m->has_security_param = 1;
  for (int w = 0; w < AIOT_MAC_SECURITY_PARAM_BITS / 64; w++) {
    const uint64_t v = splitmix64(&r->rand_state);
    for (int j = 0; j < 64; j++)
      m->security_param[64 * w + j] = (v >> (63 - j)) & 1;
  }
}

static void add_row(void *reader, const double *row)
{
  aiot_reader_t *r = reader;
  const int nc = r->proto->n_cols;
  if (r->n_rows == r->cap_rows) {
    int cap = r->cap_rows ? 2 * r->cap_rows : 64;
    double *rows = realloc(r->rows, (size_t)cap * nc * sizeof(double));
    if (!rows)
      return;
    r->rows = rows;
    r->cap_rows = cap;
  }
  memcpy(r->rows + (size_t)r->n_rows * nc, row, nc * sizeof(double));
  r->n_rows++;
}

static int step(aiot_reader_t *r, int64_t now, const aiot_reader_heard_t *h1, const aiot_reader_heard_t *h2)
{
  aiot_policy_ctx_t ctx = {.cfg = &r->cfg,
                           .node_ids = r->node_ids,
                           .n_nodes = r->n_nodes,
                           .now_ms = (double)now / AIOT_SAMPLES_PER_MS,
                           .tid = &r->tid,
                           .add_row = add_row,
                           .reader = r};
  const int rc = r->proto->step(r->state, r->params, &ctx, h1, h2, &r->next);
  if (rc == AIOT_PROTOCOL_WAIT) {
    r->ready = 0; /* until the caller pushes a message */
    return 0;
  }
  if (!rc) {
    r->done = 1;
    r->ready = 0;
    return 0;
  }
  r->ready = 1;
  r->next_is_ridr = 0;
  r->fill_sec = 1;
  return 1;
}

aiot_reader_t *aiot_reader_new(const aiot_link_config_t *cfg,
                               const char *protocol,
                               const aiot_param_t *params,
                               int n_params,
                               const int *node_ids,
                               int n_nodes,
                               char *err,
                               int err_len)
{
  char dummy[8];
  if (!err) {
    err = dummy;
    err_len = sizeof(dummy);
  }
  if (!cfg || aiot_link_config_check(cfg) < 0) {
    snprintf(err, err_len, "the link settings do not make a valid 38.391 Scheduling Info");
    return NULL;
  }
  const aiot_protocol_t *proto = aiot_protocol_find(protocol);
  if (!proto) {
    snprintf(err, err_len, "no protocol %s", protocol ? protocol : "(none)");
    return NULL;
  }
  double p[AIOT_PROTOCOL_MAX_PARAMS];
  memcpy(p, proto->param_defaults, sizeof(p));
  for (int i = 0; i < n_params; i++) {
    int k = 0;
    while (k < proto->n_params && strcmp(proto->param_names[k], params[i].name))
      k++;
    if (k == proto->n_params) {
      snprintf(err, err_len, "protocol %s has no parameter %s", proto->name, params[i].name);
      return NULL;
    }
    if (proto->param_is_int[k] && params[i].value != (double)(long long)params[i].value) {
      snprintf(err, err_len, "protocol %s: %s must be a whole number, got %g", proto->name, params[i].name, params[i].value);
      return NULL;
    }
    p[k] = params[i].value;
  }
  if (proto->check) {
    char why[200] = "";
    if (proto->check(p, why, sizeof(why)) < 0) {
      snprintf(err, err_len, "protocol %s: %s", proto->name, why);
      return NULL;
    }
  }
  aiot_reader_t *r = calloc(1, sizeof(*r));
  aiot_reader_heard_t *open = calloc(1, sizeof(*open));
  aiot_reader_heard_t *h1 = calloc(1, sizeof(*h1));
  int *nodes = calloc(n_nodes > 0 ? n_nodes : 1, sizeof(int));
  if (!r || !open || !h1 || !nodes) {
    free(r);
    free(open);
    free(h1);
    free(nodes);
    snprintf(err, err_len, "out of memory");
    return NULL;
  }
  r->cfg = *cfg;
  for (int i = 1; i < r->cfg.n_r_sfs; i++) /* ascending, as a paging's bitmap lists them */
    for (int j = i; j > 0 && r->cfg.r_sfs[j - 1] > r->cfg.r_sfs[j]; j--) {
      const int t = r->cfg.r_sfs[j];
      r->cfg.r_sfs[j] = r->cfg.r_sfs[j - 1];
      r->cfg.r_sfs[j - 1] = t;
    }
  r->proto = proto;
  memcpy(r->params, p, sizeof(p));
  r->open = open;
  r->h1 = h1;
  r->node_ids = nodes;
  if (n_nodes > 0)
    memcpy(nodes, node_ids, n_nodes * sizeof(int));
  r->n_nodes = n_nodes;
  r->next_as_id = 1;
  r->rand_state = READER_SEED;
  step(r, 0, NULL, NULL);
  return r;
}

void aiot_reader_free(aiot_reader_t *r)
{
  if (!r)
    return;
  for (int i = 0; i < r->q_n; i++)
    free(r->queue[(r->q_head + i) % HEARD_QUEUE]);
  free(r->open);
  free(r->h1);
  free(r->node_ids);
  free(r->rows);
  free(r);
}

void aiot_reader_seed(aiot_reader_t *r, uint64_t seed)
{
  r->rand_state = seed;
}

int aiot_reader_done(const aiot_reader_t *r)
{
  return r->done;
}

int aiot_reader_push(aiot_reader_t *r, const aiot_mac_r2d_msg_t *msg, int access)
{
  if (r->done || r->ready || r->waiting || !msg)
    return AIOT_EINVAL; /* the Reader is sending, or waiting for answers */
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  if (aiot_mac_pack_r2d(msg, bits, AIOT_MAC_MAX_PDU_BITS) < 0)
    return AIOT_EINVAL;
  if (access && msg->kind != AIOT_R2D_PAGING_CBRA && msg->kind != AIOT_R2D_ACCESS_TRIGGER)
    return AIOT_EINVAL;
  r->next = (aiot_policy_msg_t){.msg = *msg, .access = access};
  r->ready = 1;
  r->next_is_ridr = 0;
  r->fill_sec = 0; /* as the caller built it */
  return 0;
}

int aiot_reader_idle(const aiot_reader_t *r)
{
  return !r->done && !r->ready && !r->waiting;
}

int aiot_reader_next_r2d(aiot_reader_t *r, int64_t now, aiot_reader_r2d_t *out)
{
  if (r->done || !r->ready)
    return 0;
  if (r->fill_sec) { /* once: asking again gives the same bits */
    fill_security(r, &r->next.msg);
    r->fill_sec = 0;
  }
  out->msg = r->next.msg;
  const int n = aiot_mac_pack_r2d(&out->msg, out->bits, AIOT_MAC_MAX_PDU_BITS);
  if (n < 0)
    return n;
  out->n_bits = n;
  out->earliest_start = r->free_at > now ? r->free_at : now;
  return 1;
}

/* T_R->D of a D2R the Reader expects (link.d2r_start_offset). */
static int
offset(const aiot_reader_t *r, aiot_d2r_msg_t kind, const aiot_d2r_params_t *p, int tbs, const int *cand, int n_cand, int slot)
{
  const int chips = aiot_frame_bits(8 * tbs, p) * 2 * p->r_sfs;
  return aiot_t_r_to_d(p, r->cfg.m_chip_symb, kind, tbs, cand, n_cand, slot > 0, chips);
}

static int add_expect(aiot_reader_t *r,
                      aiot_d2r_expect_t *out,
                      int max,
                      int64_t end,
                      int off,
                      int a_bits,
                      const aiot_d2r_params_t *p,
                      const int *r_sfs,
                      int n_r_sfs,
                      expect_info_t info)
{
  if (off < 0 || r->n_expects >= MAX_EXPECTS || r->n_expects >= max)
    return AIOT_ESPACE;
  aiot_d2r_expect_t *e = &out[r->n_expects];
  memset(e, 0, sizeof(*e));
  e->start = end + off;
  e->te = aiot_timing_tolerance(off);
  e->a_bits = a_bits;
  e->params = *p;
  e->n_r_sfs = n_r_sfs;
  memcpy(e->r_sfs, r_sfs, n_r_sfs * sizeof(int));
  e->tag = r->seq * MAX_EXPECTS + r->n_expects;
  r->exp[r->n_expects++] = info;
  /* the latest end this D2R can have, plus T_D2R_min */
  for (int k = 0; k < n_r_sfs; k++) {
    aiot_d2r_params_t pr = *p;
    pr.r_sfs = r_sfs[k];
    const int64_t last = e->start + e->te + aiot_frame_samples(a_bits, &pr) + aiot_t_d2r_min(&pr);
    r->last_d2r_end = cmax(r->last_d2r_end, last);
  }
  return 0;
}

static int expects(aiot_reader_t *r, const aiot_mac_r2d_msg_t *m, int64_t end, aiot_d2r_expect_t *out, int max)
{
  aiot_d2r_params_t p;
  int rc = 0;
  switch (m->kind) {
    case AIOT_R2D_PAGING_CBRA:
      r->cbra = m->sched;
      r->has_cbra = 1;
      /* fall through */
    case AIOT_R2D_ACCESS_TRIGGER: {
      if (!r->has_cbra)
        return 0;
      const aiot_sched_info_t *s = &r->cbra;
      for (int slot = 0; slot < s->x && rc == 0; slot++) {
        if (aiot_mac_to_d2r_params(s, s->r_sfs[0], AIOT_BPSK, &p) < 0)
          return AIOT_EINVAL;
        const int off = offset(r, AIOT_MSG1, &p, 2, s->r_sfs, s->n_sfs, slot);
        rc = add_expect(r, out, max, end, off, 16, &p, s->r_sfs, s->n_sfs, (expect_info_t){AIOT_ANSWER_MSG1, slot, -1, -1});
      }
      break;
    }
    case AIOT_R2D_RANDOM_ID_RESPONSE: {
      const aiot_sched_info_t *s = &m->sched;
      for (int i = 0; i < m->n_entries && rc == 0; i++) {
        if (i >= s->n_sfs || aiot_mac_to_d2r_params(s, s->r_sfs[i], AIOT_BPSK, &p) < 0)
          return AIOT_EINVAL;
        const aiot_mac_rid_entry_t *e = &m->entries[i];
        const int as_id = e->assigned_as_id >= 0 ? e->assigned_as_id : e->echoed_random_id;
        const int off = offset(r, AIOT_MSG2, &p, s->d2r_tbs, s->r_sfs, s->n_sfs, 0);
        rc = add_expect(r, out, max, end, off, 8 * s->d2r_tbs, &p, &s->r_sfs[i], 1, (expect_info_t){AIOT_ANSWER_DATA, 0, i, as_id});
      }
      break;
    }
    case AIOT_R2D_PAGING_CFA:
    case AIOT_R2D_UPPER_LAYER_DATA: {
      const aiot_sched_info_t *s = &m->sched;
      if (aiot_mac_to_d2r_params(s, s->r_sfs[0], AIOT_BPSK, &p) < 0)
        return AIOT_EINVAL;
      const int cfa = m->kind == AIOT_R2D_PAGING_CFA;
      const int off = offset(r, cfa ? AIOT_CFA : AIOT_DATA, &p, s->d2r_tbs, NULL, 0, 0);
      rc = add_expect(r,
                      out,
                      max,
                      end,
                      off,
                      8 * s->d2r_tbs,
                      &p,
                      s->r_sfs,
                      s->n_sfs,
                      (expect_info_t){AIOT_ANSWER_DATA, 0, -1, cfa ? -1 : m->as_id});
      break;
    }
    case AIOT_R2D_NACK_FEEDBACK:
      break;
  }
  return rc < 0 ? rc : r->n_expects;
}

/* Every answer of the R2D is in: keep the record, and decide the next R2D. */
static void close_open(aiot_reader_t *r, int64_t now)
{
  if (r->q_n < HEARD_QUEUE) {
    aiot_reader_heard_t *copy = malloc(sizeof(*copy));
    if (copy) {
      memcpy(copy, r->open, sizeof(*copy));
      r->queue[(r->q_head + r->q_n) % HEARD_QUEUE] = copy;
      r->q_n++;
    }
  }
  if (r->last_d2r_end > r->open->r2d_end && r->last_d2r_end > r->free_at)
    r->free_at = r->last_d2r_end;

  if (r->open_ridr) {
    step(r, now, r->h1, r->open);
    return;
  }
  if (!r->open_access) {
    step(r, now, r->open, NULL);
    return;
  }
  /* a paging or Access Trigger: answer its Msg1s with one Random ID Response */
  aiot_mac_r2d_msg_t ridr = {.kind = AIOT_R2D_RANDOM_ID_RESPONSE};
  const int max = aiot_link_max_entries(&r->cfg);
  for (int i = 0; i < r->open->n_answers && ridr.n_entries < max; i++) {
    const aiot_reader_answer_t *a = &r->open->answers[i];
    if (a->kind != AIOT_ANSWER_MSG1)
      continue;
    int fi = 0;
    while (fi < 8 && aiot_mac_freq_index_to_r_sfs(fi) != a->r_sfs)
      fi++;
    aiot_mac_rid_entry_t *e = &ridr.entries[ridr.n_entries++];
    e->echoed_random_id = a->message.random_id;
    e->assigned_as_id = r->next_as_id;
    e->freq_index = fi < 8 ? fi : -1;
    r->next_as_id = (r->next_as_id + 1) & 0xFFFF;
    if (!r->next_as_id)
      r->next_as_id = 1;
  }
  if (ridr.n_entries == 0 || aiot_link_ridr(&r->cfg, ridr.n_entries, 0, &ridr.sched) < 0) {
    step(r, now, r->open, NULL);
    return;
  }
  memcpy(r->h1, r->open, sizeof(*r->h1));
  r->next = (aiot_policy_msg_t){.msg = ridr, .access = 0};
  r->ready = 1;
  r->next_is_ridr = 1;
  r->fill_sec = 0;
}

int aiot_reader_r2d_sent(aiot_reader_t *r, int64_t start, int64_t end, int64_t now, aiot_d2r_expect_t *out, int max)
{
  if (r->done || !r->ready || end < start)
    return AIOT_EINVAL;
  if (r->fill_sec) { /* next_r2d not asked: what is recorded is still what would be packed */
    fill_security(r, &r->next.msg);
    r->fill_sec = 0;
  }
  aiot_reader_heard_t *h = r->open;
  h->sent = r->next.msg;
  h->r2d_start = start;
  h->r2d_end = end;
  h->n_answers = 0;
  h->collided = 0;
  r->open_access = r->next.access;
  r->open_ridr = r->next_is_ridr;
  r->ready = 0;
  r->seq++;
  r->n_expects = 0;
  const int n = expects(r, &h->sent, end, out, max);
  if (n < 0)
    return n;
  r->waiting = n;
  r->free_at = end;
  if (n == 0)
    close_open(r, now);
  return n;
}

int aiot_reader_d2r_outcome(aiot_reader_t *r, const aiot_d2r_outcome_t *o, int64_t now)
{
  const int64_t tag = o->expect.tag;
  if (r->waiting == 0 || tag / MAX_EXPECTS != r->seq || tag % MAX_EXPECTS >= r->n_expects)
    return AIOT_EINVAL;
  const expect_info_t *info = &r->exp[tag % MAX_EXPECTS];
  aiot_reader_heard_t *h = r->open;
  h->collided += o->collided;
  for (int i = 0; i < o->n_decoded; i++) {
    const aiot_d2r_result_t *res = &o->decoded[i];
    aiot_mac_d2r_msg_t m;
    const int bad = info->kind == AIOT_ANSWER_MSG1
                        ? aiot_mac_parse_d2r(res->bits, res->n_bits, AIOT_D2R_ACCESS_RANDOM_ID, 0, &m)
                        : aiot_mac_parse_d2r(res->bits, res->n_bits, AIOT_D2R_UPPER_LAYER_DATA, res->n_bits / 8, &m);
    if (bad < 0 || h->n_answers == AIOT_READER_MAX_ANSWERS) {
      h->collided++; /* passed the CRC but is not a valid PDU */
      continue;
    }
    aiot_reader_answer_t *a = &h->answers[h->n_answers++];
    a->kind = info->kind;
    a->message = m;
    a->r_sfs = res->r_sfs;
    a->rssi_w = res->rssi_w;
    a->start = res->start;
    a->slot = info->slot;
    a->entry = info->entry;
    a->as_id = info->as_id;
  }
  if (--r->waiting == 0)
    close_open(r, now);
  return 0;
}

int aiot_reader_pop_heard(aiot_reader_t *r, aiot_reader_heard_t *out)
{
  if (r->q_n == 0)
    return 0;
  aiot_reader_heard_t *h = r->queue[r->q_head];
  memcpy(out, h, offsetof(aiot_reader_heard_t, answers) + h->n_answers * sizeof(h->answers[0]));
  out->collided = h->collided;
  free(h);
  r->q_head = (r->q_head + 1) % HEARD_QUEUE;
  r->q_n--;
  return 1;
}

int aiot_reader_table(const aiot_reader_t *r, const char **name, const char *const **columns, int *n_cols)
{
  if (!r->proto->table)
    return 0;
  *name = r->proto->table;
  *columns = r->proto->columns;
  *n_cols = r->proto->n_cols;
  return 1;
}

int aiot_reader_pop_rows(aiot_reader_t *r, double *rows, int max_rows)
{
  const int nc = r->proto->n_cols;
  const int n = r->n_rows < max_rows ? r->n_rows : max_rows;
  if (n <= 0 || !nc)
    return 0;
  memcpy(rows, r->rows, (size_t)n * nc * sizeof(double));
  memmove(r->rows, r->rows + (size_t)n * nc, (size_t)(r->n_rows - n) * nc * sizeof(double));
  r->n_rows -= n;
  return n;
}

int aiot_mac_struct_size(const char *type)
{
  static const struct {
    const char *name;
    int size;
  } sizes[] = {
      {"aiot_d2r_params_t", sizeof(aiot_d2r_params_t)},
      {"aiot_sched_info_t", sizeof(aiot_sched_info_t)},
      {"aiot_mac_r2d_msg_t", sizeof(aiot_mac_r2d_msg_t)},
      {"aiot_mac_d2r_msg_t", sizeof(aiot_mac_d2r_msg_t)},
      {"aiot_link_config_t", sizeof(aiot_link_config_t)},
      {"aiot_param_t", sizeof(aiot_param_t)},
      {"aiot_reader_r2d_t", sizeof(aiot_reader_r2d_t)},
      {"aiot_reader_answer_t", sizeof(aiot_reader_answer_t)},
      {"aiot_reader_heard_t", sizeof(aiot_reader_heard_t)},
      {"aiot_d2r_expect_t", sizeof(aiot_d2r_expect_t)},
      {"aiot_d2r_outcome_t", sizeof(aiot_d2r_outcome_t)},
      {"aiot_device_upper_t", sizeof(aiot_device_upper_t)},
      {"aiot_device_send_t", sizeof(aiot_device_send_t)},
      {"aiot_device_state_t", sizeof(aiot_device_state_t)},
      {"aiot_tag_data_t", sizeof(aiot_tag_data_t)},
  };
  for (size_t i = 0; type && i < sizeof(sizes) / sizeof(sizes[0]); i++)
    if (!strcmp(sizes[i].name, type))
      return sizes[i].size;
  return AIOT_EINVAL;
}
