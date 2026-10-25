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
#include <stdlib.h>
#include <string.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/utils.h"

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
  {"lead_slots",  "slots between the slot the node sent last and an R2D's first, 1 to 64 (0: the default); it grows when an R2D is late", 0, .iptr=NULL, .defintval=NR_AIOT_READER_LEAD_SLOTS, TYPE_INT, 0}, \
  {"output_dir",  "folder for reader_tx.csv and reader_rx.csv; empty: none", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"data_dest",   "ipv4:port to send each message heard from a Tag to, one UDP datagram of Tag data each (aiot_node_msg.h); empty: none. A UE gives the network's end of its user plane", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"remote_r2d",  "a split Reader: ipv4:port of the UE that sends the R2D in its UL (its aiot.serve_port), over the gNB's user plane; the gNB then has no R2D PRBs. Empty: the node sends the R2D", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"remote_d2r",  "a split Reader: ipv4:port of the UE that receives the D2R in its DL (its aiot.serve_port); the gNB then has no D2R PRBs. Empty: the node receives the D2R", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
}
#define AIOT_PEER_PARAMS_DESC { \
  {"serve_port",   "a UE: the UDP port it serves a split Reader on (the gNB's aiot.remote_r2d or aiot.remote_d2r), with its R2D PRBs or its D2R receiver; 0: none. For a lab: the messages are not authenticated", 0, .iptr=NULL, .defintval=0, TYPE_INT, 0}, \
  {"serve_addr",   "a UE: the IPv4 address it serves the split Reader on, such as its PDU session's; empty: every address of the UE", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
  {"serve_reader", "a UE: the IPv4 address of the gNB's Reader, the only sender it serves; empty: any sender", 0, .strptr=NULL, .defstrval="", TYPE_STRING, 0}, \
}
// clang-format on

/* A configuration error: the node stops, with the reason. */
#define CONFIG_FAIL(...)        \
  do {                          \
    LOG_E(NR_MAC, __VA_ARGS__); \
    exit(EXIT_FAILURE);         \
  } while (0)

static void get_section(paramdef_t *p, int n)
{
  if (config_get(config_get_if(), p, n, AIOT_SECTION) < 0)
    CONFIG_FAIL("A-IoT: cannot read the \"" AIOT_SECTION "\" section of the configuration\n");
}

static int get_int(paramdef_t *p, int n, const char *name)
{
  return *gpd(p, n, name)->iptr;
}

static double get_double(paramdef_t *p, int n, const char *name)
{
  return *gpd(p, n, name)->dblptr;
}

static const char *get_str(paramdef_t *p, int n, const char *name)
{
  const char *s = *gpd(p, n, name)->strptr;
  return s ? s : "";
}

void nr_aiot_config_phy(nr_aiot_r2d_config_t *r2d, nr_aiot_d2r_config_t *d2r)
{
  paramdef_t p[] = AIOT_PHY_PARAMS_DESC;
  const int n = sizeofArray(p);
  get_section(p, n);
  *r2d = (nr_aiot_r2d_config_t){.prb_start = get_int(p, n, "r2d_prb_start"),
                                .n_rb_max = get_int(p, n, "r2d_n_rb"),
                                .power_offset_db = get_double(p, n, "r2d_power_offset_dB"),
                                .carrier = get_int(p, n, "r2d_carrier") != 0};
  if (r2d->prb_start >= 0 && r2d->n_rb_max < 1)
    CONFIG_FAIL("aiot.r2d_prb_start is set but aiot.r2d_n_rb is %d: give the R2D at least one PRB\n", r2d->n_rb_max);
  *d2r = (nr_aiot_d2r_config_t){.prb_start = get_int(p, n, "d2r_prb_start"),
                                .n_rb = get_int(p, n, "d2r_n_rb"),
                                .noise_counts = get_double(p, n, "d2r_noise_counts"),
                                .enable_sic = get_int(p, n, "d2r_sic")};
  if (d2r->prb_start >= 0 && d2r->n_rb < 1)
    CONFIG_FAIL("aiot.d2r_prb_start is set but aiot.d2r_n_rb is %d: give the D2R at least one PRB\n", d2r->n_rb);
}

/* The Reader of the section, on r2d and d2r, from the section's p. */
static nr_aiot_reader_t *reader_from(paramdef_t *p, int n, const char *protocol, nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r)
{
  nr_aiot_reader_config_t *cfg = calloc_or_fail(1, sizeof(*cfg)); /* large: not on the stack */
  snprintf(cfg->protocol, sizeof(cfg->protocol), "%s", protocol);
  const char *s;
  if (nr_aiot_reader_parse_params(cfg, s = get_str(p, n, "params")) != 0)
    CONFIG_FAIL("aiot.params \"%s\" is not name=value,...\n", s);
  if (nr_aiot_reader_parse_nodes(cfg, s = get_str(p, n, "nodes")) != 0 || cfg->n_nodes == 0)
    CONFIG_FAIL("aiot.nodes \"%s\" must list the Tags' node ids\n", s);
  if (nr_aiot_reader_parse_t_bit(cfg, s = get_str(p, n, "t_bit")) != 0)
    CONFIG_FAIL("aiot.t_bit \"%s\" is not a bit duration\n", s);
  if (nr_aiot_reader_parse_r_sfs(cfg, s = get_str(p, n, "r_sfs")) != 0)
    CONFIG_FAIL("aiot.r_sfs \"%s\" is not a list of R_SFS\n", s);
  cfg->link.m_chip_symb = get_int(p, n, "m_chip_symb");
  cfg->link.n_rb = get_int(p, n, "n_rb");
  cfg->link.x = get_int(p, n, "x");
  cfg->link.r_code = get_int(p, n, "r_code");
  cfg->link.r_block = get_int(p, n, "r_block");
  cfg->link.l_amble = get_int(p, n, "l_amble");
  cfg->link.interval_cp = get_int(p, n, "interval_cp");
  cfg->link.i_add = get_int(p, n, "i_add");
  cfg->link.d2r_tbs = get_int(p, n, "d2r_tbs");
  cfg->lead_slots = get_int(p, n, "lead_slots");
  snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", get_str(p, n, "output_dir"));
  if (nr_aiot_reader_parse_data_dest(cfg, s = get_str(p, n, "data_dest")) != 0)
    CONFIG_FAIL("aiot.data_dest \"%s\" is not ipv4:port\n", s);
  if (nr_aiot_reader_parse_address(cfg->remote_r2d, s = get_str(p, n, "remote_r2d")) != 0)
    CONFIG_FAIL("aiot.remote_r2d \"%s\" is not ipv4:port\n", s);
  if (nr_aiot_reader_parse_address(cfg->remote_d2r, s = get_str(p, n, "remote_d2r")) != 0)
    CONFIG_FAIL("aiot.remote_d2r \"%s\" is not ipv4:port\n", s);
  char err[256] = "";
  nr_aiot_reader_t *r = nr_aiot_reader_new(cfg, r2d, d2r, err, sizeof(err));
  if (!r)
    CONFIG_FAIL("A-IoT Reader: %s\n", err);
  LOG_I(NR_MAC,
        "A-IoT Reader: protocol %s for %d Tags, M = %d, T_bit = %d/%d tau, %d R_SFS%s%s%s%s%s%s%s%s\n",
        cfg->protocol,
        cfg->n_nodes,
        cfg->link.m_chip_symb,
        cfg->link.t_bit_num,
        cfg->link.t_bit_den,
        cfg->link.n_r_sfs,
        cfg->output_dir[0] ? ", records in " : "",
        cfg->output_dir,
        cfg->data_dest[0] ? ", Tag data to " : "",
        cfg->data_dest,
        cfg->remote_r2d[0] ? ", R2D sent by the UE at " : "",
        cfg->remote_r2d,
        cfg->remote_d2r[0] ? ", D2R received by the UE at " : "",
        cfg->remote_d2r);
  free(cfg); /* the Reader keeps its own copy */
  nr_aiot_reader_start(r); /* a new Reader: it does not run yet */
  return r;
}

nr_aiot_reader_t *nr_aiot_reader_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r)
{
  paramdef_t p[] = AIOT_PARAMS_DESC;
  const int n = sizeofArray(p);
  get_section(p, n);
  const char *protocol = get_str(p, n, "protocol");
  return protocol[0] ? reader_from(p, n, protocol, r2d, d2r) : NULL;
}

/* The UE's end of a split Reader, from the section's p. */
static nr_aiot_peer_t *peer_from(paramdef_t *p, int n, nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r)
{
  nr_aiot_peer_config_t cfg = {.port = get_int(p, n, "serve_port")};
  const char *addr = get_str(p, n, "serve_addr"), *reader = get_str(p, n, "serve_reader");
  if (cfg.port < 0 || cfg.port > 65535)
    CONFIG_FAIL("aiot.serve_port %d is no UDP port\n", cfg.port);
  if (strlen(addr) >= sizeof(cfg.addr) || strlen(reader) >= sizeof(cfg.reader))
    CONFIG_FAIL("aiot.serve_addr \"%s\" and aiot.serve_reader \"%s\" must be IPv4 addresses\n", addr, reader);
  snprintf(cfg.addr, sizeof(cfg.addr), "%s", addr);
  snprintf(cfg.reader, sizeof(cfg.reader), "%s", reader);
  char err[256] = "";
  nr_aiot_peer_t *peer = nr_aiot_peer_new(&cfg, r2d, d2r, err, sizeof(err));
  if (!peer)
    CONFIG_FAIL("A-IoT peer: %s\n", err);
  LOG_I(NR_MAC,
        "A-IoT peer: serving a split Reader on %s:%d, for %s:%s%s\n",
        cfg.addr[0] ? cfg.addr : "every address",
        cfg.port,
        cfg.reader[0] ? cfg.reader : "any sender",
        r2d && r2d->cfg.prb_start >= 0 ? " the R2D it sends, in the UE's UL PRBs" : "",
        d2r ? " the D2R windows it opens, on the UE's DL" : "");
  nr_aiot_peer_start(peer); /* a new peer: it does not run yet */
  return peer;
}

void nr_aiot_ue_from_config(nr_aiot_r2d_t *r2d, nr_aiot_d2r_t *d2r, nr_aiot_reader_t **reader, nr_aiot_peer_t **peer)
{
  paramdef_t p[] = AIOT_PARAMS_DESC;
  paramdef_t q[] = AIOT_PEER_PARAMS_DESC;
  const int n = sizeofArray(p), m = sizeofArray(q);
  get_section(p, n);
  get_section(q, m);
  const char *protocol = get_str(p, n, "protocol");
  const int port = get_int(q, m, "serve_port");
  /* both checked before either starts: the R2D and the D2R are either the UE's Reader's or the gNB's */
  if (protocol[0] && port != 0)
    CONFIG_FAIL("aiot.protocol and aiot.serve_port are both set: the UE is either a Reader or the UE of a gNB's split Reader\n");
  *reader = protocol[0] ? reader_from(p, n, protocol, r2d, d2r) : NULL;
  *peer = port != 0 ? peer_from(q, m, r2d, d2r) : NULL;
}
