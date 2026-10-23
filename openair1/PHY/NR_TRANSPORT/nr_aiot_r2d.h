/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_r2d.h
 * \brief The gNB as an Ambient IoT Reader: R2D transmissions (3GPP TS 38.291 §6.2) on PRBs of the gNB's
 * own downlink grid.
 *
 * An R2D is OOK chips as DFT-s-OFDM on N_RB PRBs at 15 kHz (§6.2.7). Its symbols align with the NR
 * symbols (§4.3.3.2) and it starts on a half-subframe, so its first symbol has the long CP: symbol 0
 * or 7 of a slot at 15 kHz. A transmission is scheduled ahead of time and written, symbol by symbol,
 * into the slots it spans, after the NR channels of each slot have been written. Its PRBs must be in
 * the DL PRB blacklist (gNBs.[0].dl_prbblacklist) and away from the SSB and the CORESETs, so that no
 * NR channel uses them; a resource element that is not empty when the R2D comes is counted as a
 * collision and logged.
 *
 * The functions take the grid's sizes and buffers, not the gNB, so they run in unit tests. Only
 * numerology 0 (15 kHz) is supported: A-IoT has no other.
 */

#ifndef NR_AIOT_R2D_H
#define NR_AIOT_R2D_H

#include <pthread.h>
#include <stdint.h>
#include "common/platform_types.h" /* c16_t, cd_t */

#ifdef __cplusplus
extern "C" {
#endif

#define NR_AIOT_R2D_MAX_PENDING 8
/* A TDD pattern repeats within 20 ms (TS 38.331 TDD-UL-DL-ConfigCommon, both patterns): 20 slots at 15 kHz */
#define NR_AIOT_R2D_TX_SLOT_PERIOD 20
#define NR_AIOT_R2D_ETOOLATE (-10) /* the start slot has been mapped already, or is being mapped */

/* The R2D resources of one cell. */
typedef struct {
  int prb_start; /* first PRB of the R2D, counted from point A; -1: the gNB sends no R2D */
  int n_rb_max; /* PRBs reserved for the R2D, from prb_start; 0: the gNB sends no R2D */
  double power_offset_db; /* R2D EPRE over PDSCH EPRE */
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
  int collisions;
} nr_aiot_r2d_tx_t;

typedef struct {
  nr_aiot_r2d_config_t cfg;
  int slots_per_frame;
  int symbols_per_slot;
  int ofdm_symbol_size;
  int n_prb; /* PRBs of the carrier */
  double on_amplitude; /* the amplitude of an "on" chip, in the grid's units */
  int carrier_sc; /* the carrier's subcarrier: the centre of the reserved PRBs */
  c16_t carrier_re[14]; /* its resource element in each symbol of a slot, phase-continuous */
  pthread_mutex_t lock; /* scheduling and mapping run in different threads */
  uint32_t tx_slots; /* the slots the node sends in, see nr_aiot_r2d_set_tx_slots() */
  nr_aiot_r2d_tx_t pending[NR_AIOT_R2D_MAX_PENDING];
  int n_pending;
  int now_slot; /* absolute slot mapped last, -1: none yet */
  uint64_t sent, dropped; /* transmissions sent in full; dropped because one of their slots was missed */
  uint64_t collisions; /* resource elements of NR channels the R2D overwrote */
  uint64_t carrier_blocked; /* symbols where an NR channel held the carrier's resource element */
} nr_aiot_r2d_t;

/* Checks cfg against the carrier and prepares r2d. tx_amp is the gNB's TX_AMP: a PDSCH QPSK resource
 * element is TX_AMP / sqrt(2) in magnitude. The R2D is scaled so that, with half its chips on, its
 * EPRE is the PDSCH EPRE plus cfg->power_offset_db. Returns 0, or -1 with the reason logged. */
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

/* The absolute slot (frame * slots_per_frame + slot) the gNB mapped last, or -1 before the first. */
int nr_aiot_r2d_now(nr_aiot_r2d_t *r2d);

/* Writes the R2D symbols that fall in (frame, slot) into txdataF, the slot's grid of the first
 * antenna (symbols_per_slot x ofdm_symbol_size, the carrier's subcarriers from index 0 at point A).
 * rotation, if not NULL, is the slot's per-symbol phase rotation (symbols_per_slot values, Q15), the
 * same as the NR channels get. A transmission that misses one of its slots (scheduled too late, or a
 * slot without DL) is dropped. Returns the number of R2D symbols written.
 *
 * With cfg.carrier, every symbol without R2D gets the carrier wave: one resource element at the centre
 * of the reserved PRBs (subcarrier 12 prb_start + 6 n_rb_max), as strong as an "on" chip of an R2D on
 * all of them, its phase advanced over each cyclic prefix so that the tone runs on unbroken from symbol
 * to symbol and slot to slot (it gets no rotation). The R2D is then the carrier keyed by its chips, as
 * from a standalone Reader, and the devices reflect the carrier for their D2R. */
int nr_aiot_r2d_map(nr_aiot_r2d_t *r2d, int frame, int slot, c16_t *txdataF, const c16_t *rotation);

#ifdef __cplusplus
}
#endif

#endif
