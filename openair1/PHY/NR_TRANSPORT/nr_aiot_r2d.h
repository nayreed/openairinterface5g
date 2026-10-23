/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_r2d.h
 * \brief An NR node as an Ambient IoT Reader: R2D transmissions (3GPP TS 38.291 §6.2) on PRBs of the
 * node's own grid, a gNB's downlink or a UE's uplink.
 *
 * An R2D is OOK chips as DFT-s-OFDM on N_RB PRBs at 15 kHz (§6.2.7). Its symbols align with the NR
 * symbols (§4.3.3.2) and it starts on a half-subframe, so its first symbol has the long CP: symbol 0
 * or 7 of a slot at 15 kHz. A transmission is scheduled ahead of time and written, symbol by symbol,
 * into the slots it spans, after the NR channels of each slot have been written. Its PRBs must be in
 * the gNB's PRB blacklist (gNBs.[0].dl_prbblacklist for a gNB's R2D, ul_prbblacklist for a UE's) and
 * away from the SSB, the CORESETs, PUCCH and PRACH, so that no NR channel uses them; a resource element
 * that is not empty when the R2D comes is counted as a collision and logged.
 *
 * The functions take the grid's sizes and buffers, not the gNB, so they run in unit tests. Only
 * numerology 0 (15 kHz) is supported: A-IoT has no other.
 */

#ifndef NR_AIOT_R2D_H
#define NR_AIOT_R2D_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "common/platform_types.h" /* c16_t, cd_t */

#ifdef __cplusplus
extern "C" {
#endif

#define NR_AIOT_R2D_MAX_PENDING 8
#define NR_AIOT_R2D_SYMBOLS_PER_SLOT 14 /* 15 kHz, normal cyclic prefix */
/* An R2D's subcarriers: at most 3/4 of the 384-point symbol of the A-IoT rate, 5.76 Msps */
#define NR_AIOT_R2D_MAX_SC 288
/* A TDD pattern repeats within 20 ms (TS 38.331 TDD-UL-DL-ConfigCommon, both patterns): 20 slots at 15 kHz */
#define NR_AIOT_R2D_TX_SLOT_PERIOD 20
#define NR_AIOT_R2D_ETOOLATE (-10) /* the start slot has been mapped already, or is being mapped */

/* The R2D resources of one cell. */
typedef struct {
  int prb_start; /* first PRB of the R2D, counted from point A; -1: the gNB sends no R2D */
  int n_rb_max; /* PRBs reserved for the R2D, from prb_start; 0: the gNB sends no R2D */
  double power_offset_db; /* R2D EPRE over the EPRE of the node's data channel (PDSCH, or PUSCH for a UE) */
  int carrier; /* 1: the gNB also sends the carrier wave the devices reflect (see nr_aiot_r2d_map) */
} nr_aiot_r2d_config_t;

typedef struct {
  int start_slot; /* absolute slot, frame * slots_per_frame + slot */
  int start_symbol; /* 0 or 7 */
  int n_sym;
  int prb_start;
  int n_rb;
  c16_t *re; /* n_sym x n_rb x 12 */
  int next_slot; /* slots of it mapped so far */
  int last_mapped; /* absolute slot mapped last, -1: none (a slot mapped again is left as it is) */
} nr_aiot_r2d_tx_t;

/* The R2D of one slot, taken from the transmissions in slot order (nr_aiot_r2d_take()) and written into
 * the slot's grid later (nr_aiot_r2d_put()). */
typedef struct {
  int frame, slot;
  int prb_start; /* first PRB of every R2D symbol */
  int carrier; /* 1: the symbols without R2D get the carrier wave */
  uint8_t n_rb[NR_AIOT_R2D_SYMBOLS_PER_SLOT]; /* PRBs of the R2D in each symbol; 0: no R2D */
  c16_t re[NR_AIOT_R2D_SYMBOLS_PER_SLOT][NR_AIOT_R2D_MAX_SC]; /* their resource elements, unrotated */
} nr_aiot_r2d_slot_t;

typedef struct {
  nr_aiot_r2d_config_t cfg;
  int slots_per_frame;
  int symbols_per_slot;
  int ofdm_symbol_size;
  int n_prb; /* PRBs of the carrier */
  double on_amplitude; /* the amplitude of an "on" chip, in the grid's units */
  int carrier_sc; /* the carrier's subcarrier: the centre of the reserved PRBs */
  c16_t carrier_re[NR_AIOT_R2D_SYMBOLS_PER_SLOT]; /* its resource element in each symbol of a slot, phase-continuous */
  pthread_mutex_t lock; /* scheduling and mapping run in different threads */
  uint32_t tx_slots; /* the slots the node sends in, see nr_aiot_r2d_set_tx_slots() */
  nr_aiot_r2d_tx_t pending[NR_AIOT_R2D_MAX_PENDING];
  int n_pending;
  int now_slot; /* absolute slot mapped last, -1: none yet */
  int64_t now_ts; /* the radio timestamp of now_slot's first sample, -1: not given */
  int timed_slot; /* the slot nr_aiot_r2d_set_slot_time() gave the time of last, -1: none */
  int64_t timed_ts;
  uint64_t sent, dropped; /* transmissions sent in full; dropped because one of their slots was missed */
  uint64_t collisions; /* resource elements of NR channels the R2D overwrote */
  uint64_t carrier_blocked; /* symbols where an NR channel held the carrier's resource element */
} nr_aiot_r2d_t;

/* Checks cfg against the carrier and prepares r2d. tx_amp is the amplitude of the node's data channel:
 * a QPSK resource element of a gNB's PDSCH (TX_AMP) or a UE's PUSCH (AMP) is tx_amp / sqrt(2) in
 * magnitude. The R2D is scaled so that, with half its chips on, its EPRE is that data channel's EPRE
 * plus cfg->power_offset_db. Returns 0, or -1 with the reason logged. */
int nr_aiot_r2d_init(nr_aiot_r2d_t *r2d,
                     const nr_aiot_r2d_config_t *cfg,
                     int slots_per_frame,
                     int symbols_per_slot,
                     int ofdm_symbol_size,
                     int n_prb,
                     int tx_amp);
void nr_aiot_r2d_free(nr_aiot_r2d_t *r2d);
/* The slots the node sends in, in every symbol (downlink slots for a gNB, uplink slots for a UE): bit
 * (frame % 2) * 10 + slot of tx_slots. schedule() refuses an R2D that spans any other slot (for a gNB, a
 * TDD uplink or mixed slot), since the node would send it only in part. Every slot, unless set. */
void nr_aiot_r2d_set_tx_slots(nr_aiot_r2d_t *r2d, uint32_t tx_slots);

/* Schedules the R2D of the transport block tb (a_bits bits, 1 to 1000) with M = m_chip_symb chips
 * per symbol on n_rb PRBs (aiot_n_rb_min(M) to the configured n_rb_max), starting at symbol 0 or 7
 * of (frame, slot). Must be called before that slot is mapped, and must not overlap a transmission
 * already scheduled. Returns the number of OFDM symbols of the transmission, or a negative AIOT_E*
 * code (AIOT_EINVAL for a bad argument or an overlap, AIOT_ESPACE when too many are pending), or
 * NR_AIOT_R2D_ETOOLATE when (frame, slot) is not after the slot mapped last. */
int nr_aiot_r2d_schedule(nr_aiot_r2d_t *r2d,
                         int frame,
                         int slot,
                         int symbol,
                         const uint8_t *tb,
                         int a_bits,
                         int m_chip_symb,
                         int n_rb);

/* The node's clock: ts is the radio timestamp (the node's own sample count) of the first sample of
 * (frame, slot), the slot the node maps next. The Reader plans each R2D, and the D2R windows it opens,
 * from the time of the slot mapped last, so a node calls this before it maps each slot. */
void nr_aiot_r2d_set_slot_time(nr_aiot_r2d_t *r2d, int frame, int slot, int64_t ts);

/* The absolute slot (frame * slots_per_frame + slot) the node mapped last, or -1 before the first.
 * *ts gets the radio timestamp of its first sample, or -1 when the node did not give it. */
int nr_aiot_r2d_now(nr_aiot_r2d_t *r2d, int64_t *ts);

/* Writes the R2D symbols that fall in (frame, slot) into txdataF, the slot's grid of the first
 * antenna (symbols_per_slot x ofdm_symbol_size, the carrier's subcarriers from index 0 at point A).
 * rotation, if not NULL, is the slot's per-symbol phase rotation (symbols_per_slot values, Q15), the
 * same as the NR channels get. symbol_used, if not NULL, gets true for each symbol written
 * (symbols_per_slot values), for a node that modulates only the symbols it uses. A transmission that
 * misses one of its slots (scheduled too late, or a slot the node did not send) is dropped. Returns the
 * number of R2D symbols written.
 *
 * With cfg.carrier, every symbol without R2D gets the carrier wave: one resource element at the centre
 * of the reserved PRBs (subcarrier 12 prb_start + 6 n_rb_max), as strong as an "on" chip of an R2D on
 * all of them, its phase advanced over each cyclic prefix so that the tone runs on unbroken from symbol
 * to symbol and slot to slot (it gets no rotation). The R2D is then the carrier keyed by its chips, as
 * from a standalone Reader, and the devices reflect the carrier for their D2R.
 *
 * map() is take() and then put(), for a node that maps its slots one by one in order (a gNB). */
int nr_aiot_r2d_map(nr_aiot_r2d_t *r2d, int frame, int slot, c16_t *txdataF, const c16_t *rotation, bool *symbol_used);

/* The first half of map(), for a node that builds its slots in parallel (a UE's TX threads): takes the
 * R2D symbols of (frame, slot) into out, and moves the transmissions on. The slots must be taken one by
 * one in order, every slot the node sends; one not taken counts as not sent. Returns the number of R2D
 * symbols taken. */
int nr_aiot_r2d_take(nr_aiot_r2d_t *r2d, int frame, int slot, nr_aiot_r2d_slot_t *out);

/* The second half of map(): writes the slot in taken into txdataF, as map() does. Slots can be put in
 * any order, from any thread. Returns the number of R2D symbols written. */
int nr_aiot_r2d_put(nr_aiot_r2d_t *r2d, const nr_aiot_r2d_slot_t *in, c16_t *txdataF, const c16_t *rotation, bool *symbol_used);

#ifdef __cplusplus
}
#endif

#endif
