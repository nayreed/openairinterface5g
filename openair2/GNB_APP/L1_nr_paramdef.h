/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief definition of configuration parameters for all eNodeB modules
 */

#ifndef __GNB_APP_L1_NR_PARAMDEF__H__
#define __GNB_APP_L1_NR_PARAMDEF__H__

/*-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------*/


/* L1 configuration parameters names   */
#define L1_LOCAL_N_ADDRESS                   "local_n_address"
#define L1_REMOTE_N_ADDRESS                  "remote_n_address"
#define L1_REMOTE_N_PORTC                    "remote_n_portc"
#define L1_LOCAL_N_PORTD                     "local_n_portd"
#define L1_REMOTE_N_PORTD                    "remote_n_portd"
#define L1_TRANSPORT_N_PREFERENCE            "tr_n_preference"
#define L1_THREAD_POOL_SIZE                  "thread_pool_size"
#define L1_OFDM_OFFSET_DIVISOR               "ofdm_offset_divisor"
#define L1_PUCCH0_DTX_THRESHOLD              "pucch0_dtx_threshold"
#define L1_PRACH_DTX_THRESHOLD               "prach_dtx_threshold"
#define L1_PUSCH_DTX_THRESHOLD               "pusch_dtx_threshold"
#define L1_SRS_DTX_THRESHOLD                 "srs_dtx_threshold"
#define L1_MAX_LDPC_ITERATIONS               "max_ldpc_iterations"
#define L1_RX_THREAD_CORE                    "L1_rx_thread_core"
#define L1_TX_THREAD_CORE                    "L1_tx_thread_core"
#define L1_NUM_RX_SYM_PER_THREAD             "L1_num_rx_sym_per_thread"
#define HLP_L1_NUM_RX_SYM_PER_THREAD         "number of symbols processed per PUSCH generation thread"
#define L1_NUM_TX_SYM_PER_THREAD             "L1_num_tx_sym_per_thread"
#define HLP_L1_NUM_TX_SYM_PER_THREAD         "number of symbols processed per PDSCH generation thread"
#define HLP_TP_SIZ "thread_pool_size paramter removed, please use --thread-pool"
#define L1_TX_AMP_BACKOFF_dB                 "tx_amp_backoff_dB"
#define HLP_L1TX_BO "Backoff from full-scale output at the L1 entity(frequency domain), ex. 12 would corresponding to 14-bit input level (6 dB/bit). Default 36 dBFS for OAI RU entity"
#define L1_PHASE_COMP                        "phase_compensation"
#define HLP_L1_PHASE_COMP "Apply NR symbolwise phase rotation"
#define L1_NUM_ANTENNAS_PER_THREAD           "dmrs_num_antennas_per_thread"
#define HLP_NUM_ARX "Number of antennas per thread for PUSCH channel estimation"
#define L1_ANALOG_DAS                        "enable_das"
#define L1_AIOT_R2D_PRB_START "aiot_r2d_prb_start"
#define HLP_L1_AIOT_R2D_PRB_START \
  "First DL PRB of the Ambient IoT R2D (38.291), from point A; -1: no R2D. The PRBs must be in dl_prbblacklist"
#define L1_AIOT_R2D_N_RB "aiot_r2d_n_rb"
#define HLP_L1_AIOT_R2D_N_RB "DL PRBs reserved for the Ambient IoT R2D, from aiot_r2d_prb_start"
#define L1_AIOT_R2D_POWER_OFFSET_DB "aiot_r2d_power_offset_dB"
#define HLP_L1_AIOT_R2D_POWER_OFFSET_DB "EPRE of the Ambient IoT R2D over the PDSCH EPRE, in dB"
#define L1_AIOT_R2D_CARRIER "aiot_r2d_carrier"
#define HLP_L1_AIOT_R2D_CARRIER \
  "1: the gNB also sends the Ambient IoT carrier wave, at the centre of the R2D PRBs, whenever no R2D is sent"
#define L1_AIOT_D2R_PRB_START "aiot_d2r_prb_start"
#define HLP_L1_AIOT_D2R_PRB_START \
  "First UL PRB of the Ambient IoT D2R (38.291), from point A; -1: no D2R. The PRBs must be in ul_prbblacklist"
#define L1_AIOT_D2R_N_RB "aiot_d2r_n_rb"
#define HLP_L1_AIOT_D2R_N_RB \
  "UL PRBs reserved for the Ambient IoT D2R, from aiot_d2r_prb_start; the carrier wave and the D2R are at their centre"
#define L1_AIOT_D2R_NOISE_COUNTS "aiot_d2r_noise_counts"
#define HLP_L1_AIOT_D2R_NOISE_COUNTS "Noise floor of the Ambient IoT D2R receiver, rms on I and on Q, in ADC counts"
#define L1_AIOT_D2R_SIC "aiot_d2r_sic"
#define HLP_L1_AIOT_D2R_SIC "1: the Ambient IoT D2R receiver cancels each D2R it decodes and looks again"

/*----------------------------------------------------------------------------------------------------------------------------------------------------*/
/*                                            L1 configuration parameters                                                                             */
/*   optname                                         helpstr   paramflags    XXXptr              defXXXval                  type           numelt     */
/*----------------------------------------------------------------------------------------------------------------------------------------------------*/
// clang-format off
#define L1PARAMS_DESC { \
  {L1_TRANSPORT_N_PREFERENCE,            NULL,       0,         .strptr=NULL,         .defstrval="local_mac",     TYPE_STRING,   0},         \
  {L1_LOCAL_N_ADDRESS,                   NULL,       0,         .strptr=NULL,         .defstrval="127.0.0.1",     TYPE_STRING,   0},         \
  {L1_REMOTE_N_ADDRESS,                  NULL,       0,         .strptr=NULL,         .defstrval="127.0.0.2",     TYPE_STRING,   0},         \
  {L1_REMOTE_N_PORTC,                    NULL,       0,         .uptr=NULL,           .defintval=50030,           TYPE_UINT,     0},         \
  {L1_LOCAL_N_PORTD,                     NULL,       0,         .uptr=NULL,           .defintval=50031,           TYPE_UINT,     0},         \
  {L1_REMOTE_N_PORTD,                    NULL,       0,         .uptr=NULL,           .defintval=50031,           TYPE_UINT,     0},         \
  {L1_THREAD_POOL_SIZE,                  HLP_TP_SIZ, 0,         .uptr=NULL,           .defintval=2022,            TYPE_UINT,     0},         \
  {L1_OFDM_OFFSET_DIVISOR,               NULL,       0,         .uptr=NULL,           .defuintval=8,              TYPE_UINT,     0},         \
  {L1_PUCCH0_DTX_THRESHOLD,              NULL,       0,         .uptr=NULL,           .defintval=100,             TYPE_UINT,     0},         \
  {L1_PRACH_DTX_THRESHOLD,               NULL,       0,         .uptr=NULL,           .defintval=150,             TYPE_UINT,     0},         \
  {L1_PUSCH_DTX_THRESHOLD,               NULL,       0,         .uptr=NULL,           .defintval=50,              TYPE_UINT,     0},         \
  {L1_SRS_DTX_THRESHOLD,                 NULL,       0,         .uptr=NULL,           .defintval=30,              TYPE_UINT,     0},         \
  {L1_MAX_LDPC_ITERATIONS,               NULL,       0,         .uptr=NULL,           .defintval=8,               TYPE_UINT,     0},         \
  {L1_RX_THREAD_CORE,                    NULL,       0,         .iptr=NULL,           .defintval=-1,              TYPE_INT,      0},         \
  {L1_TX_THREAD_CORE,                    NULL,       0,         .iptr=NULL,           .defintval=-1,              TYPE_INT,      0},         \
  {L1_NUM_RX_SYM_PER_THREAD,             HLP_L1_NUM_RX_SYM_PER_THREAD, 0, .iptr=NULL, .defintval=1,               TYPE_INT,      0},         \
  {L1_NUM_TX_SYM_PER_THREAD,             HLP_L1_NUM_TX_SYM_PER_THREAD, 0, .iptr=NULL, .defintval=0,               TYPE_INT,      0},         \
  {L1_TX_AMP_BACKOFF_dB,                 HLP_L1TX_BO,0,         .uptr=NULL,           .defintval=36,              TYPE_UINT,     0},         \
  {L1_PHASE_COMP,                        HLP_L1_PHASE_COMP,PARAMFLAG_BOOL, .uptr=NULL,.defintval=1,               TYPE_UINT,     0},         \
  {L1_NUM_ANTENNAS_PER_THREAD,           HLP_NUM_ARX,0,         .uptr=NULL,           .defintval=1,               TYPE_UINT,     0},         \
  {L1_ANALOG_DAS,                        NULL,       0,         .uptr=NULL,           .defintval=0,               TYPE_UINT,     0},         \
  {L1_AIOT_R2D_PRB_START,                HLP_L1_AIOT_R2D_PRB_START, 0, .iptr=NULL,  .defintval=-1,              TYPE_INT,      0},         \
  {L1_AIOT_R2D_N_RB,                     HLP_L1_AIOT_R2D_N_RB, 0,  .iptr=NULL,        .defintval=3,               TYPE_INT,      0},         \
  {L1_AIOT_R2D_POWER_OFFSET_DB,          HLP_L1_AIOT_R2D_POWER_OFFSET_DB, 0, .dblptr=NULL, .defdblval=0.0,       TYPE_DOUBLE,   0},         \
  {L1_AIOT_R2D_CARRIER,                  HLP_L1_AIOT_R2D_CARRIER, 0, .iptr=NULL,    .defintval=0,               TYPE_INT,      0},         \
  {L1_AIOT_D2R_PRB_START,                HLP_L1_AIOT_D2R_PRB_START, 0, .iptr=NULL,  .defintval=-1,              TYPE_INT,      0},         \
  {L1_AIOT_D2R_N_RB,                     HLP_L1_AIOT_D2R_N_RB, 0,  .iptr=NULL,        .defintval=7,               TYPE_INT,      0},         \
  {L1_AIOT_D2R_NOISE_COUNTS,             HLP_L1_AIOT_D2R_NOISE_COUNTS, 0, .dblptr=NULL, .defdblval=1.0,         TYPE_DOUBLE,   0},         \
  {L1_AIOT_D2R_SIC,                      HLP_L1_AIOT_D2R_SIC, 0,   .iptr=NULL,        .defintval=1,               TYPE_INT,      0},         \
}
// clang-format on

/*----------------------------------------------------------------------------------------------------------------------------------------------------*/
#endif
