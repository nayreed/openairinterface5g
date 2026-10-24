/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_node_msg.h
 * \brief The messages the nodes of an Ambient IoT network send each other over the 5G user plane: one
 * encoder and decoder, used by every node.
 *
 * Tag data: a D2R message the Reader heard from a Tag, with what the Reader's MAC knew of it. A UE that
 * reads the Tags sends one per message to the network (topology 2, aiot.data_dest).
 *
 * A split Reader (topology 3): the gNB runs the Reader's MAC, and a UE sends the R2D in its UL (the
 * gNB's aiot.remote_r2d) or receives the D2R in its DL (aiot.remote_d2r). The gNB sends the UE an R2D
 * to send, which the UE answers with R2D done, or a D2R window to open, which the UE answers with the
 * window's outcome once it has received it. Both nodes count time in the cell's frames and slots,
 * which the UE's own slots follow: an R2D goes in the UE's UL slot of that number, and a window opens
 * at that A-IoT sample of the UE's DL slot of that number.
 *
 * A message is one datagram. Integers are big-endian, a double is its IEEE 754 binary64 bits:
 *
 *   byte  0      version, AIOT_NODE_MSG_VERSION
 *   byte  1      type, aiot_node_msg_type_t
 *   bytes 2-3    length of the whole message, in bytes
 *
 * then, for AIOT_NODE_MSG_TAG_DATA:
 *
 *   bytes 4-7    seq, uint32
 *   bytes 8-15   answer.start, int64: A-IoT samples (5.76 Msps) of the Reader's D2R receiver
 *   bytes 16-23  answer.rssi_w, double
 *   byte  24     after, the aiot_r2d_kind_t of the R2D the Tag answered
 *   byte  25     answer.kind, aiot_answer_kind_t
 *   byte  26     answer.r_sfs
 *   byte  27     answer.message.mdi
 *   bytes 28-29  answer.slot, int16
 *   bytes 30-31  answer.entry, int16, -1: none
 *   bytes 32-35  answer.as_id, int32, -1: none
 *   bytes 36-37  answer.message.random_id, uint16
 *   byte  38     answer.message.padding_bytes
 *   byte  39     answer.message.sdu_len, then the SDU
 *
 * answer.message.kind is not sent: it follows from answer.kind.
 *
 * AIOT_NODE_MSG_R2D, from the gNB:
 *
 *   bytes 4-7    seq, uint32
 *   bytes 8-9    frame, 0-1023
 *   byte  10     slot, 0-9
 *   byte  11     symbol, 0 or 7
 *   byte  12     m_chip_symb
 *   byte  13     n_rb
 *   bytes 14-15  a_bits, 1 to AIOT_MAX_TB_BITS, then the bits, 8 a byte, the first in the top bit
 *
 * AIOT_NODE_MSG_R2D_DONE, from the UE:
 *
 *   bytes 4-7    seq, the R2D's
 *   bytes 8-9    result, int16: what nr_aiot_r2d_schedule() returned (symbols, or a negative code)
 *   bytes 10-11  ahead, int16: the R2D's first slot less the slot the UE had sent last
 *
 * AIOT_NODE_MSG_WINDOW, from the gNB:
 *
 *   bytes 4-11   expect.tag, int64
 *   bytes 12-15  expect.te, int32
 *   bytes 16-17  frame, 0-1023
 *   bytes 18-19  sample, 0-5759: A-IoT samples into the slot, where the window starts
 *   byte  20     slot, 0-9
 *   byte  21     expect.n_r_sfs
 *   bytes 22-23  expect.a_bits
 *   bytes 24-25  expect.params.i_bit
 *   bytes 26-33  expect.params: t_bit_num, t_bit_den, r_sfs, r_block, r_code, l_amble, i_add, modulation
 *   then         expect.r_sfs, a byte each
 *
 * AIOT_NODE_MSG_OUTCOME, from the UE:
 *
 *   bytes 4-11   tag, the window's
 *   bytes 12-15  ahead, int32: the window's first A-IoT sample less the first one the UE had not yet
 *                received, when the window came (negative: it came late)
 *   bytes 16-17  collided
 *   bytes 18-19  n_decoded, then each decoded D2R:
 *     bytes 0-3    start, int32: A-IoT samples from the window's start
 *     bytes 4-11   rssi_w, double
 *     bytes 12-13  n_bits, then r_sfs (a byte) and the bits, packed as an R2D's
 */

#ifndef AIOT_NODE_MSG_H
#define AIOT_NODE_MSG_H

#include <stdint.h>
#include "aiot_mac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOT_NODE_MSG_VERSION 1
#define AIOT_TAG_DATA_FIXED_BYTES 40 /* a Tag data message without its SDU */
#define AIOT_NODE_MSG_MAX_BYTES (AIOT_TAG_DATA_FIXED_BYTES + AIOT_MAC_MAX_TBS) /* Tag data */
#define AIOT_NODE_R2D_FIXED_BYTES 16 /* an R2D without its bits */
#define AIOT_NODE_R2D_DONE_BYTES 12
#define AIOT_NODE_WINDOW_FIXED_BYTES 34 /* a window without its R_SFS list */
#define AIOT_NODE_OUTCOME_FIXED_BYTES 20 /* an outcome without its D2Rs */
#define AIOT_NODE_D2R_FIXED_BYTES 15 /* a decoded D2R of an outcome, without its bits */
#define AIOT_NODE_SPLIT_MAX_BYTES /* the longest split message: an outcome with every D2R the receiver can decode */ \
  (AIOT_NODE_OUTCOME_FIXED_BYTES + AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS * (AIOT_NODE_D2R_FIXED_BYTES + AIOT_MAX_TB_BITS / 8))

typedef enum {
  AIOT_NODE_MSG_TAG_DATA = 1,
  AIOT_NODE_MSG_R2D = 2,
  AIOT_NODE_MSG_R2D_DONE = 3,
  AIOT_NODE_MSG_WINDOW = 4,
  AIOT_NODE_MSG_OUTCOME = 5,
} aiot_node_msg_type_t;

typedef struct {
  uint32_t seq; /* the sender's count of its Tag data messages before this one: a lost one leaves a gap */
  aiot_r2d_kind_t after; /* the R2D message the Tag answered */
  aiot_reader_answer_t answer; /* as the Reader's MAC gave it (aiot_reader_pop_heard) */
} aiot_tag_data_t;

/* The type of the message in buf, or AIOT_EINVAL when buf is not one whole message of this version. */
int aiot_node_msg_type(const uint8_t *buf, int len);

/* Writes m into buf (room for cap bytes). Returns the message's length, AIOT_ESPACE, or AIOT_EINVAL for
 * a field the layout cannot hold. */
int aiot_node_msg_pack_tag_data(const aiot_tag_data_t *m, uint8_t *buf, int cap);
/* The Tag data message in buf. Returns 0, or AIOT_EINVAL when buf is not one. */
int aiot_node_msg_parse_tag_data(const uint8_t *buf, int len, aiot_tag_data_t *m);

/* A time of the cell: an A-IoT sample of a slot at 15 kHz (1 ms, 10 slots a frame). */
typedef struct {
  int frame; /* 0-1023 */
  int slot; /* 0-9 */
  int sample; /* 0-5759; an R2D's time is its first symbol's: sample 0 or AIOT_HALF_SUBFRAME */
} aiot_cell_time_t;

/* An R2D the gNB asks a UE to send, as nr_aiot_r2d_schedule() takes it. */
typedef struct {
  uint32_t seq; /* the gNB's count of its R2Ds before this one */
  int frame, slot, symbol;
  int m_chip_symb;
  int n_rb;
  int a_bits;
  uint8_t bits[AIOT_MAX_TB_BITS]; /* one a byte */
} aiot_node_r2d_t;

typedef struct {
  uint32_t seq;
  int result;
  int ahead;
} aiot_node_r2d_done_t;

/* A D2R window the gNB asks a UE to open. */
typedef struct {
  aiot_cell_time_t at; /* where it starts */
  aiot_d2r_expect_t expect; /* start is not sent: at stands for it */
} aiot_node_window_t;

/* A window's outcome, as the UE sends it back: outcome.expect has only its tag, and each decoded
 * D2R's start counts from the window's start. */
typedef struct {
  int ahead;
  aiot_d2r_outcome_t outcome;
} aiot_node_outcome_t;

/* Each pack_ writes m into buf (room for cap bytes) and returns the message's length, AIOT_ESPACE, or
 * AIOT_EINVAL for a field the layout cannot hold. Each parse_ returns 0, or AIOT_EINVAL when buf is
 * not one message of its type. */
int aiot_node_msg_pack_r2d(const aiot_node_r2d_t *m, uint8_t *buf, int cap);
int aiot_node_msg_parse_r2d(const uint8_t *buf, int len, aiot_node_r2d_t *m);
int aiot_node_msg_pack_r2d_done(const aiot_node_r2d_done_t *m, uint8_t *buf, int cap);
int aiot_node_msg_parse_r2d_done(const uint8_t *buf, int len, aiot_node_r2d_done_t *m);
int aiot_node_msg_pack_window(const aiot_node_window_t *m, uint8_t *buf, int cap);
int aiot_node_msg_parse_window(const uint8_t *buf, int len, aiot_node_window_t *m);
int aiot_node_msg_pack_outcome(const aiot_node_outcome_t *m, uint8_t *buf, int cap);
int aiot_node_msg_parse_outcome(const uint8_t *buf, int len, aiot_node_outcome_t *m);

#ifdef __cplusplus
}
#endif

#endif
