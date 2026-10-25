/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_phy.h
 * \brief The Rel-19 Ambient IoT physical layer, 3GPP TS 38.291 V19.3.0, at 5.76 Msps.
 *
 * The library links into OAI programs (aiot_sim) and is also built as libaiot_phy.so, which the
 * Amber A-IoT simulator loads for its signal processing (see openair2/AMBER).
 *
 * Conventions:
 *  - Bits are uint8_t arrays of 0/1, first element = first bit (MSB first).
 *  - Soft values: > 0 means bit 0, < 0 means bit 1.
 *  - Samples are complex doubles stored as cd_t (same layout as C99 double complex).
 *  - Sample counts are at AIOT_SAMPLE_RATE_HZ. Times that can grow over a run are int64_t.
 *  - No globals, no threads, no OAI context. Output buffers are the caller's; a function that
 *    fills one is given its capacity and returns the length written, or a negative AIOT_E* code.
 *  - Receivers take samples with their noise already in them: the caller adds the noise, so a
 *    run draws it from its own seeded rng and a test can hand in exactly the noise it wants.
 */

#ifndef AIOT_PHY_H
#define AIOT_PHY_H

#include <stdint.h>
#include "common/platform_types.h" /* cd_t: complex double IQ */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------
 * Errors
 * ------------------------------------------------------------------------------------------- */

#define AIOT_EINVAL (-1) /* a parameter is not allowed by the spec */
#define AIOT_ESPACE (-2) /* the output buffer is too small */
#define AIOT_ENOMEM (-3) /* malloc failed */

/* ---------------------------------------------------------------------------------------------
 * Sampling and numerology (38.291 §4.3, §6.2.7)
 * ------------------------------------------------------------------------------------------- */

#define AIOT_SAMPLE_RATE_HZ 5760000
#define AIOT_SAMPLES_PER_MS 5760
#define AIOT_SCS_HZ 15000
#define AIOT_N_FFT 384
#define AIOT_CP_LONG 30
#define AIOT_CP_SHORT 27
#define AIOT_HALF_SUBFRAME 2880
#define AIOT_SC_PER_RB 12
#define AIOT_TAU 768 /* tau = 2/15 ms in samples */

#define AIOT_N_SIP 8
#define AIOT_N_CAP 4
#define AIOT_SIP_CHIPS_PER_SYMBOL 4

#define AIOT_MAX_TBS_BYTES 125
#define AIOT_MAX_TB_BITS (8 * AIOT_MAX_TBS_BYTES)

/* The NR cell's time, which A-IoT nodes count in: 15 kHz, 10 slots of 1 ms a frame, the SFN wrapping
 * at 1024 frames (TS 38.211 §4.3.1). */
#define AIOT_CELL_FRAMES 1024
#define AIOT_CELL_SLOTS_PER_FRAME 10
#define AIOT_CELL_WRAP_SLOTS (AIOT_CELL_FRAMES * AIOT_CELL_SLOTS_PER_FRAME)

/* An event to log when it is the 1st, 2nd, 4th, 8th... (n counts it): for what can repeat every slot. */
static inline int aiot_log_due(uint64_t n)
{
  return n && !(n & (n - 1));
}

/* 1 when m is an R2D M_chip^symb (2, 6, 12, 24); the minimum N_RB for it, or AIOT_EINVAL. */
int aiot_m_valid(int m_chip_symb);
int aiot_n_rb_min(int m_chip_symb);

/* The D2R preamble/midamble of length 7 or 31 (§8.3). Returns the length or AIOT_EINVAL. */
int aiot_m_sequence(int length, uint8_t *out);

/* ---------------------------------------------------------------------------------------------
 * D2R parameters (38.291 §7.1.1, 38.391 Table 6.2.1.6-1)
 * ------------------------------------------------------------------------------------------- */

typedef enum { AIOT_OOK = 0, AIOT_BPSK = 1 } aiot_mod_t;

typedef struct {
  int t_bit_num; /* T_bit = t_bit_num / t_bit_den tau: 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32, 1/96 */
  int t_bit_den;
  int r_sfs; /* small frequency shift factor */
  int r_block; /* block repetition, 1 or 2 */
  int r_code; /* 1 when the TBCC is used */
  int l_amble; /* 7 or 31 */
  int i_bit; /* bits between midambles, already scaled by S */
  int i_add; /* 1 for the additional midamble */
  aiot_mod_t modulation;
} aiot_d2r_params_t;

/* The bit durations of 38.391 Table 6.2.1.6-1, in the order of their Bit Duration codepoint, and the
 * Interval Bits codepoints of each: I_bit = S x 48, 96, 168 or 240. */
#define AIOT_N_BIT_DURATIONS 8
#define AIOT_N_I_BIT 4

typedef struct {
  int num, den; /* T_bit = num / den tau */
  int max_r_sfs; /* the largest R_SFS allowed with it */
  int scale; /* S, the scale factor of the Interval Bits */
} aiot_bit_duration_t;

/* The Bit Duration codepoint of T_bit = num / den tau (any equal fraction), or AIOT_EINVAL. */
int aiot_bit_duration_code(int t_bit_num, int t_bit_den);
/* The bit duration of codepoint code, 0 to AIOT_N_BIT_DURATIONS - 1, or NULL. */
const aiot_bit_duration_t *aiot_bit_duration(int code);

/* 0 when the spec allows these values together and a chip is a whole number of samples, else
 * AIOT_EINVAL. */
int aiot_d2r_params_check(const aiot_d2r_params_t *p);
int aiot_bit_samples(const aiot_d2r_params_t *p);
int aiot_chip_samples(const aiot_d2r_params_t *p);
/* The R_SFS values 38.391 allows with this bit duration; returns how many (out has room for 8). */
int aiot_allowed_r_sfs(int t_bit_num, int t_bit_den, int *out);
/* The I_bit values allowed with this bit duration, by Interval Bits codepoint; returns how many (out
 * has room for AIOT_N_I_BIT). */
int aiot_i_bit_values(int t_bit_num, int t_bit_den, int *out);

/* V, the bits on air for an a_bits transport block (§6.1.2, §6.1.3). */
int aiot_frame_bits(int a_bits, const aiot_d2r_params_t *p);
int aiot_frame_samples(int a_bits, const aiot_d2r_params_t *p);

typedef enum { AIOT_MSG1 = 0, AIOT_MSG2 = 1, AIOT_CFA = 2, AIOT_DATA = 3 } aiot_d2r_msg_t;

/* T_R->D in samples (§7.1.2). candidate_r_sfs may be NULL/0 (then p->r_sfs).
 * Returns AIOT_EINVAL when it is not a whole number of samples. */
int aiot_t_r_to_d(const aiot_d2r_params_t *p,
                  int m_chip_symb,
                  aiot_d2r_msg_t msg,
                  int n_tbs_bytes,
                  const int *candidate_r_sfs,
                  int n_candidates,
                  int earlier_occasion,
                  int d2r_chips);
/* Te = ceil(T_SFO / 10) (38.191 §9.2.1: 10% of T_SFO; rounded up to whole samples here);
 * T_D2R_min = max(3 T_chip, 10 us) (38.191 §9.3; 10 us = 57.6 samples, rounded up to 58). */
int aiot_timing_tolerance(int t_sfo_samples);
int aiot_t_d2r_min(const aiot_d2r_params_t *p);

/* ---------------------------------------------------------------------------------------------
 * Coding (38.291 clause 8)
 * ------------------------------------------------------------------------------------------- */

/* The L parity bits (L = 16 or 6) of bits[0..n-1]: zero start, no final XOR. */
int aiot_crc(const uint8_t *bits, int n, int L, uint8_t *parity);
int aiot_crc_length(int a_bits); /* 16 if A > 24 else 6 */
/* out = bits followed by their CRC; returns A + L. out may not alias bits. */
int aiot_attach_crc(const uint8_t *bits, int a_bits, uint8_t *out);
/* Checks a received B = A + L bit word. Returns 1 if the CRC holds, 0 if not, AIOT_EINVAL for
 * B in 31..40 or B < L. *a_bits gets B - L. */
int aiot_check_crc(const uint8_t *word, int b_bits, int *a_bits);

/* Tail-biting convolutional code, rate 1/3, K = 7, generators 133, 171, 165 (octal):
 * e[3k + i] = d^(i)_k. The encoder works on unpacked bits, so OAI's ccodelte packing rule for
 * K mod 8 != 0 does not apply. */
void aiot_tbcc_encode(const uint8_t *c, int k, uint8_t *e);
/* Soft Viterbi with wrap-around decoding: n_soft = 3K values, K bits out.
 * Returns K, or AIOT_EINVAL / AIOT_ENOMEM. */
int aiot_tbcc_decode(const double *soft, int n_soft, uint8_t *out);

/* out = bits repeated r_block times; returns n * r_block. */
int aiot_repeat_block(const uint8_t *bits, int n, int r_block, uint8_t *out);
/* out[k] = sum of the r_block segments of soft; n must be a multiple of r_block. Returns n / r_block. */
int aiot_combine_repetitions(const double *soft, int n, int r_block, double *out);

/* ---------------------------------------------------------------------------------------------
 * R2D: Reader transmitter and device receiver (38.291 §6.2)
 * ------------------------------------------------------------------------------------------- */

/* Manchester line code (§8.5): 0 -> (1, 0), 1 -> (0, 1). Returns 2n. */
int aiot_manchester(const uint8_t *bits, int n, uint8_t *out);
/* All chips of one R2D transmission: SIP, CAP, Manchester(TB + CRC), postamble, padding (§6.2.5). */
/* n bits, one a byte, into (n + 7) / 8 bytes, the first in the top bit of the first byte; and back. */
void aiot_pack_bits(const uint8_t *bits, int n, uint8_t *bytes);
void aiot_unpack_bits(const uint8_t *bytes, int n, uint8_t *bits);
/* Room for the chips of a transport block of a_bits: the body, the forced chips of M = 24 and the padding. */
#define AIOT_R2D_MAX_CHIPS(a_bits) (4 * ((a_bits) + 16) + 64)
int aiot_r2d_chips(const uint8_t *tb_bits, int a_bits, int m_chip_symb, uint8_t *chips, int max_chips);
int aiot_r2d_n_symbols(int n_chips, int m_chip_symb);
/* Samples of n_sym symbols starting with symbol l = 0 (long CP at l mod 7 == 0). */
int aiot_r2d_n_samples(int n_sym);
/* The resource elements of the chips (§6.2.7): for each OFDM symbol l, the K = N_RB x 12 subcarriers
 * q - K/2, q = 0..K-1, at re[l * K + q], lowest subcarrier first. A transmitter that has its own
 * resource grid (a gNB) maps them onto K consecutive subcarriers of its carrier; the subcarrier at
 * q = K/2 is the R2D's centre. Scaled so that the waveform of a symbol is
 * x(i) = sum_q re[q] exp(j 2 pi (q - K/2) i / 384), i = 0..383, before its cyclic prefix: an "on"
 * chip is about 1 in amplitude, an "off" chip about 0. The chips must fill whole symbols, as
 * aiot_r2d_chips() gives them. Returns n_sym x K. */
int aiot_r2d_n_resource_elements(int n_sym, int n_rb);
int aiot_r2d_resource_elements(const uint8_t *chips, int n_chips, int m_chip_symb, int n_rb, cd_t *re, int max_re);
/* Baseband samples of the chips, band limited to N_RB x 12 subcarriers around DC (§6.2.7). */
int aiot_r2d_waveform(const uint8_t *chips, int n_chips, int m_chip_symb, int n_rb, cd_t *out, int max_out);
/* aiot_r2d_chips then aiot_r2d_waveform. */
int aiot_r2d_transmission(const uint8_t *tb_bits, int a_bits, int m_chip_symb, int n_rb, cd_t *out, int max_out);

typedef struct {
  uint8_t bits[AIOT_MAX_TB_BITS + 8]; /* the TB, CRC removed; the receiver accepts up to a byte more than the largest TB */
  int n_bits;
  int64_t end_sample; /* the sample after its last chip, in the receiver's own sample count */
  int m_chip_symb;
} aiot_r2d_frame_t;

typedef struct aiot_r2d_rx aiot_r2d_rx_t;

aiot_r2d_rx_t *aiot_r2d_rx_new(void);
void aiot_r2d_rx_free(aiot_r2d_rx_t *rx);
/* The MAC switches the receiver off while the device may skip R2D (§7.2.3). */
void aiot_r2d_rx_set_enabled(aiot_r2d_rx_t *rx, int enabled);
/* Feed the next n samples (signal plus noise, sqrt(watt)). Writes up to max_frames frames and
 * returns how many, or a negative error. */
int aiot_r2d_rx_receive(aiot_r2d_rx_t *rx, const cd_t *y, int n, aiot_r2d_frame_t *frames, int max_frames);

/* ---------------------------------------------------------------------------------------------
 * D2R: device backscatter and Reader receiver (38.291 §6.1)
 * ------------------------------------------------------------------------------------------- */

/* e_0..e_{E-1}: CRC, repetition, then the TBCC if r_code (§6.1.2). Returns E. */
int aiot_d2r_coded_bits(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, uint8_t *e, int max_e);
/* For each of the V bits on air: -1 for a PDRCH bit, else the index of its amble bit (§6.1.3).
 * Uses only l_amble, i_bit (any positive value) and i_add of p. */
int aiot_d2r_amble_layout(int e_bits, const aiot_d2r_params_t *p, int *lay, int max_v);
/* v_0..v_{V-1}: the PDRCH bits with the preamble and midambles. Returns V. */
int aiot_d2r_assemble(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, uint8_t *v, int max_v);
/* 2 R_SFS chips per bit: OOK 0/1, BPSK -1/+1 (§8.4). Returns 2 R_SFS n. */
int aiot_d2r_sfs_chips(const uint8_t *v, int n, const aiot_d2r_params_t *p, double *chips, int max_chips);
/* The reflection coefficient, one value per sample. Returns the frame's sample count. */
int aiot_d2r_waveform(const uint8_t *tb_bits, int a_bits, const aiot_d2r_params_t *p, double *out, int max_out);

#define AIOT_MAX_SHIFTS 8
#define AIOT_MAX_SIC_ROUNDS 16

/* A D2R the Reader scheduled. The receiver searches start +- te for a preamble on
 * each listed R_SFS (none listed: params.r_sfs). */
typedef struct {
  int64_t start;
  int te;
  int a_bits;
  aiot_d2r_params_t params;
  int r_sfs[AIOT_MAX_SHIFTS];
  int n_r_sfs;
  int64_t tag; /* the caller's own handle, returned as is */
} aiot_d2r_expect_t;

typedef struct {
  uint8_t bits[AIOT_MAX_TB_BITS];
  int n_bits;
  int r_sfs;
  int64_t start;
  double rssi_w;
  int64_t tag;
} aiot_d2r_result_t;

typedef struct {
  aiot_d2r_expect_t expect;
  aiot_d2r_result_t decoded[AIOT_MAX_SHIFTS * AIOT_MAX_SIC_ROUNDS];
  int n_decoded;
  int collided; /* preambles found whose frame did not decode */
} aiot_d2r_outcome_t;

typedef struct aiot_d2r_rx aiot_d2r_rx_t;

aiot_d2r_rx_t *aiot_d2r_rx_new(double noise_w, int enable_sic);
void aiot_d2r_rx_free(aiot_d2r_rx_t *rx);
/* 0 when the receiver can open window e (its length, and each R_SFS's parameters), else AIOT_EINVAL. */
int aiot_d2r_expect_check(const aiot_d2r_expect_t *e);
int aiot_d2r_rx_expect(aiot_d2r_rx_t *rx, const aiot_d2r_expect_t *e);
/* Feed the next n samples (signal plus noise). Decodes every expected D2R whose window has fully
 * arrived, in the order they were expected. Returns how many outcomes it
 * wrote (at most max_out), or a negative error. noise_w is only used for the detection threshold. */
int aiot_d2r_rx_receive(aiot_d2r_rx_t *rx, const cd_t *y, int n, aiot_d2r_outcome_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* AIOT_PHY_H */
