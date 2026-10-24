/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac.h
 * \brief The Rel-19 Ambient IoT MAC procedures, 3GPP TS 38.391 V19.4.0 clause 5: the Reader's MAC
 * with its protocols, and the device's MAC.
 *
 * One implementation for every node that runs them: the Amber simulator (through libaiot_phy.so),
 * a gNB that is the Reader, a UE that is the Reader. Neither entity touches the air or a clock of
 * its own. The caller owns the PHY and the time, in samples at 5.76 Msps (aiot_phy.h), and drives
 * the entity with what happened:
 *
 *   Reader:  next_r2d() -> the caller sends it, from a half-subframe of its own timeline
 *            r2d_sent()  -> the D2R windows to expect (aiot_d2r_rx_expect)
 *            d2r_outcome() for each of them (aiot_d2r_rx_receive) -> the protocol decides the next
 *   Device:  on_r2d() for each R2D decoded -> the D2R to send, and T_R->D after the R2D's end
 *
 * 38.391 says what the device does. What the Reader does (whom to page, how many access occasions,
 * when to trigger, which AS ID to give, when to NACK) is left to the Reader: that is a protocol. The
 * protocols are part of this library, named as a scenario names them; "manual" sends only what the
 * caller pushes (an A-IoT function in the core network, or a test).
 *
 * The readings of the spec where 38.391 leaves room:
 * - Access occasion numbering (5.3.1.1 selects "the (counter+1)th of the m = X * N_SFS occasions"
 *   without ordering them): occasion k is time slot k / N_SFS on R_SFS value k % N_SFS of the
 *   ascending set. The Reader listens on every R_SFS in every slot, so it sees (slot, R_SFS) itself.
 * - Msg2 matching (5.3.1.3 joins "Echoed Random ID matches" and "Frequency Index present and
 *   matching" with "and", yet defines FIPI = 0): the Echoed Random ID matches and, when the
 *   Frequency Index is present, it matches the R_SFS of Msg1 too.
 * - The D2R after Msg2: the i-th entry's device takes the i-th R_SFS of the Random ID Response's
 *   bitmap (Table 6.2.1.6-1, derived-parameter column).
 * - 5.5.3 is checked before 5.2 when a paging arrives, so that a failed CBRA lets the same
 *   Transaction ID start access again.
 * - The Security Parameter of a paging (5.2) is the Reader's own: each paging a Reader protocol
 *   sends carries 128 bits drawn from the Reader's generator (fixed seed, aiot_reader_seed), and a
 *   message pushed to the Reader is sent as the caller built it, SPPI and bits included.
 * - The Msg2 window of 5.3.1.3 (K Access Triggers) runs from the Access Random ID message, whether
 *   or not a Random ID Response succeeded in it, and ends at the next paging, at R2D data
 *   addressed to the device's AS ID, or at a NACK Feedback naming the device's AS ID.
 *
 * Conventions are those of aiot_phy.h: no globals, no threads, caller buffers with a capacity,
 * negative AIOT_E* codes. Random numbers come from the caller (draw callbacks), so a simulator
 * keeps its own seeded generator and every run can be repeated.
 */

#ifndef AIOT_MAC_H
#define AIOT_MAC_H

#include <stdint.h>

#include "aiot_phy.h"
#include "aiot_mac_pdu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------
 * Link settings: what every D2R Scheduling Info of a Reader is built from
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  int m_chip_symb; /* R2D chips per OFDM symbol: 2, 6, 12 or 24 (38.291 6.2.4) */
  int n_rb; /* R2D PRBs; 0: the fewest M allows */
  int t_bit_num; /* D2R bit duration t_bit_num / t_bit_den tau: 2, 1, 1/2 ... 1/96 */
  int t_bit_den;
  int n_r_sfs; /* the R_SFS values a paging offers (its bitmap), ascending */
  int r_sfs[8];
  int x; /* time slots of access occasions per paging or Access Trigger, 1 or 2 */
  int r_code; /* FEC on the D2R */
  int r_block; /* block repetition, 1 or 2 */
  int l_amble; /* 7 or 31 */
  int interval_cp; /* Interval Bits codepoint 0..3: I_bit = S x {48, 96, 168, 240} */
  int i_add; /* additional midamble */
  int d2r_tbs; /* D2R transport block for data, bytes (the SDU gets d2r_tbs - 2) */
} aiot_link_config_t;

/* 0 when the settings make a valid CBRA Scheduling Info (38.391 Table 6.2.1.6-1), else AIOT_EINVAL. */
int aiot_link_config_check(const aiot_link_config_t *cfg);
/* R2D PRBs: cfg->n_rb, or the fewest M allows. */
int aiot_link_n_rb(const aiot_link_config_t *cfg);
/* Devices one Random ID Response can answer: min(8, R_SFS values the bit duration allows). */
int aiot_link_max_entries(const aiot_link_config_t *cfg);
/* The Scheduling Info of each kind of message. cbra: n_r_sfs 0 and x 0 take the link's own;
 * ridr: entry i answers on the i-th smallest R_SFS the bit duration allows; cfa and unicast: r_sfs
 * 0 takes the link's smallest, d2r_tbs 0 the link's. Each returns 0 or AIOT_EINVAL. */
int aiot_link_cbra(const aiot_link_config_t *cfg, const int *r_sfs, int n_r_sfs, int x, aiot_sched_info_t *out);
int aiot_link_ridr(const aiot_link_config_t *cfg, int n_entries, int d2r_tbs, aiot_sched_info_t *out);
int aiot_link_cfa(const aiot_link_config_t *cfg, int r_sfs, int d2r_tbs, aiot_sched_info_t *out);
int aiot_link_unicast(const aiot_link_config_t *cfg, int r_sfs, int d2r_tbs, aiot_sched_info_t *out);

/* The 16-bit Paging ID of a node: its id, MSB first. */
void aiot_paging_id_of_node(int node_id, uint8_t bits[16]);

/* ---------------------------------------------------------------------------------------------
 * Reader
 * ------------------------------------------------------------------------------------------- */

typedef struct aiot_reader aiot_reader_t;

/* A protocol parameter, by the name a scenario gives it. Integers are given as doubles. */
typedef struct {
  const char *name;
  double value;
} aiot_param_t;

/* The protocols this library has, and their parameters with defaults. */
int aiot_protocol_count(void);
const char *aiot_protocol_name(int i);
/* The parameters of a protocol: n written into names and defaults (room for max). is_int[k] is 1
 * when the parameter takes whole numbers only. Returns the count or AIOT_EINVAL. */
int aiot_protocol_params(const char *protocol, const char **names, double *defaults, int *is_int, int max);

/* A Reader running protocol on the link cfg, for the devices node_ids (in the scenario's order).
 * Parameters not given keep their defaults. On a bad protocol, parameter or setting, returns NULL
 * and writes why into err. */
aiot_reader_t *aiot_reader_new(const aiot_link_config_t *cfg,
                               const char *protocol,
                               const aiot_param_t *params,
                               int n_params,
                               const int *node_ids,
                               int n_nodes,
                               char *err,
                               int err_len);
void aiot_reader_free(aiot_reader_t *r);
/* Seeds the generator the Reader draws the Security Parameter of its pagings from (a fixed seed
 * until this is called, so runs repeat). */
void aiot_reader_seed(aiot_reader_t *r, uint64_t seed);

/* The R2D the protocol sends next. */
typedef struct {
  aiot_mac_r2d_msg_t msg;
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  int n_bits;
  int64_t earliest_start; /* the R2D must not start before this sample (the last D2R and T_D2R_min) */
} aiot_reader_r2d_t;

/* 1 and out filled when an R2D is ready to send; 0 while D2R outcomes are awaited, or when the
 * protocol has ended (aiot_reader_done). now is the caller's current sample. */
int aiot_reader_next_r2d(aiot_reader_t *r, int64_t now, aiot_reader_r2d_t *out);

/* The caller sent the R2D next_r2d() gave, over samples [start, end): start a half-subframe at or
 * after earliest_start, end the sample after its last chip. Writes the D2R windows to expect
 * (their tag is the Reader's), at most max, and returns how many. With none (a message no device
 * answers), the protocol moves on at once and the next R2D is ready. */
int aiot_reader_r2d_sent(aiot_reader_t *r, int64_t start, int64_t end, int64_t now, aiot_d2r_expect_t *expects, int max);

/* One outcome of a window r2d_sent() gave (from aiot_d2r_rx_receive). When the last window of the
 * R2D is in, the protocol takes what was heard and decides the next R2D. Returns 0, or AIOT_EINVAL
 * for an outcome the Reader did not expect. */
int aiot_reader_d2r_outcome(aiot_reader_t *r, const aiot_d2r_outcome_t *o, int64_t now);

/* 1 once the protocol has nothing more to send. */
int aiot_reader_done(const aiot_reader_t *r);

/* The caller's own message, sent next (with the protocol "manual", every message is the
 * caller's): when access is 1 (a CBRA paging or an Access Trigger), the Reader answers its Msg1s
 * with a Random ID Response, as for a protocol's. Only while the Reader is idle; returns 0, or
 * AIOT_EINVAL when it is not idle or msg does not pack. */
int aiot_reader_push(aiot_reader_t *r, const aiot_mac_r2d_msg_t *msg, int access);
/* 1 when the Reader has nothing to send and no answers to wait for (aiot_reader_push). */
int aiot_reader_idle(const aiot_reader_t *r);

/* What followed one R2D: every answer decoded, and the collisions (preambles whose frame did not
 * decode, or decoded bits that are not a valid PDU). */
typedef enum { AIOT_ANSWER_MSG1 = 0, AIOT_ANSWER_DATA = 1 } aiot_answer_kind_t;

typedef struct {
  aiot_answer_kind_t kind;
  aiot_mac_d2r_msg_t message; /* Msg1: random_id; data: the SDU */
  int r_sfs;
  double rssi_w;
  int64_t start;
  int slot; /* Msg1: its access occasion's time slot */
  int entry; /* data after a Random ID Response: the entry it answers, else -1 */
  int as_id; /* data: the AS ID it came from, if known, else -1 */
} aiot_reader_answer_t;

#define AIOT_READER_MAX_ANSWERS (AIOT_MAC_MAX_ID_ENTRIES * AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS)

typedef struct {
  aiot_mac_r2d_msg_t sent;
  int64_t r2d_start, r2d_end;
  int n_answers;
  aiot_reader_answer_t answers[AIOT_READER_MAX_ANSWERS];
  int collided;
} aiot_reader_heard_t;

/* The oldest R2D whose answers are all in and not yet taken, for the caller's records: 1 and out
 * filled, or 0 when there is none. Keep taking them: the Reader holds at most a few. */
int aiot_reader_pop_heard(aiot_reader_t *r, aiot_reader_heard_t *out);

/* A protocol's own records (gen2_q's q.csv, adaptive_aloha's adaptation.csv): the table name and
 * columns, and the rows added since the last call, row-major (n_cols doubles a row). Returns the
 * rows written, or 0 for a protocol without one. */
int aiot_reader_table(const aiot_reader_t *r, const char **name, const char *const **columns, int *n_cols);
int aiot_reader_pop_rows(aiot_reader_t *r, double *rows, int max_rows);

/* ---------------------------------------------------------------------------------------------
 * Device
 * ------------------------------------------------------------------------------------------- */

typedef struct aiot_device aiot_device_t;

/* 5.4.2: next_sdu returns for upper layer data not to send now */
#define AIOT_UPPER_SDU_NONE (-1) /* no upper layer data (or the NAS message was ignored, TS 24.369): MDI 0 */
#define AIOT_UPPER_SDU_PENDING (-2) /* not available yet due to processing delay: MDI 1, SDU Length 0 */
/* deliver returns this (any nonzero value) for an integrity protection failure */
#define AIOT_UPPER_INTEGRITY_FAILURE 1

/* What the device's upper layer and its random numbers give the MAC. Any callback may be NULL:
 * every Paging ID matches, nothing is delivered or sent. draw must not be NULL. */
typedef struct {
  void *user;
  /* 5.2: the paging's Paging ID (len bits) is this device's */
  int (*paging_id_matched)(void *user, const uint8_t *paging_id, int len);
  /* 5.2: a paging without Paging ID selected the device */
  void (*selected)(void *user);
  /* 5.4.3: an R2D Data SDU for this device. Returns 0, or nonzero for an integrity protection
   * failure: the SDU is delivered all the same, but the MAC sends no D2R for it (5.4.2). */
  int (*deliver)(void *user, const uint8_t *sdu, int len);
  /* 5.4.2: upper layer data to send now, written into sdu (room for cap bytes): its length,
   * AIOT_UPPER_SDU_NONE (any other negative value too) or AIOT_UPPER_SDU_PENDING */
  int (*next_sdu)(void *user, uint8_t *sdu, int cap);
  /* a uniform whole number in [0, n) */
  uint32_t (*draw)(void *user, uint32_t n);
  /* 5.2: the Security Parameter of a paging (n_bits bits, 0/1, first = MSB), when SPPI is 1 */
  void (*security_param)(void *user, const uint8_t *bits, int n_bits);
} aiot_device_upper_t;

aiot_device_t *aiot_device_new(const aiot_device_upper_t *upper, aiot_mod_t modulation);
void aiot_device_free(aiot_device_t *d);

/* One D2R the device's MAC asks the PHY to send. */
typedef struct {
  aiot_mac_d2r_msg_t message;
  uint8_t bits[AIOT_MAC_MAX_PDU_BITS];
  int n_bits;
  aiot_d2r_params_t params;
  aiot_d2r_msg_t msg; /* for T_R->D: AIOT_MSG1, AIOT_MSG2, AIOT_CFA or AIOT_DATA */
  int tbs_bytes;
  int slot; /* Msg1: the time slot of its access occasion, 0 or 1 */
  int n_candidates; /* Msg1 and Msg2: the R_SFS set T'_chip is taken over */
  int candidates[8];
} aiot_device_send_t;

/* Handles one R2D message the device decoded (38.391 5.2 to 5.5). Returns 1 and out filled when it
 * makes the device send a D2R, 0 when not, or AIOT_EINVAL for a message it cannot act on (a Random
 * ID Response whose bitmap has no R_SFS for the device's entry). */
int aiot_device_on_r2d(aiot_device_t *d, const aiot_mac_r2d_msg_t *msg, aiot_device_send_t *out);

/* T_R->D of a D2R the device sends: from the end of the R2D it answers to the D2R's first sample,
 * for an R2D with m_chip_symb chips per symbol. Returns samples or AIOT_EINVAL. */
int aiot_device_send_offset(const aiot_device_send_t *s, int m_chip_symb);

/* The device's state, for logs and tests. */
typedef struct {
  int transaction_id; /* -1: none */
  int as_id; /* -1: none */
  int failed_tid; /* -1: none */
  int cbra_active, cbra_success;
  int counter; /* ACCESS_OCCASION_COUNTER, -1 once Msg1 is sent */
  int random_id; /* -1: none */
  int msg2_budget;
  int cfa_in_procedure;
  int nack_monitoring;
  int sending_len; /* bytes of the SDU being segmented, -1 when none */
} aiot_device_state_t;

void aiot_device_state(const aiot_device_t *d, aiot_device_state_t *out);

/* sizeof the named type of this API ("aiot_reader_heard_t" ...), or AIOT_EINVAL: lets a binding
 * from another language check its copies of these structures when it loads the library. */
int aiot_mac_struct_size(const char *type);

#ifdef __cplusplus
}
#endif

#endif
