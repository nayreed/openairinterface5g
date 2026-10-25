/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_protocols.c
 * \brief The Reader's protocols: policies over 38.391 messages (see aiot_protocols.h).
 *
 * registration  Tags join once (CBRA, one Transaction ID), then the Reader polls each by AS ID.
 *               The Reader keeps one Transaction ID for the whole run, so a Tag that has joined
 *               ignores later pagings (38.391 5.2), and a Tag whose access failed tries again at the
 *               next paging (5.5.3). Each frame: a CBRA paging offering 2^occasions_exp access
 *               occasions and its Access Triggers, every Msg1 answered with a Random ID Response
 *               that assigns an AS ID and brings that Tag's data; then an R2D Upper Layer Data
 *               Transfer to every registered AS ID (Received Data Size 0), which asks that Tag
 *               alone for data. A Tag is registered once its data after the Random ID Response is
 *               heard. The AS IDs whose data was not heard get a NACK Feedback right after that
 *               Random ID Response: the Tag releases its AS ID and finds its procedure failed
 *               (5.5.2), so it joins again at the next paging instead of waiting for a poll that
 *               never comes. A Tag that missed the NACK keeps the AS ID and ignores the pagings, so
 *               those AS IDs are also polled, up to AIOT_REGISTRATION_UNCONFIRMED_POLLS times: a
 *               poll answered registers the AS ID, and one never answered is dropped.
 * broadcast     Each frame: a CBRA paging with a new Transaction ID, so every Tag answers again,
 *               offering 2^occasions_exp access occasions over the paging and its Access Triggers;
 *               every Msg1 decoded is answered, which brings that Tag's data.
 * unicast       Each frame: for every Tag in the scenario's order, an A-IoT Paging for CFA with
 *               its Paging ID (its node id on 16 bits). Only that Tag answers: no collisions.
 * gen2_q        EPC Gen2's Q algorithm on 38.391: a round is a CBRA paging with a new Transaction
 *               ID offering 2^Q occasions, one per R2D (one time slot, one R_SFS), then 2^Q - 1
 *               Access Triggers (Gen2's Query and QueryRep). After every occasion Qf (from q0) goes
 *               up by c after a collision, down by c after an empty occasion, within [0, 15]; the
 *               next round uses Q = round(Qf), halves to even. Table q: (time, Q, decoded, collided)
 *               per occasion.
 * adaptive_aloha  Framed ALOHA whose frame follows the load: each frame is a CBRA paging with a new
 *               Transaction ID offering 2^v occasions and its Access Triggers. After each frame the
 *               Reader smooths the load, load = (1 - smoothing) load + smoothing (heard + collided),
 *               and moves v one step towards log2(occasions_per_contender x load), within
 *               [min_exp, max_exp], from start_exp. Table adaptation: (time, n, heard, collided) per
 *               frame.
 *
 * The time in a table row is the Reader's clock in milliseconds when the row is added. */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "aiot_protocols.h"

/* -- building blocks ------------------------------------------------------------------------ */

static int next_tid(aiot_policy_ctx_t *ctx)
{
  *ctx->tid = (*ctx->tid + 1) % 64;
  return *ctx->tid;
}

static void paging_cbra(aiot_policy_msg_t *out, int tid, const aiot_sched_info_t *sched, int code)
{
  memset(out, 0, sizeof(*out));
  out->msg.kind = AIOT_R2D_PAGING_CBRA;
  out->msg.transaction_id = tid;
  out->msg.sched = *sched;
  out->msg.num_access_occasions_code = code;
  out->access = 1;
}

static void access_trigger(aiot_policy_msg_t *out)
{
  memset(out, 0, sizeof(*out));
  out->msg.kind = AIOT_R2D_ACCESS_TRIGGER;
  out->access = 1;
}

static int msg1s(const aiot_reader_heard_t *h)
{
  int n = 0;
  for (int i = 0; i < h->n_answers; i++)
    n += h->answers[i].kind == AIOT_ANSWER_MSG1;
  return n;
}

/* R2D messages (the paging and its Access Triggers) to play out 2^v occasions. */
static int r2d_per_paging(int v, const aiot_sched_info_t *sched)
{
  const int m = sched->x * sched->n_sfs;
  return ((1 << v) + m - 1) / m;
}

static int within(const char *name, double v, double lo, double hi, char *err, int err_len)
{
  if (v < lo || v > hi) {
    snprintf(err, err_len, "%s must be within [%g, %g], got %g", name, lo, hi, v);
    return AIOT_EINVAL;
  }
  return 0;
}

/* -- registration ----------------------------------------------------------------------------- */

/* the AS IDs polled, as many as the protocol state holds next to the rest (asserted below) */
#define MAX_REGISTERED 3900

typedef struct {
  bool started, polling, nacked;
  aiot_sched_info_t sched;
  int tid, i, n;
  int n_registered, j;
  uint16_t registered[MAX_REGISTERED];
  /* the AS IDs NACKed and not heard since: their Tag may have missed the NACK (see the top) */
  int n_unconfirmed;
  uint16_t unconfirmed[AIOT_MAC_MAX_NACK_IDS];
  uint8_t polls_left[AIOT_MAC_MAX_NACK_IDS];
  bool answered[AIOT_MAC_MAX_NACK_IDS];
} registration_t;

/* true when as_id is, or now is, in the list; false when the list is full */
static bool registration_add(registration_t *s, int as_id)
{
  for (int r = 0; r < s->n_registered; r++)
    if (s->registered[r] == as_id)
      return true;
  if (s->n_registered == MAX_REGISTERED)
    return false;
  s->registered[s->n_registered++] = (uint16_t)as_id;
  return true;
}

static void registration_unconfirmed(registration_t *s, int as_id)
{
  for (int u = 0; u < s->n_unconfirmed; u++)
    if (s->unconfirmed[u] == as_id)
      return;
  if (s->n_unconfirmed == AIOT_MAC_MAX_NACK_IDS)
    return;
  s->unconfirmed[s->n_unconfirmed] = (uint16_t)as_id;
  s->polls_left[s->n_unconfirmed] = AIOT_REGISTRATION_UNCONFIRMED_POLLS;
  s->answered[s->n_unconfirmed] = false;
  s->n_unconfirmed++;
}

/* After the polls: an unconfirmed AS ID that answered is registered, one out of polls is dropped. */
static void registration_settle(registration_t *s)
{
  int kept = 0;
  for (int u = 0; u < s->n_unconfirmed; u++) {
    if (s->answered[u] && registration_add(s, s->unconfirmed[u]))
      continue;
    if (s->answered[u] || s->polls_left[u] == 0)
      continue; /* the list is full, or nobody holds that AS ID */
    s->unconfirmed[kept] = s->unconfirmed[u];
    s->polls_left[kept] = s->polls_left[u];
    s->answered[kept] = false;
    kept++;
  }
  s->n_unconfirmed = kept;
}

static bool data_from(const aiot_reader_heard_t *h, int as_id)
{
  for (int a = 0; h && a < h->n_answers; a++)
    if (h->answers[a].kind == AIOT_ANSWER_DATA && h->answers[a].as_id == as_id)
      return true;
  return false;
}

static int registration_check(const double *p, char *err, int err_len)
{
  return within("occasions_exp", p[0], 0, 15, err, err_len);
}

static void registration_join(registration_t *s, const double *p, aiot_policy_msg_t *next)
{
  if (s->i == 0)
    paging_cbra(next, s->tid, &s->sched, (int)p[0]);
  else
    access_trigger(next);
}

static int registration_step(void *state,
                             const double *p,
                             aiot_policy_ctx_t *ctx,
                             const aiot_reader_heard_t *h1,
                             const aiot_reader_heard_t *h2,
                             aiot_policy_msg_t *next)
{
  registration_t *s = state;
  (void)h1;
  if (!s->started) {
    s->started = true;
    if (aiot_link_cbra(ctx->cfg, NULL, 0, 0, &s->sched) < 0)
      return 0;
    s->tid = next_tid(ctx);
    s->n = r2d_per_paging((int)p[0], &s->sched);
    s->i = 0;
    registration_join(s, p, next);
    return 1;
  }
  if (!s->polling) {
    if (s->nacked) {
      s->nacked = false; /* the NACK Feedback brings nothing back */
    } else if (h2) {
      /* A Tag whose data was heard has joined. One whose data was lost has its AS ID too and
       * would ignore the pagings that follow (38.391 5.2): NACK it, so that it releases the AS ID
       * and finds its procedure failed (5.5.2), and joins again at the next paging. A Tag heard
       * when the list is full is NACKed too, rather than left holding an AS ID never polled. */
      aiot_policy_msg_t nack = {.msg.kind = AIOT_R2D_NACK_FEEDBACK};
      for (int k = 0; k < h2->sent.n_entries; k++) {
        const aiot_mac_rid_entry_t *e = &h2->sent.entries[k];
        const int as_id = e->assigned_as_id >= 0 ? e->assigned_as_id : e->echoed_random_id;
        if (data_from(h2, as_id) && registration_add(s, as_id))
          continue;
        if (nack.msg.n_as_ids < AIOT_MAC_MAX_NACK_IDS) {
          nack.msg.as_ids[nack.msg.n_as_ids++] = as_id;
          registration_unconfirmed(s, as_id);
        }
      }
      if (nack.msg.n_as_ids > 0) {
        *next = nack;
        s->nacked = true;
        return 1;
      }
    }
    if (++s->i < s->n) {
      registration_join(s, p, next);
      return 1;
    }
    s->polling = true;
    s->j = 0;
  } else {
    const int u = s->j - s->n_registered;
    if (u >= 0 && data_from(h1, s->unconfirmed[u]))
      s->answered[u] = true; /* the Tag missed its NACK and kept the AS ID */
    s->j++;
  }
  /* the registered AS IDs, then the unconfirmed ones */
  const int u = s->j - s->n_registered;
  if (u < s->n_unconfirmed) {
    aiot_sched_info_t uni;
    if (aiot_link_unicast(ctx->cfg, 0, 0, &uni) < 0)
      return 0;
    memset(next, 0, sizeof(*next));
    next->msg.kind = AIOT_R2D_UPPER_LAYER_DATA;
    if (u < 0) {
      next->msg.as_id = s->registered[s->j];
    } else {
      next->msg.as_id = s->unconfirmed[u];
      s->polls_left[u]--;
    }
    next->msg.sched = uni;
    next->msg.has_sdu = 0;
    next->msg.received_data_size = 0;
    return 1;
  }
  registration_settle(s);
  s->polling = false;
  s->i = 0;
  registration_join(s, p, next);
  return 1;
}

/* -- broadcast -------------------------------------------------------------------------------- */

typedef struct {
  bool started;
  aiot_sched_info_t sched;
  int i, n;
} broadcast_t;

static int broadcast_step(void *state,
                          const double *p,
                          aiot_policy_ctx_t *ctx,
                          const aiot_reader_heard_t *h1,
                          const aiot_reader_heard_t *h2,
                          aiot_policy_msg_t *next)
{
  broadcast_t *s = state;
  (void)h1;
  (void)h2;
  if (!s->started) {
    s->started = true;
    if (aiot_link_cbra(ctx->cfg, NULL, 0, 0, &s->sched) < 0)
      return 0;
    s->n = r2d_per_paging((int)p[0], &s->sched);
    s->i = 0;
  } else if (++s->i < s->n) {
    access_trigger(next);
    return 1;
  }
  s->i = 0;
  paging_cbra(next, next_tid(ctx), &s->sched, (int)p[0]);
  return 1;
}

/* -- unicast ---------------------------------------------------------------------------------- */

typedef struct {
  bool started;
  aiot_sched_info_t sched;
  int k;
} unicast_t;

static int unicast_step(void *state,
                        const double *p,
                        aiot_policy_ctx_t *ctx,
                        const aiot_reader_heard_t *h1,
                        const aiot_reader_heard_t *h2,
                        aiot_policy_msg_t *next)
{
  unicast_t *s = state;
  (void)p;
  (void)h1;
  (void)h2;
  if (ctx->n_nodes == 0)
    return 0;
  if (!s->started) {
    s->started = true;
    if (aiot_link_cfa(ctx->cfg, 0, 0, &s->sched) < 0)
      return 0;
    s->k = 0;
  } else {
    s->k = (s->k + 1) % ctx->n_nodes;
  }
  memset(next, 0, sizeof(*next));
  next->msg.kind = AIOT_R2D_PAGING_CFA;
  next->msg.sched = s->sched;
  next->msg.has_paging_id = 1;
  next->msg.paging_id_len = 16;
  aiot_paging_id_of_node(ctx->node_ids[s->k], next->msg.paging_id);
  return 1;
}

/* -- gen2_q ----------------------------------------------------------------------------------- */

typedef struct {
  bool started;
  aiot_sched_info_t sched;
  double qf;
  int q, i;
} gen2_q_t;

static int gen2_q_check(const double *p, char *err, int err_len)
{
  if (within("q0", p[0], 0, 15, err, err_len) < 0)
    return AIOT_EINVAL;
  if (!(p[1] > 0 && p[1] <= 1)) {
    snprintf(err, err_len, "c must be in (0, 1], got %g", p[1]);
    return AIOT_EINVAL;
  }
  return 0;
}

static void gen2_q_round(gen2_q_t *s, aiot_policy_ctx_t *ctx, aiot_policy_msg_t *next)
{
  s->q = (int)nearbyint(s->qf); /* round half to even */
  s->i = 0;
  paging_cbra(next, next_tid(ctx), &s->sched, s->q);
}

static int gen2_q_step(void *state,
                       const double *p,
                       aiot_policy_ctx_t *ctx,
                       const aiot_reader_heard_t *h1,
                       const aiot_reader_heard_t *h2,
                       aiot_policy_msg_t *next)
{
  gen2_q_t *s = state;
  (void)h2;
  if (!s->started) {
    s->started = true;
    s->qf = p[0];
    /* one occasion per R2D, as Gen2's slots: the first R_SFS, one time slot */
    if (aiot_link_cbra(ctx->cfg, ctx->cfg->r_sfs, 1, 1, &s->sched) < 0)
      return 0;
    gen2_q_round(s, ctx, next);
    return 1;
  }
  const int decoded = msg1s(h1);
  if (h1->collided || decoded > 1)
    s->qf = fmin(15.0, s->qf + p[1]);
  else if (!decoded)
    s->qf = fmax(0.0, s->qf - p[1]);
  const double row[4] = {ctx->now_ms, s->q, decoded, h1->collided};
  ctx->add_row(ctx->reader, row);
  if (++s->i < (1 << s->q)) {
    access_trigger(next);
    return 1;
  }
  gen2_q_round(s, ctx, next);
  return 1;
}

/* -- adaptive_aloha --------------------------------------------------------------------------- */

typedef struct {
  bool started;
  aiot_sched_info_t sched;
  int v;
  double load;
  int heard, collided, i, n;
} adaptive_aloha_t;

enum { AA_START, AA_MIN, AA_MAX, AA_SMOOTHING, AA_OPC };

static int adaptive_aloha_check(const double *p, char *err, int err_len)
{
  if (within("min_exp", p[AA_MIN], 0, 15, err, err_len) < 0 || within("max_exp", p[AA_MAX], p[AA_MIN], 15, err, err_len) < 0
      || within("start_exp", p[AA_START], p[AA_MIN], p[AA_MAX], err, err_len) < 0)
    return AIOT_EINVAL;
  if (p[AA_OPC] < 1) {
    snprintf(err, err_len, "occasions_per_contender must be at least 1, got %g", p[AA_OPC]);
    return AIOT_EINVAL;
  }
  if (!(p[AA_SMOOTHING] > 0 && p[AA_SMOOTHING] <= 1)) {
    snprintf(err, err_len, "smoothing must be in (0, 1], got %g", p[AA_SMOOTHING]);
    return AIOT_EINVAL;
  }
  return 0;
}

static void adaptive_aloha_frame(adaptive_aloha_t *s, aiot_policy_ctx_t *ctx, aiot_policy_msg_t *next)
{
  s->heard = s->collided = 0;
  s->i = 0;
  s->n = r2d_per_paging(s->v, &s->sched);
  paging_cbra(next, next_tid(ctx), &s->sched, s->v);
}

static int adaptive_aloha_step(void *state,
                               const double *p,
                               aiot_policy_ctx_t *ctx,
                               const aiot_reader_heard_t *h1,
                               const aiot_reader_heard_t *h2,
                               aiot_policy_msg_t *next)
{
  adaptive_aloha_t *s = state;
  (void)h2;
  if (!s->started) {
    s->started = true;
    s->v = (int)p[AA_START];
    s->load = 0.0;
    if (aiot_link_cbra(ctx->cfg, NULL, 0, 0, &s->sched) < 0)
      return 0;
    adaptive_aloha_frame(s, ctx, next);
    return 1;
  }
  s->heard += msg1s(h1);
  s->collided += h1->collided;
  if (++s->i < s->n) {
    access_trigger(next);
    return 1;
  }
  const double row[4] = {ctx->now_ms, 1 << s->v, s->heard, s->collided};
  ctx->add_row(ctx->reader, row);
  const double sm = p[AA_SMOOTHING];
  s->load = (1 - sm) * s->load + sm * (s->heard + s->collided);
  const double target = fmax(1.0, s->load * p[AA_OPC]);
  int target_v = (int)ceil(log2(target));
  if (target_v < (int)p[AA_MIN])
    target_v = (int)p[AA_MIN];
  if (target_v > (int)p[AA_MAX])
    target_v = (int)p[AA_MAX];
  s->v += (target_v > s->v) - (target_v < s->v);
  adaptive_aloha_frame(s, ctx, next);
  return 1;
}

/* -- manual: the caller decides every message --------------------------------------------------
 * Nothing of its own: the Reader sends what its caller pushes (aiot_reader_push), the way a core
 * network's A-IoT function asks a gNB for a paging or a command, and still answers the Msg1s of a
 * pushed access message with a Random ID Response. */

static int manual_step(void *state,
                       const double *p,
                       aiot_policy_ctx_t *ctx,
                       const aiot_reader_heard_t *h1,
                       const aiot_reader_heard_t *h2,
                       aiot_policy_msg_t *next)
{
  (void)state, (void)p, (void)ctx, (void)h1, (void)h2, (void)next;
  return AIOT_PROTOCOL_WAIT;
}

/* -- the list --------------------------------------------------------------------------------- */

static const aiot_protocol_t PROTOCOLS[] = {
    {.name = "adaptive_aloha",
     .n_params = 5,
     .param_names = {"start_exp", "min_exp", "max_exp", "smoothing", "occasions_per_contender"},
     .param_defaults = {4, 1, 6, 0.25, 2},
     .param_is_int = {1, 1, 1, 0, 1},
     .table = "adaptation.csv",
     .n_cols = 4,
     .columns = {"time_ms", "n_occasions", "heard", "collided"},
     .check = adaptive_aloha_check,
     .step = adaptive_aloha_step},
    {.name = "broadcast",
     .n_params = 1,
     .param_names = {"occasions_exp"},
     .param_defaults = {2},
     .param_is_int = {1},
     .check = registration_check,
     .step = broadcast_step},
    {.name = "gen2_q",
     .n_params = 2,
     .param_names = {"q0", "c"},
     .param_defaults = {4.0, 0.3},
     .param_is_int = {0, 0},
     .table = "q.csv",
     .n_cols = 4,
     .columns = {"time_ms", "q", "decoded", "collided"},
     .check = gen2_q_check,
     .step = gen2_q_step},
    {.name = "manual", .n_params = 0, .step = manual_step},
    {.name = "registration",
     .n_params = 1,
     .param_names = {"occasions_exp"},
     .param_defaults = {2},
     .param_is_int = {1},
     .check = registration_check,
     .step = registration_step},
    {.name = "unicast", .n_params = 0, .step = unicast_step},
};

#define N_PROTOCOLS ((int)(sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0])))

_Static_assert(sizeof(registration_t) <= AIOT_PROTOCOL_STATE_BYTES, "registration state");

const aiot_protocol_t *aiot_protocol_find(const char *name)
{
  for (int i = 0; i < N_PROTOCOLS; i++)
    if (name && !strcmp(PROTOCOLS[i].name, name))
      return &PROTOCOLS[i];
  return NULL;
}

int aiot_protocol_count(void)
{
  return N_PROTOCOLS;
}

const char *aiot_protocol_name(int i)
{
  return i >= 0 && i < N_PROTOCOLS ? PROTOCOLS[i].name : NULL;
}

int aiot_protocol_params(const char *protocol, const char **names, double *defaults, int *is_int, int max)
{
  const aiot_protocol_t *p = aiot_protocol_find(protocol);
  if (!p)
    return AIOT_EINVAL;
  for (int i = 0; i < p->n_params && i < max; i++) {
    names[i] = p->param_names[i];
    defaults[i] = p->param_defaults[i];
    is_int[i] = p->param_is_int[i];
  }
  return p->n_params;
}
