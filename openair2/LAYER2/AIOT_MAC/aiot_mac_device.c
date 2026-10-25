/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac_device.c
 * \brief The device's MAC, 3GPP TS 38.391 V19.4.0 clause 5: paging (5.2), contention-based access
 * (5.3.1), contention-free access (5.3.2), upper layer data (5.4) and failure detection (5.5).
 *
 * The device never touches the air or a clock: each decoded R2D message goes in through
 * aiot_device_on_r2d(), and the D2R it makes the device send comes back as an aiot_device_send_t.
 * The order of the operations and of the random draws is part of the contract (a simulator with a
 * seeded generator repeats its runs), so the code follows the procedures step by step.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "aiot_mac.h"

#define SDU_HEADER_BYTES 2 /* D2R Upper Layer Data Transfer: type, R, MDI, SDU Length */
#define MAX_SDU_BYTES 127 /* 7-bit SDU Length */
#define SENDING_CAP 65536 /* the largest upper layer SDU the device keeps for segmentation */

struct aiot_device {
  aiot_device_upper_t upper;
  aiot_mod_t modulation;
  /* stored state, named as in the spec */
  int transaction_id; /* -1: none */
  int as_id; /* -1: none */
  int failed_tid; /* Transaction ID whose procedure was found failed, -1: none */
  /* CBRA in progress */
  bool cbra_active, cbra_success;
  int counter; /* ACCESS_OCCASION_COUNTER, -1 once Msg1 is sent */
  aiot_sched_info_t cbra_sched;
  int random_id; /* -1: none */
  int msg1_r_sfs;
  int msg2_budget; /* Access Trigger / Paging still allowed while waiting for Msg2 */
  int k;
  /* CFA and data */
  bool cfa_in_procedure;
  bool nack_monitoring;
  int sending_len; /* the SDU being sent, kept while segmented; -1: none */
  uint8_t sending[SENDING_CAP];
};

typedef enum { SEND_MSG2, SEND_CFA, SEND_DATA } data_kind_t;

aiot_device_t *aiot_device_new(const aiot_device_upper_t *upper, aiot_mod_t modulation)
{
  if (!upper || !upper->draw || (modulation != AIOT_OOK && modulation != AIOT_BPSK))
    return NULL;
  aiot_device_t *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->upper = *upper;
  d->modulation = modulation;
  d->transaction_id = -1;
  d->as_id = -1;
  d->failed_tid = -1;
  d->counter = -1;
  d->random_id = -1;
  d->msg1_r_sfs = -1;
  d->k = 1;
  d->sending_len = -1;
  return d;
}

void aiot_device_free(aiot_device_t *d)
{
  free(d);
}

void aiot_device_state(const aiot_device_t *d, aiot_device_state_t *out)
{
  out->transaction_id = d->transaction_id;
  out->as_id = d->as_id;
  out->failed_tid = d->failed_tid;
  out->cbra_active = d->cbra_active;
  out->cbra_success = d->cbra_success;
  out->counter = d->counter;
  out->random_id = d->random_id;
  out->msg2_budget = d->msg2_budget;
  out->cfa_in_procedure = d->cfa_in_procedure;
  out->nack_monitoring = d->nack_monitoring;
  out->sending_len = d->sending_len;
}

int aiot_device_send_offset(const aiot_device_send_t *s, int m_chip_symb)
{
  int frame = aiot_frame_bits(8 * s->tbs_bytes, &s->params);
  if (frame < 0)
    return frame;
  int d2r_chips = frame * 2 * s->params.r_sfs;
  return aiot_t_r_to_d(&s->params, m_chip_symb, s->msg, s->tbs_bytes, s->candidates, s->n_candidates, s->slot > 0, d2r_chips);
}

/* ---------------------------------------------------------------------------------------------
 * Upper layer
 * ------------------------------------------------------------------------------------------- */

static bool upper_paging_id_matched(const aiot_device_t *d, const uint8_t *id, int len)
{
  return d->upper.paging_id_matched ? d->upper.paging_id_matched(d->upper.user, id, len) != 0 : true;
}

/* 5.2: forward the Security Parameter of a paging, when SPPI says it is present */
static void upper_security_param(const aiot_device_t *d, const aiot_mac_r2d_msg_t *m)
{
  if (m->has_security_param && d->upper.security_param)
    d->upper.security_param(d->upper.user, m->security_param, AIOT_MAC_SECURITY_PARAM_BITS);
}

/* ---------------------------------------------------------------------------------------------
 * D2R transmission
 * ------------------------------------------------------------------------------------------- */

static void set_candidates(aiot_device_send_t *out, const aiot_sched_info_t *s)
{
  out->n_candidates = s->n_sfs;
  for (int i = 0; i < s->n_sfs; i++)
    out->candidates[i] = s->r_sfs[i];
}

/* D2RSend of a data message m, on r_sfs (0: the first of the Scheduling Info). */
static int send_data_msg(const aiot_device_t *d,
                         const aiot_mac_d2r_msg_t *m,
                         const aiot_sched_info_t *sched,
                         data_kind_t kind,
                         int r_sfs,
                         aiot_device_send_t *out)
{
  if (r_sfs == 0) {
    if (sched->n_sfs < 1)
      return AIOT_EINVAL;
    r_sfs = sched->r_sfs[0];
  }
  aiot_d2r_params_t p;
  if (aiot_mac_to_d2r_params(sched, r_sfs, d->modulation, &p) < 0)
    return AIOT_EINVAL;
  memset(out, 0, sizeof(*out));
  out->message = *m;
  out->n_bits = aiot_mac_pack_d2r(m, out->bits, sizeof(out->bits));
  if (out->n_bits < 0)
    return AIOT_EINVAL;
  out->params = p;
  out->msg = kind == SEND_MSG2 ? AIOT_MSG2 : kind == SEND_CFA ? AIOT_CFA : AIOT_DATA;
  out->tbs_bytes = sched->d2r_tbs;
  out->slot = 0;
  if (kind == SEND_MSG2)
    set_candidates(out, sched);
  return 1;
}

/* 5.4.4: the segment starting at byte x of the SDU being sent. */
static int segment(aiot_device_t *d, const aiot_sched_info_t *sched, int x, data_kind_t kind, int r_sfs, aiot_device_send_t *out)
{
  if (d->sending_len < 0)
    return 0; /* nothing was being segmented: nothing to continue */
  int room = sched->d2r_tbs - SDU_HEADER_BYTES;
  if (room > MAX_SDU_BYTES)
    room = MAX_SDU_BYTES;
  if (room <= 0)
    return AIOT_EINVAL; /* the D2R TBS leaves no room for a segment */
  int len = d->sending_len;
  int part = x >= len ? 0 : (len - x < room ? len - x : room);
  int last = (long)x + room >= len;
  aiot_mac_d2r_msg_t m;
  if (aiot_mac_d2r_for_tbs(d->sending + (part ? x : 0), part, sched->d2r_tbs, last ? 0 : 1, &m) < 0)
    return AIOT_EINVAL;
  if (last)
    d->sending_len = -1;
  return send_data_msg(d, &m, sched, kind, r_sfs, out);
}

/* 5.4.2: send upper layer data now, segmenting it when it does not fit the D2R TBS. */
static int d2r_data(aiot_device_t *d, const aiot_sched_info_t *sched, data_kind_t kind, int r_sfs, aiot_device_send_t *out)
{
  int len = AIOT_UPPER_SDU_NONE;
  if (d->upper.next_sdu)
    len = d->upper.next_sdu(d->upper.user, d->sending, SENDING_CAP);
  if (len > SENDING_CAP)
    return AIOT_EINVAL;
  d->sending_len = -1;
  if (len >= 0 && SDU_HEADER_BYTES + len > sched->d2r_tbs) {
    d->sending_len = len;
    return segment(d, sched, 0, kind, r_sfs, out);
  }
  /* 5.4.2: MDI 1 when the data is not available yet due to processing delay, else 0 */
  const int mdi = len == AIOT_UPPER_SDU_PENDING;
  aiot_mac_d2r_msg_t m;
  if (aiot_mac_d2r_for_tbs(d->sending, len > 0 ? len : 0, sched->d2r_tbs, mdi, &m) < 0)
    return AIOT_EINVAL;
  return send_data_msg(d, &m, sched, kind, r_sfs, out);
}

/* ---------------------------------------------------------------------------------------------
 * 5.3.1 CBRA
 * ------------------------------------------------------------------------------------------- */

static int occasions(const aiot_device_t *d)
{
  return d->cbra_sched.x * d->cbra_sched.n_sfs;
}

static void stop_cbra(aiot_device_t *d)
{
  d->cbra_active = false;
  d->counter = -1;
}

static int try_occasion(aiot_device_t *d, aiot_device_send_t *out)
{
  int m = occasions(d);
  if (d->counter >= m)
    return 0;
  int k = d->counter;
  int n_sfs = d->cbra_sched.n_sfs;
  int slot = k / n_sfs;
  int r_sfs = d->cbra_sched.r_sfs[k % n_sfs];
  aiot_d2r_params_t p;
  if (aiot_mac_to_d2r_params(&d->cbra_sched, r_sfs, d->modulation, &p) < 0) {
    stop_cbra(d); /* the occasion cannot be sent: the procedure ends, it does not wait for a Msg2 */
    return AIOT_EINVAL;
  }
  /* 5.3.1.2 */
  aiot_mac_d2r_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.kind = AIOT_D2R_ACCESS_RANDOM_ID;
  msg.random_id = (int)d->upper.draw(d->upper.user, 1u << 16);
  memset(out, 0, sizeof(*out));
  out->n_bits = aiot_mac_pack_d2r(&msg, out->bits, sizeof(out->bits));
  if (out->n_bits < 0) {
    stop_cbra(d);
    return AIOT_EINVAL;
  }
  /* Msg1 is sent: only now does the device wait for Msg2 */
  d->counter = -1;
  d->random_id = msg.random_id;
  d->msg1_r_sfs = r_sfs;
  d->msg2_budget = d->k;
  out->message = msg;
  out->params = p;
  out->msg = AIOT_MSG1;
  out->tbs_bytes = 2;
  out->slot = slot;
  set_candidates(out, &d->cbra_sched);
  return 1;
}

static int start_cbra(aiot_device_t *d, const aiot_mac_r2d_msg_t *m, aiot_device_send_t *out)
{
  int k = aiot_mac_k_value(m->k_bit);
  int n = aiot_mac_num_access_occasions(m->num_access_occasions_code);
  if (k < 0 || n < 0)
    return AIOT_EINVAL;
  d->cbra_active = true;
  d->cbra_success = false;
  d->cbra_sched = m->sched;
  d->k = k;
  d->counter = (int)d->upper.draw(d->upper.user, (uint32_t)n);
  return try_occasion(d, out);
}

static int access_trigger(aiot_device_t *d, aiot_device_send_t *out)
{
  /* 5.3.1.3: Msg2 is monitored until K Access Triggers, from Msg1 on, success or not */
  if (d->cbra_active && d->counter < 0 && d->msg2_budget > 0)
    d->msg2_budget--;
  if (d->cbra_active && d->counter >= 0) {
    d->counter -= occasions(d);
    return try_occasion(d, out);
  }
  return 0;
}

/* ---------------------------------------------------------------------------------------------
 * 5.2 paging, with 5.5.3 first
 * ------------------------------------------------------------------------------------------- */

static int paging(aiot_device_t *d, const aiot_mac_r2d_msg_t *m, aiot_device_send_t *out)
{
  /* 5.5.3: a paging while a CBRA has not succeeded marks the stored Transaction ID failed */
  if (d->cbra_active && !d->cbra_success)
    d->failed_tid = d->transaction_id;
  stop_cbra(d);
  d->nack_monitoring = false; /* 5.5.2: NACK monitoring ends at a paging */
  d->msg2_budget = 0;

  if (m->kind == AIOT_R2D_PAGING_CBRA) {
    int tid = m->transaction_id;
    bool fresh = d->transaction_id < 0 || tid != d->transaction_id || (tid == d->transaction_id && d->failed_tid == tid);
    if (!fresh)
      return 0; /* 5.2: already done */
    d->as_id = -1;
    d->transaction_id = tid;
    d->failed_tid = -1;
    d->cfa_in_procedure = false;
    upper_security_param(d, m);
    bool selected;
    if (!m->has_paging_id) {
      if (d->upper.selected)
        d->upper.selected(d->upper.user);
      selected = true;
    } else {
      selected = upper_paging_id_matched(d, m->paging_id, m->paging_id_len);
    }
    if (!selected)
      return 0;
    return start_cbra(d, m, out);
  }

  /* CFA */
  d->as_id = -1;
  d->transaction_id = -1;
  d->failed_tid = -1;
  upper_security_param(d, m);
  if (!upper_paging_id_matched(d, m->paging_id, m->paging_id_len)) {
    d->cfa_in_procedure = false;
    return 0;
  }
  d->cfa_in_procedure = true; /* 5.3.2 */
  return d2r_data(d, &m->sched, SEND_CFA, 0, out);
}

/* ---------------------------------------------------------------------------------------------
 * 5.3.1.3 Random ID Response
 * ------------------------------------------------------------------------------------------- */

static int after_msg2(aiot_device_t *d, const aiot_mac_r2d_msg_t *m, int i, aiot_device_send_t *out)
{
  if (i >= m->sched.n_sfs)
    return AIOT_EINVAL; /* the entry has no R_SFS: the bitmap indicates fewer */
  int r = d2r_data(d, &m->sched, SEND_MSG2, m->sched.r_sfs[i], out);
  if (r < 0)
    return r;
  d->nack_monitoring = true; /* 5.5.2: the first data after CBRA */
  return r;
}

static int random_id_response(aiot_device_t *d, const aiot_mac_r2d_msg_t *m, aiot_device_send_t *out)
{
  if (d->msg2_budget <= 0)
    return 0; /* 5.3.1.3: the Random ID Response is no longer monitored */
  if (d->as_id < 0) {
    if (!d->cbra_active || d->counter >= 0 || d->msg2_budget <= 0)
      return 0;
    for (int i = 0; i < m->n_entries; i++) {
      const aiot_mac_rid_entry_t *e = &m->entries[i];
      if (e->echoed_random_id != d->random_id)
        continue;
      if (e->freq_index >= 0 && aiot_mac_freq_index_to_r_sfs(e->freq_index) != d->msg1_r_sfs)
        continue;
      d->cbra_success = true;
      d->as_id = e->assigned_as_id >= 0 ? e->assigned_as_id : e->echoed_random_id;
      return after_msg2(d, m, i, out);
    }
    return 0;
  }
  for (int i = 0; i < m->n_entries; i++) {
    const aiot_mac_rid_entry_t *e = &m->entries[i];
    int mine = e->assigned_as_id >= 0 ? e->assigned_as_id == d->as_id : e->echoed_random_id == d->as_id;
    if (mine)
      return after_msg2(d, m, i, out);
  }
  return 0;
}

/* ---------------------------------------------------------------------------------------------
 * 5.4.3 R2D data
 * ------------------------------------------------------------------------------------------- */

/* 0, or nonzero when the upper layer indicates an integrity protection failure */
static int deliver(const aiot_device_t *d, const aiot_mac_r2d_msg_t *m)
{
  return d->upper.deliver ? d->upper.deliver(d->upper.user, m->sdu, m->sdu_len) : 0;
}

static int r2d_data(aiot_device_t *d, const aiot_mac_r2d_msg_t *m, aiot_device_send_t *out)
{
  if (d->as_id >= 0 && m->as_id == d->as_id) {
    /* 5.5.2, 5.3.1.3: NACK and Random ID Response monitoring end at R2D data addressed to the device */
    d->nack_monitoring = false;
    d->msg2_budget = 0;
    if (m->has_sdu) {
      /* 5.4.2: no D2R after an integrity protection failure */
      return deliver(d, m) ? 0 : d2r_data(d, &m->sched, SEND_DATA, 0, out);
    }
    if (m->received_data_size == 0)
      return d2r_data(d, &m->sched, SEND_DATA, 0, out);
    return segment(d, &m->sched, m->received_data_size, SEND_DATA, 0, out);
  }
  if (d->as_id < 0 && d->cfa_in_procedure) {
    if (m->has_sdu) {
      d->as_id = m->as_id;
      return deliver(d, m) ? 0 : d2r_data(d, &m->sched, SEND_DATA, 0, out);
    }
    if (m->received_data_size == 0) { /* 5.4.3: the AS ID is taken from this too */
      d->as_id = m->as_id;
      return d2r_data(d, &m->sched, SEND_DATA, 0, out);
    }
  }
  return 0;
}

/* ---------------------------------------------------------------------------------------------
 * 5.5.2 NACK
 * ------------------------------------------------------------------------------------------- */

static void nack(aiot_device_t *d, const aiot_mac_r2d_msg_t *m)
{
  if (d->as_id < 0)
    return;
  int named = 0;
  for (int i = 0; i < m->n_as_ids && !named; i++)
    named = m->as_ids[i] == d->as_id;
  if (!named)
    return;
  d->msg2_budget = 0; /* 5.3.1.3: a NACK naming the device ends the Random ID Response monitoring */
  if (!d->nack_monitoring)
    return;
  d->as_id = -1;
  d->failed_tid = d->transaction_id;
  d->nack_monitoring = false;
}

int aiot_device_on_r2d(aiot_device_t *d, const aiot_mac_r2d_msg_t *msg, aiot_device_send_t *out)
{
  if (!d || !msg || !out)
    return AIOT_EINVAL;
  switch (msg->kind) {
    case AIOT_R2D_PAGING_CBRA:
    case AIOT_R2D_PAGING_CFA:
      return paging(d, msg, out);
    case AIOT_R2D_ACCESS_TRIGGER:
      return access_trigger(d, out);
    case AIOT_R2D_RANDOM_ID_RESPONSE:
      return random_id_response(d, msg, out);
    case AIOT_R2D_UPPER_LAYER_DATA:
      return r2d_data(d, msg, out);
    case AIOT_R2D_NACK_FEEDBACK:
      nack(d, msg);
      return 0;
  }
  return AIOT_EINVAL;
}
