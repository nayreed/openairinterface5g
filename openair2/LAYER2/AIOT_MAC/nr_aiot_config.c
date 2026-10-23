/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_config.c
 * \brief The "aiot" section of an NR node's configuration (and --aiot.* options), see nr_aiot_config.h:
 * the PRBs of the node's R2D and D2R, and the Reader's protocol, its Tags and its link, with the
 * defaults of Amber's scenarios.
 */

#include "nr_aiot_config.h"

#include <stdio.h>
#include <string.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "assertions.h"

#define AIOT_SECTION "aiot"

// clang-format off
#define AIOT_PHY_PARAMS_DESC { \
  {"r2d_prb_start",       "first PRB of the R2D (38.291), from point A; -1: no R2D. A gNB sends it in DL PRBs, which must be in its dl_prbblacklist; a UE in UL PRBs, which must be in the gNB's ul_prbblacklist", 0, .iptr=NULL, .defintval=-1, TYPE_INT, 0}, \
  {"r2d_n_rb",            "PRBs reserved for the R2D, from r2d_prb_start", 0, .iptr=NULL, .defintval=3, TYPE_INT, 0}, \
  {"r2d_power_offset_dB", "EPRE of the R2D over the PDSCH EPRE (a UE: PUSCH), in dB", 0, .dblptr=NULL, .defdblval=0.0, TYPE_DOUBLE, 0}, \
  {"r2d_carrier",         "1: the node also sends the carrier wave, at the centre of the R2D PRBs, whenever it sends no R2D (a gNB only)", 0, .iptr=NULL, .defintval=0, TYPE_INT, 0}, \
  {"d2r_prb_start",       "first PRB of the D2R (38.291), from point A; -1: no D2R. A gNB receives it in UL PRBs, which must be in its ul_prbblacklist; a UE in its DL band, in PRBs of the gNB's dl_prbblacklist or past the carrier's, in the guard band its samples hold (25 PRBs at 5.76 Msps: PRBs 25 to 31)", 0, .iptr=NULL, .defintval=-1, TYPE_INT, 0}, \
  {"d2r_n_rb",            "PRBs reserved for the D2R, from d2r_prb_start; the carrier wave and the D2R are at their centre", 0, .iptr=NULL, .defintval=7, TYPE_INT, 0}, \
  {"d2r_noise_counts",    "noise floor of the D2R receiver, rms on I and on Q, in ADC counts", 0, .dblptr=NULL, .defdblval=1.0, TYPE_DOUBLE, 0}, \
  {"d2r_sic",             "1: the D2R receiver cancels each D2R it decodes and looks again", 0, .iptr=NULL, .defintval=1, TYPE_INT, 0}, \
}
#define AIOT_PARAMS_DESC { \
  {"protocol",    "A-IoT Reader protocol (registration, broadcast, unicast, gen2_q, adaptive_aloha); empty: the node is no Reader", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"params",      "protocol parameters, name=value,name=value", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"nodes",       "the Tags' node ids, as 0,1,2 or 0-3", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"m_chip_symb", "R2D chips per OFDM symbol: 2, 6, 12 or 24", 0, .iptr=NULL, .defintval=6, TYPE_INT, 0}, \
  {"n_rb",        "R2D PRBs; 0: the fewest m_chip_symb allows", 0, .iptr=NULL, .defintval=0, TYPE_INT, 0}, \
  {"t_bit",       "D2R bit duration in tau = 2/15 ms: 2, 1, 1/2 ... 1/96", 0, .strptr=NULL, .defstrval="1/8", TYPE_STRING, 0}, \
  {"r_sfs",       "the R_SFS values a paging offers", 0, .strptr=NULL, .defstrval="1,2,4,8", TYPE_STRING, 0}, \
  {"x",           "time slots of access occasions per paging, 1 or 2", 0, .iptr=NULL, .defintval=1, TYPE_INT, 0}, \
  {"r_code",      "1: FEC on the D2R", 0, .iptr=NULL, .defintval=1, TYPE_INT, 0}, \
  {"r_block",     "D2R block repetition, 1 or 2", 0, .iptr=NULL, .defintval=1, TYPE_INT, 0}, \
  {"l_amble",     "D2R preamble and midamble length, 7 or 31", 0, .iptr=NULL, .defintval=31, TYPE_INT, 0}, \
  {"interval_cp", "Interval Bits codepoint 0..3", 0, .iptr=NULL, .defintval=0, TYPE_INT, 0}, \
  {"i_add",       "1: additional midamble", 0, .iptr=NULL, .defintval=0, TYPE_INT, 0}, \
  {"d2r_tbs",     "D2R transport block for data, bytes", 0, .iptr=NULL, .defintval=5, TYPE_INT, 0}, \
  {"lead_slots",  "slots between the slot the node sent last and an R2D's first", 0, .iptr=NULL, .defintval=2, TYPE_INT, 0}, \
  {"output_dir",  "folder for reader_tx.csv and reader_rx.csv; empty: none", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
}
// clang-format on

enum { P_R2D_PRB_START, P_R2D_N_RB, P_R2D_POWER, P_R2D_CARRIER, P_D2R_PRB_START, P_D2R_N_RB, P_D2R_NOISE, P_D2R_SIC };

enum {
  P_PROTOCOL,
  P_PARAMS,
  P_NODES,
  P_M,
  P_N_RB,
  P_T_BIT,
  P_R_SFS,
  P_X,
  P_R_CODE,
  P_R_BLOCK,
  P_L_AMBLE,
  P_INTERVAL,
  P_I_ADD,
  P_TBS,
  P_LEAD,
  P_DIR
};

void nr_aiot_config_phy(nr_aiot_r2d_config_t *r2d, nr_aiot_d2r_config_t *d2r)
{
  paramdef_t p[] = AIOT_PHY_PARAMS_DESC;
  config_get(config_get_if(), p, sizeofArray(p), AIOT_SECTION);
  *r2d = (nr_aiot_r2d_config_t){.prb_start = *p[P_R2D_PRB_START].iptr,
                                .n_rb_max = *p[P_R2D_N_RB].iptr,
                                .power_offset_db = *p[P_R2D_POWER].dblptr,
                                .carrier = *p[P_R2D_CARRIER].iptr};
  AssertFatal(r2d->prb_start < 0 || r2d->n_rb_max >= 1,
              "aiot.r2d_prb_start is set but aiot.r2d_n_rb is %d: give the R2D at least one PRB\n",
              r2d->n_rb_max);
  *d2r = (nr_aiot_d2r_config_t){.prb_start = *p[P_D2R_PRB_START].iptr,
                                .n_rb = *p[P_D2R_N_RB].iptr,
                                .noise_counts = *p[P_D2R_NOISE].dblptr,
                                .enable_sic = *p[P_D2R_SIC].iptr};
  AssertFatal(d2r->prb_start < 0 || d2r->n_rb >= 1,
              "aiot.d2r_prb_start is set but aiot.d2r_n_rb is %d: give the D2R at least one PRB\n",
              d2r->n_rb);
}

nr_aiot_reader_t *nr_aiot_reader_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r)
{
  paramdef_t p[] = AIOT_PARAMS_DESC;
  config_get(config_get_if(), p, sizeofArray(p), AIOT_SECTION);
  const char *protocol = *p[P_PROTOCOL].strptr;
  if (!protocol || !protocol[0])
    return NULL;
  static nr_aiot_reader_config_t cfg; /* large: not on the stack */
  memset(&cfg, 0, sizeof(cfg));
  snprintf(cfg.protocol, sizeof(cfg.protocol), "%s", protocol);
  AssertFatal(nr_aiot_reader_parse_params(&cfg, *p[P_PARAMS].strptr) == 0,
              "aiot.params \"%s\" is not name=value,...\n",
              *p[P_PARAMS].strptr);
  AssertFatal(nr_aiot_reader_parse_nodes(&cfg, *p[P_NODES].strptr) == 0 && cfg.n_nodes > 0,
              "aiot.nodes \"%s\" must list the Tags' node ids\n",
              *p[P_NODES].strptr);
  AssertFatal(nr_aiot_reader_parse_t_bit(&cfg, *p[P_T_BIT].strptr) == 0,
              "aiot.t_bit \"%s\" is not a bit duration\n",
              *p[P_T_BIT].strptr);
  AssertFatal(nr_aiot_reader_parse_r_sfs(&cfg, *p[P_R_SFS].strptr) == 0,
              "aiot.r_sfs \"%s\" is not a list of R_SFS\n",
              *p[P_R_SFS].strptr);
  cfg.link.m_chip_symb = *p[P_M].iptr;
  cfg.link.n_rb = *p[P_N_RB].iptr;
  cfg.link.x = *p[P_X].iptr;
  cfg.link.r_code = *p[P_R_CODE].iptr;
  cfg.link.r_block = *p[P_R_BLOCK].iptr;
  cfg.link.l_amble = *p[P_L_AMBLE].iptr;
  cfg.link.interval_cp = *p[P_INTERVAL].iptr;
  cfg.link.i_add = *p[P_I_ADD].iptr;
  cfg.link.d2r_tbs = *p[P_TBS].iptr;
  cfg.lead_slots = *p[P_LEAD].iptr;
  snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", *p[P_DIR].strptr ? *p[P_DIR].strptr : "");
  char err[256] = "";
  nr_aiot_reader_t *r = nr_aiot_reader_new(&cfg, r2d, d2r, err, sizeof(err));
  AssertFatal(r, "A-IoT Reader: %s\n", err);
  AssertFatal(nr_aiot_reader_start(r) == 0, "A-IoT Reader: cannot start its thread\n");
  LOG_I(NR_MAC,
        "A-IoT Reader: protocol %s for %d Tags, M = %d, T_bit = %d/%d tau, %d R_SFS%s%s\n",
        cfg.protocol,
        cfg.n_nodes,
        cfg.link.m_chip_symb,
        cfg.link.t_bit_num,
        cfg.link.t_bit_den,
        cfg.link.n_r_sfs,
        cfg.output_dir[0] ? ", records in " : "",
        cfg.output_dir);
  return r;
}
