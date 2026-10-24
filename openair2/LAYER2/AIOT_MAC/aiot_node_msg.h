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
#define AIOT_NODE_MSG_MAX_BYTES (AIOT_TAG_DATA_FIXED_BYTES + AIOT_MAC_MAX_TBS)

typedef enum { AIOT_NODE_MSG_TAG_DATA = 1 } aiot_node_msg_type_t;

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

#ifdef __cplusplus
}
#endif

#endif
