/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_d2r.h
 * \brief An NR node as an Ambient IoT Reader: receiving D2R (3GPP TS 38.291 §6.1) in PRBs of the
 * band the node receives, a gNB's uplink or a UE's downlink.
 *
 * A D2R is not OFDM: it is a device's backscatter of a carrier wave (CW), OOK or BPSK chips shifted
 * by R_SFS. The CW sits at the centre of the D2R PRBs, so that is where the D2R arrives. The node
 * takes the time-domain samples its radio delivers, shifts the centre of those PRBs to 0 Hz, brings
 * them to the A-IoT rate of 5.76 Msps, and runs the A-IoT D2R receiver (aiot_d2r_rx) over each
 * window the A-IoT MAC opened. PRBs of the carrier must be in the gNB's PRB blacklist
 * (gNBs.[0].ul_prbblacklist for a gNB's D2R, dl_prbblacklist for a UE's), so that no NR channel uses
 * them. The guard band beyond the carrier's PRBs holds no NR channel; the RFsimulator delivers it
 * whole, while a real radio's filter may not.
 *
 * The clock is the radio's sample clock: sample t of the A-IoT clock is radio sample t * D, where
 * D = (radio rate) / 5.76 Msps. A window (aiot_d2r_expect_t.start) and every decoded D2R
 * (aiot_d2r_result_t.start) are on this clock. The radio rate must be 5.76 Msps times a whole
 * number, as with three-quarter sampling (-E) at 15 kHz.
 *
 * Samples are ADC counts. The receiver's noise floor is set in counts; a decoded D2R's power
 * (rssi_w) is in counts squared, so 10 log10(rssi_w / 32767^2) is its level in dBFS.
 *
 * Everything but nr_aiot_d2r_feed() may be called from any thread. With a worker thread
 * (nr_aiot_d2r_start), nr_aiot_d2r_push() copies a read's samples and returns at once, and the worker
 * decodes; without one, nr_aiot_d2r_feed() decodes in the caller's thread (as the unit tests do).
 */

#ifndef NR_AIOT_D2R_H
#define NR_AIOT_D2R_H

#include <stdbool.h>
#include <stdint.h>
#include "common/platform_types.h" /* c16_t */
#include "PHY/AIOT/aiot_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The D2R resources of one cell. */
typedef struct {
  /* first PRB of the D2R, counted from point A; -1: the node receives no D2R. PRBs past the carrier's
   * are its guard band in the radio's samples: sample rate / 180 kHz PRBs in all, the last ones above
   * the carrier and wrapping round below it (at 5.76 Msps, 25 PRBs and 7 of guard band) */
  int prb_start;
  int n_rb; /* PRBs reserved for the D2R, from prb_start; the CW and the D2R are at their centre */
  double noise_counts; /* the receiver's noise floor, rms on I and on Q, in ADC counts */
  int enable_sic; /* 1: cancel each decoded D2R and look again (successive interference cancellation) */
} nr_aiot_d2r_config_t;

typedef struct nr_aiot_d2r nr_aiot_d2r_t;

typedef struct {
  uint64_t windows; /* windows decoded (an outcome each) */
  uint64_t decoded; /* D2Rs decoded */
  uint64_t collided; /* preambles found whose frame did not decode */
  uint64_t late; /* windows opened after the receiver had already passed their first sample */
  uint64_t zero_filled; /* A-IoT samples the radio did not deliver, received as zeros */
  uint64_t overruns; /* slots the worker had no room for (their samples are received as zeros) */
} nr_aiot_d2r_stats_t;

/* Checks cfg against the carrier (n_prb PRBs at scs_hz, which must be 15 kHz, sampled at
 * sample_rate_hz) and makes the receiver. Returns NULL with the reason logged when the configuration
 * does not fit. */
nr_aiot_d2r_t *nr_aiot_d2r_new(const nr_aiot_d2r_config_t *cfg, int n_prb, int scs_hz, int64_t sample_rate_hz);
/* Stops the worker, if any, and frees everything. */
void nr_aiot_d2r_free(nr_aiot_d2r_t *d);

/* Where the D2R is, in Hz from the carrier's centre (the DC of the samples): subcarrier
 * 12 prb_start + 6 n_rb of the carrier, whose subcarrier k is (k - 6 n_prb) 15 kHz from the DC. */
double nr_aiot_d2r_shift_hz(const nr_aiot_d2r_t *d);
/* D: radio samples per A-IoT sample. */
int nr_aiot_d2r_decimation(const nr_aiot_d2r_t *d);

/* Opens a window. Must come before the radio delivers the window's first sample; a later one is
 * still decoded on what is left, and counted in stats.late. Returns 0 or a negative AIOT_E* code. */
int nr_aiot_d2r_expect(nr_aiot_d2r_t *d, const aiot_d2r_expect_t *e);

/* Receives n radio samples, the first at radio timestamp ts. Slots must come in time order; a gap
 * is received as zeros, an overlap is skipped. Decodes every window that has fully arrived; its
 * outcome waits for nr_aiot_d2r_pop(). Returns 0 or a negative AIOT_E* code. Not with a worker. */
int nr_aiot_d2r_feed(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n);

/* Starts the worker thread, with room for max_slots slots of up to max_slot_samples samples.
 * wait_when_full: 1 for a simulated radio, whose time waits for its slowest node: push() then waits
 * for room, and no slot is lost. 0 for a real radio, which must not be held up: push() drops. */
int nr_aiot_d2r_start(nr_aiot_d2r_t *d, int max_slots, int max_slot_samples, int wait_when_full);
/* new() and start(), as a node does: the worker has room for 100 ms of slots of slot_samples
 * samples. band names the PRBs in the log ("UL" for a gNB, "DL" for a UE). Returns NULL with the
 * reason logged. */
nr_aiot_d2r_t *nr_aiot_d2r_open(const nr_aiot_d2r_config_t *cfg,
                                int n_prb,
                                int scs_hz,
                                int64_t sample_rate_hz,
                                int slot_samples,
                                bool wait_when_full,
                                const char *band);
/* Hands n radio samples, the first at radio timestamp ts, to the worker: copies them and returns. A
 * read longer than max_slot_samples takes several of the worker's slots. Returns 0, or AIOT_ESPACE
 * when the worker is max_slots behind and does not wait (what was not handed over is then received
 * as zeros). */
int nr_aiot_d2r_push(nr_aiot_d2r_t *d, int64_t ts, const c16_t *x, int n);
/* Waits until the worker has received every slot pushed so far. */
void nr_aiot_d2r_drain(nr_aiot_d2r_t *d);

/* Takes up to max decoded windows, oldest first. Returns how many. */
int nr_aiot_d2r_pop(nr_aiot_d2r_t *d, aiot_d2r_outcome_t *out, int max);
/* The first A-IoT sample not yet received: windows decode once it passes their end. */
int64_t nr_aiot_d2r_received_until(nr_aiot_d2r_t *d);
/* Waits until the receiver has passed A-IoT sample after, or timeout_ms (wall clock) have gone by.
 * Returns nr_aiot_d2r_received_until(). */
int64_t nr_aiot_d2r_wait(nr_aiot_d2r_t *d, int64_t after, int timeout_ms);

/* The node's receive clock, for a Reader in another node that opens windows in cell time (a split
 * Reader): ts is the radio timestamp of the first sample of (frame, slot) as the node receives it (a
 * UE's DL slot). The node gives it for each slot it reads. */
void nr_aiot_d2r_set_slot_time(nr_aiot_d2r_t *d, int frame, int slot, int64_t ts);
/* The A-IoT sample of this receiver's clock that is A-IoT sample `sample` (from 0, 5760 a slot) of
 * (frame, slot) of the cell, counted on from the slot given last at 1 ms a slot (15 kHz, 10 slots a
 * frame), the nearer way round the 1024 frames. -1 before the first slot time. */
int64_t nr_aiot_d2r_cell_time(nr_aiot_d2r_t *d, int frame, int slot, int sample);
nr_aiot_d2r_stats_t nr_aiot_d2r_stats(nr_aiot_d2r_t *d);

#ifdef __cplusplus
}
#endif

#endif
