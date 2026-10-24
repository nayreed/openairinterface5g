/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac_pdu.h
 * \brief The Rel-19 Ambient IoT MAC PDUs, 3GPP TS 38.391 V19.4.0 clause 6: packer and parser.
 *
 * Conventions are those of aiot_phy.h: bits are uint8_t 0/1, first element = first bit (MSB first),
 * caller buffers come with a capacity, negative AIOT_E* codes are errors, nothing is global.
 * A PDU or field the spec does not allow gives AIOT_EINVAL.
 *
 * Optional values: 0 for "TBS not given" (r2d_tbs, d2r_tbs), 0 for
 * "absent" (sched.x, sched.d2r_tbs), -1 for an absent optional field (entry.assigned_as_id,
 * entry.freq_index) and an explicit has_ flag for the optional Paging ID, Data SDU and the
 * Security Parameter of a Paging message (has_security_param: SPPI, 0 or 1).
 */

#ifndef AIOT_MAC_PDU_H
#define AIOT_MAC_PDU_H

#include <stdint.h>

#include "aiot_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOT_MAC_MAX_TBS 125 /* MAX_TBS: largest R2D and D2R transport block, bytes */
#define AIOT_MAC_MAX_ID_ENTRIES 8 /* MAX_ID_ENTRIES of a Random ID Response */
#define AIOT_MAC_MAX_PAGING_ID_BITS 1023 /* 10-bit Paging ID Length (V19.4.0); 125 bytes hold at most 954 (CFA, SPPI 0) */
#define AIOT_MAC_SECURITY_PARAM_BITS 128 /* Security Parameter: RAND_AIOT_n of TS 33.369 */
#define AIOT_MAC_MAX_RECEIVED_DATA_SIZE 125 /* Received Data Size {0..125} (6.2.1.4) */
#define AIOT_MAC_MAX_NACK_IDS 61 /* TBS = 2 + 2 n <= 125 */
#define AIOT_MAC_MAX_PDU_BITS (8 * AIOT_MAC_MAX_TBS)

/* ---------------------------------------------------------------------------------------------
 * D2R Scheduling Info (6.2.1.6, Table 6.2.1.6-1)
 * ------------------------------------------------------------------------------------------- */

typedef enum {
  AIOT_SCHED_PAGING_CBRA = 0, /* 18 bits */
  AIOT_SCHED_PAGING_CFA = 1, /* 24 bits */
  AIOT_SCHED_RANDOM_ID_RESPONSE = 2, /* 24 bits */
  AIOT_SCHED_R2D_DATA = 3 /* 19 bits, unicast frequency */
} aiot_sched_variant_t;

/* Width in bits of the Scheduling Info of a variant, or AIOT_EINVAL (SCHED_WIDTH). */
int aiot_mac_sched_width(int variant);

typedef struct {
  aiot_sched_variant_t variant;
  int t_bit_num; /* Bit Duration T_bit = num/den tau, one of 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32, 1/96 */
  int t_bit_den;
  int n_sfs; /* number of R_SFS values: the bitmap bits set, or 1 for the Unicast value */
  int r_sfs[8]; /* ascending, from 1 2 4 8 16 32 64 128 */
  int x; /* Time Resource Indication 1 or 2 (CBRA paging only, else 0) */
  int r_block; /* block repetition number, 1 or 2 */
  int r_code; /* 1 when FEC is used (Channel Coding Indicator codepoint 0) */
  int i_bit; /* interval between midambles in bits, already scaled by S (0: codepoint 0) */
  int l_amble; /* 7 (short) or 31 (long) */
  int i_add; /* additional midamble present */
  int d2r_tbs; /* 1..125 bytes (0 in CBRA paging, where the field is absent) */
} aiot_sched_info_t;

/* 0 if valid, else AIOT_EINVAL (SchedulingInfo.validate). */
int aiot_mac_sched_validate(const aiot_sched_info_t *s);
/* Packs into bits (capacity cap); returns the width or AIOT_EINVAL / AIOT_ESPACE (pack). */
int aiot_mac_sched_pack(const aiot_sched_info_t *s, uint8_t *bits, int cap);
/* n must be exactly the width of the variant (parse). Returns 0 or AIOT_EINVAL. */
int aiot_mac_sched_parse(const uint8_t *bits, int n, aiot_sched_variant_t variant, aiot_sched_info_t *s);
/* The scale factor S of the Interval Bits (1 at 2 tau ... 192 at tau/96), or AIOT_EINVAL. */
int aiot_mac_sched_scale(const aiot_sched_info_t *s);
/* The Interval Bits codepoint of s->i_bit, or AIOT_EINVAL. */
int aiot_mac_sched_i_bit_codepoint(const aiot_sched_info_t *s);

/* The L1 parameters of one D2R transmission (to_d2r_params): r_sfs must be one of s->r_sfs. The
 * checks are those of D2RParams.__post_init__; the whole-chip-sample check stays in
 * aiot_d2r_params_check. */
int aiot_mac_to_d2r_params(const aiot_sched_info_t *s, int r_sfs, aiot_mod_t modulation, aiot_d2r_params_t *out);

/* ---------------------------------------------------------------------------------------------
 * R2D messages (6.2.1)
 * ------------------------------------------------------------------------------------------- */

#define AIOT_MAC_R2D_TYPE_PAGING 1 /* 3-bit R2D Message Type, Table 6.1-1 */
#define AIOT_MAC_R2D_TYPE_ACCESS_TRIGGER 2
#define AIOT_MAC_R2D_TYPE_RANDOM_ID_RESPONSE 3
#define AIOT_MAC_R2D_TYPE_UPPER_LAYER_DATA 4
#define AIOT_MAC_R2D_TYPE_NACK_FEEDBACK 5

typedef enum {
  AIOT_R2D_PAGING_CBRA = 0,
  AIOT_R2D_PAGING_CFA = 1,
  AIOT_R2D_ACCESS_TRIGGER = 2,
  AIOT_R2D_RANDOM_ID_RESPONSE = 3,
  AIOT_R2D_UPPER_LAYER_DATA = 4,
  AIOT_R2D_NACK_FEEDBACK = 5
} aiot_r2d_kind_t;

/* One ID entry of the Random ID Response. */
typedef struct {
  int echoed_random_id; /* 0..65535 */
  int assigned_as_id; /* 0..65535, or -1 when AIPI = 0 */
  int freq_index; /* 0..7 (codepoint into R_SFS 1..128), or -1 when FIPI = 0 */
} aiot_mac_rid_entry_t;

/* A tagged R2D message; the fields used depend on kind. */
typedef struct {
  aiot_r2d_kind_t kind;
  /* all but Access Trigger: R2D TBS in bytes. On pack 0 means "compute it"; always set on parse. */
  int r2d_tbs;
  /* paging CBRA, paging CFA, Random ID Response, R2D data (variant must match kind) */
  aiot_sched_info_t sched;
  /* paging CBRA and CFA: SPPI, and the Security Parameter when SPPI = 1 ("set to 1 in this release") */
  int has_security_param;
  uint8_t security_param[AIOT_MAC_SECURITY_PARAM_BITS]; /* bits 0/1, first = MSB */
  /* paging CBRA */
  int transaction_id; /* 0..63 */
  int num_access_occasions_code; /* 0..15: 2^n access occasions */
  int k_bit; /* 0 -> K = 1, 1 -> K = 4 */
  /* paging CBRA (optional, PIPI) and CFA (always) */
  int has_paging_id;
  int paging_id_len; /* 0..1023 bits (10-bit Paging ID Length) */
  uint8_t paging_id[AIOT_MAC_MAX_PAGING_ID_BITS];
  /* Random ID Response: 1..8 entries, all with or all without freq_index */
  int n_entries;
  aiot_mac_rid_entry_t entries[AIOT_MAC_MAX_ID_ENTRIES];
  /* R2D Upper Layer Data Transfer */
  int as_id; /* 0..65535 */
  int has_sdu; /* CI: 1 with a Data SDU, 0 with a Received Data Size */
  int sdu_len;
  uint8_t sdu[AIOT_MAC_MAX_TBS];
  int received_data_size; /* 0..125, when has_sdu = 0 */
  /* NACK Feedback: 1..61 AS IDs */
  int n_as_ids;
  int as_ids[AIOT_MAC_MAX_NACK_IDS];
} aiot_mac_r2d_msg_t;

/* Packs msg into bits (capacity cap). Returns the number of bits (8 * TBS, 3 for Access Trigger)
 * or AIOT_EINVAL / AIOT_ESPACE. msg is not changed: with r2d_tbs = 0 the TBS is computed. */
int aiot_mac_pack_r2d(const aiot_mac_r2d_msg_t *msg, uint8_t *bits, int cap);
/* Parses one R2D PDU (parse_r2d); msg is zeroed first. Returns 0 or AIOT_EINVAL. */
int aiot_mac_parse_r2d(const uint8_t *bits, int n, aiot_mac_r2d_msg_t *msg);
/* The natural length in bytes of the message without extra Fill Bits (the TBS computed when
 * r2d_tbs is 0), or AIOT_EINVAL if the message is invalid. 0 for Access Trigger (3 bits). */
int aiot_mac_r2d_natural_bytes(const aiot_mac_r2d_msg_t *msg);
/* Bits of the message before any Fill Bits (3 for Access Trigger), or AIOT_EINVAL. */
int aiot_mac_r2d_body_bits(const aiot_mac_r2d_msg_t *msg);

/* ---------------------------------------------------------------------------------------------
 * D2R messages (6.2.2)
 * ------------------------------------------------------------------------------------------- */

typedef enum {
  AIOT_D2R_ACCESS_RANDOM_ID = 0, /* Msg1: the bare 16-bit Random ID */
  AIOT_D2R_UPPER_LAYER_DATA = 1 /* type 00 */
} aiot_d2r_kind_t;

typedef struct {
  aiot_d2r_kind_t kind;
  int random_id; /* Access Random ID: 0..65535 */
  int mdi; /* Upper Layer Data: More Data Indication */
  int sdu_len; /* 0..125 */
  uint8_t sdu[AIOT_MAC_MAX_TBS];
  int padding_bytes; /* MAC Padding, zero bytes after the SDU */
} aiot_mac_d2r_msg_t;

/* Total length 2 + sdu_len + padding_bytes, at most 125. Returns the number of bits or
 * AIOT_EINVAL / AIOT_ESPACE. */
int aiot_mac_pack_d2r(const aiot_mac_d2r_msg_t *msg, uint8_t *bits, int cap);
/* kind says which message the bits are. d2r_tbs (bytes, from the Scheduling Info), when not 0,
 * must equal the length of an Upper Layer Data PDU. Returns 0 or AIOT_EINVAL. */
int aiot_mac_parse_d2r(const uint8_t *bits, int n, aiot_d2r_kind_t kind, int d2r_tbs, aiot_mac_d2r_msg_t *msg);
/* msg = Upper Layer Data padded to exactly tbs bytes (D2RUpperLayerDataTransfer.for_tbs). */
int aiot_mac_d2r_for_tbs(const uint8_t *sdu, int sdu_len, int tbs, int mdi, aiot_mac_d2r_msg_t *msg);

/* ---------------------------------------------------------------------------------------------
 * Field value helpers
 * ------------------------------------------------------------------------------------------- */

/* 1 when the 3-bit R2D Message Type is assigned (1..5), 0 when reserved (Table 6.1-1). */
int aiot_mac_r2d_type_valid(int type3);
/* 2^code access occasions for code 0..15, else AIOT_EINVAL (num_access_occasions). */
int aiot_mac_num_access_occasions(int code);
/* K for the K bit 0 or 1 (1 or 4), else AIOT_EINVAL (k). */
int aiot_mac_k_value(int k_bit);
/* R_SFS of a Frequency Index codepoint 0..7 (1..128), else AIOT_EINVAL (RandomIDEntry.r_sfs). */
int aiot_mac_freq_index_to_r_sfs(int freq_index);

#ifdef __cplusplus
}
#endif

#endif
