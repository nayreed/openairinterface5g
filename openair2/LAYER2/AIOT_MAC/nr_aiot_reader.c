/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_reader.c
 * \brief An NR node as an Ambient IoT Reader: the Reader MAC on the node's R2D and D2R (nr_aiot_reader.h).
 */

#include "nr_aiot_reader.h"

#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aiot_node_msg.h"
#include "nr_aiot_udp.h"
#include "common/utils/LOG/log.h"
#include "common/utils/system.h"
#include "common/utils/utils.h"

#define SLOT_SAMPLES AIOT_SAMPLES_PER_MS /* one 15 kHz slot */
#define LEAD_MARGIN_SLOTS 2 /* a remote R2D's lead over the delay of the one that came too late */
/* the windows the MAC opens after one R2D (it opens at most 16), and the remote windows open at a time,
 * which the UE's peer has as many of */
#define MAX_EXPECTS AIOT_NODE_MAX_WINDOWS
#define MAX_OUTCOMES 8
#define ROWS_AT_A_TIME 64 /* the protocol's table rows taken at a time */
#define FULL_SCALE2 (32767.0 * 32767.0) /* counts squared of a full-scale sample */
/* a split Reader */
#define WINDOW_TIMEOUT_SLOTS 200 /* a remote window's outcome not back this long after its end: given up as empty */
#define REPORT_SAMPLES (1000 * (int64_t)SLOT_SAMPLES) /* the split's delays are logged once a second of the cell */
#define SENT_RING 256 /* the times of the R2Ds last sent to the remote R2D, for their round trip */
/* until the remote R2D has answered, one R2D this often; until the remote D2R has answered, one probe
 * window this often */
#define PROBE_SLOTS 100

typedef struct {
  int used;
  aiot_d2r_expect_t e;
  int64_t deadline;
  int lead; /* the lead it was opened with */
  int early; /* opened before the remote D2R first answered */
} remote_window_t;

struct nr_aiot_reader {
  nr_aiot_reader_config_t cfg;
  nr_aiot_r2d_t *r2d;
  nr_aiot_d2r_t *d2r;
  aiot_reader_t *mac;
  int n_rb;
  int lead;
  aiot_reader_r2d_t next; /* the R2D the protocol gave, not yet on the grid */
  int have_next;
  aiot_d2r_expect_t expects[MAX_EXPECTS];
  aiot_d2r_outcome_t *outcomes; /* MAX_OUTCOMES */
  aiot_reader_heard_t *heard;
  FILE *tx_csv, *rx_csv, *table_csv;
  int table_cols;
  double *rows;
  int ended; /* the protocol's end is logged */
  int sock; /* to the data destination, -1: none */
  struct sockaddr_in dest;
  uint32_t seq; /* Tag data messages so far */
  nr_aiot_udp_tx_t data_tx;
  int decimation; /* radio samples per A-IoT sample */
  /* a split Reader: one socket to the remote R2D, the remote D2R, or both */
  int peer_sock; /* -1: none */
  int remote_r2d, remote_d2r;
  struct sockaddr_in r2d_peer, d2r_peer;
  uint32_t r2d_seq; /* R2Ds sent to the remote R2D */
  uint32_t lead_seq; /* the first R2D sent with the current lead */
  int peer_up; /* the remote R2D has answered: until then the Reader sends it one R2D at a time */
  int64_t probe_at; /* when the last of those went */
  uint32_t up_seq; /* the first R2D sent after it answered */
  int d2r_up; /* the remote D2R has answered: until then the Reader sends no R2D, only probe windows */
  int64_t d2r_probe_at; /* when the last of those went */
  int64_t sent_at[SENT_RING]; /* when the R2D of seq % SENT_RING was sent, in A-IoT samples */
  remote_window_t windows[MAX_EXPECTS];
  uint8_t *msg; /* AIOT_NODE_SPLIT_MAX_BYTES + 1 */
  aiot_node_outcome_t *remote_outcome;
  nr_aiot_udp_tx_t peer_tx;
  int recv_errno;
  int64_t now; /* the pass's now, for the messages it takes */
  int64_t next_report;
  pthread_mutex_t lock; /* step() and stats() */
  nr_aiot_reader_stats_t stats;
  pthread_t thread;
  int running, stop;
};

/* ---------------------------------------------------------------------------------------------
 * Configuration strings
 * ------------------------------------------------------------------------------------------- */

int nr_aiot_reader_parse_params(nr_aiot_reader_config_t *cfg, const char *s)
{
  cfg->n_params = 0;
  if (!s)
    return 0;
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", s);
  char *save = NULL;
  for (char *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
    while (*item == ' ')
      item++;
    if (!*item)
      continue;
    char *eq = strchr(item, '=');
    if (!eq || eq == item || cfg->n_params == NR_AIOT_READER_MAX_PARAMS)
      return -1;
    *eq = 0;
    char *end = NULL;
    double v = strtod(eq + 1, &end);
    if (end == eq + 1 || *end)
      return -1;
    int i = cfg->n_params++;
    snprintf(cfg->param_names[i], sizeof(cfg->param_names[i]), "%s", item);
    cfg->params[i] = (aiot_param_t){.name = cfg->param_names[i], .value = v};
  }
  return 0;
}

int nr_aiot_reader_parse_nodes(nr_aiot_reader_config_t *cfg, const char *s)
{
  cfg->n_nodes = 0;
  if (!s)
    return 0;
  const char *p = s;
  while (*p) {
    while (*p == ' ' || *p == ',')
      p++;
    if (!*p)
      break;
    char *end;
    long a = strtol(p, &end, 10), b = a;
    if (end == p)
      return -1;
    p = end;
    if (*p == '-') {
      b = strtol(p + 1, &end, 10);
      if (end == p + 1)
        return -1;
      p = end;
    }
    if (a < 0 || b < a || b > 65535)
      return -1;
    for (long n = a; n <= b; n++) {
      if (cfg->n_nodes == NR_AIOT_READER_MAX_NODES)
        return -1;
      cfg->node_ids[cfg->n_nodes++] = (int)n;
    }
    if (*p && *p != ',' && *p != ' ')
      return -1;
  }
  return 0;
}

int nr_aiot_reader_parse_r_sfs(nr_aiot_reader_config_t *cfg, const char *s)
{
  int n = 0;
  const char *p = s;
  while (p && *p) {
    while (*p == ' ' || *p == ',')
      p++;
    if (!*p)
      break;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p || n == 8 || v < 1)
      return -1;
    cfg->link.r_sfs[n++] = (int)v;
    p = end;
  }
  if (!n)
    return -1;
  /* ascending, as the link wants them */
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && cfg->link.r_sfs[j] < cfg->link.r_sfs[j - 1]; j--) {
      int t = cfg->link.r_sfs[j];
      cfg->link.r_sfs[j] = cfg->link.r_sfs[j - 1];
      cfg->link.r_sfs[j - 1] = t;
    }
  cfg->link.n_r_sfs = n;
  return 0;
}

int nr_aiot_reader_parse_t_bit(nr_aiot_reader_config_t *cfg, const char *s)
{
  int num = 0, den = 1;
  char extra;
  if (!s || (sscanf(s, " %d / %d %c", &num, &den, &extra) != 2 && (den = 1, sscanf(s, " %d %c", &num, &extra) != 1)))
    return -1;
  if (num < 1 || den < 1)
    return -1;
  cfg->link.t_bit_num = num;
  cfg->link.t_bit_den = den;
  return 0;
}

int nr_aiot_reader_parse_address(char *dst, const char *s)
{
  dst[0] = 0;
  if (!s || !*s)
    return 0;
  struct sockaddr_in addr;
  if (strlen(s) >= NR_AIOT_READER_ADDRESS_LEN || nr_aiot_udp_parse(s, &addr) != 0)
    return -1;
  snprintf(dst, NR_AIOT_READER_ADDRESS_LEN, "%s", s);
  return 0;
}

int nr_aiot_reader_parse_data_dest(nr_aiot_reader_config_t *cfg, const char *s)
{
  return nr_aiot_reader_parse_address(cfg->data_dest, s);
}

/* ---------------------------------------------------------------------------------------------
 * Records, in the columns of Amber's Reader
 * ------------------------------------------------------------------------------------------- */

/* A float as Python's repr writes it: the shortest digits that read back, in fixed notation from
 * 1e-4 to 1e16 with ".0" on a whole number, else in exponent notation. */
static void put_float(FILE *f, double v)
{
  char b[64];
  const double a = fabs(v);
  if (isfinite(v) && (a == 0 || (a >= 1e-4 && a < 1e16))) {
    for (int d = 0; d <= 20; d++) {
      snprintf(b, sizeof(b), "%.*f", d, v);
      if (strtod(b, NULL) == v)
        break;
    }
    if (!strchr(b, '.'))
      strcat(b, ".0");
  } else {
    for (int prec = 1; prec <= 17; prec++) {
      snprintf(b, sizeof(b), "%.*e", prec - 1, v);
      if (strtod(b, NULL) == v)
        break;
    }
  }
  fputs(b, f);
}

static double ms(int64_t samples)
{
  return (double)samples / AIOT_SAMPLES_PER_MS;
}

/* (name, detail) of an R2D message, as amber.mac.describe() writes them. */
static const char *describe(const aiot_mac_r2d_msg_t *m, char *detail, size_t len)
{
  detail[0] = 0;
  switch (m->kind) {
    case AIOT_R2D_PAGING_CBRA: {
      int o = snprintf(detail,
                       len,
                       "tid=%d n=%d k=%d x=%d r_sfs=[",
                       m->transaction_id,
                       aiot_mac_num_access_occasions(m->num_access_occasions_code),
                       m->k_bit ? 4 : 1,
                       m->sched.x);
      for (int i = 0; i < m->sched.n_sfs && o < (int)len; i++)
        o += snprintf(detail + o, len - o, i ? ", %d" : "%d", m->sched.r_sfs[i]);
      if (o < (int)len)
        snprintf(detail + o, len - o, "]");
      return "paging_cbra";
    }
    case AIOT_R2D_PAGING_CFA:
      snprintf(detail, len, "paging_id=%d bits r_sfs=%d", m->paging_id_len, m->sched.r_sfs[0]);
      return "paging_cfa";
    case AIOT_R2D_ACCESS_TRIGGER:
      return "access_trigger";
    case AIOT_R2D_RANDOM_ID_RESPONSE:
      snprintf(detail, len, "entries=%d", m->n_entries);
      return "random_id_response";
    case AIOT_R2D_UPPER_LAYER_DATA:
      snprintf(detail, len, "as_id=%d ci=%d", m->as_id, m->has_sdu ? 1 : 0);
      return "r2d_data";
    case AIOT_R2D_NACK_FEEDBACK: {
      int o = snprintf(detail, len, "as_ids=[");
      for (int i = 0; i < m->n_as_ids && o < (int)len; i++)
        o += snprintf(detail + o, len - o, i ? ", %d" : "%d", m->as_ids[i]);
      if (o < (int)len)
        snprintf(detail + o, len - o, "]");
      return "nack_feedback";
    }
  }
  return "unknown";
}

/* A CSV field: quoted when it holds a comma, as Python's csv writes it. */
static void put_field(FILE *f, const char *s)
{
  if (strpbrk(s, ",\"\n")) {
    fputc('"', f);
    for (; *s; s++) {
      if (*s == '"')
        fputc('"', f);
      fputc(*s, f);
    }
    fputc('"', f);
  } else {
    fputs(s, f);
  }
}

static FILE *open_csv(const char *dir, const char *name, const char *header)
{
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f) {
    LOG_E(NR_MAC, "A-IoT Reader: cannot write %s: %s\n", path, strerror(errno));
    return NULL;
  }
  fputs(header, f);
  fputs("\r\n", f); /* Python's csv ends its lines so */
  return f;
}

static double dbfs(double counts2)
{
  return counts2 > 0 ? round(100 * 10 * log10(counts2 / FULL_SCALE2)) / 100 : -999.0;
}

/* The node id and value of a Tag's Data SDU (amber.iq.read_sdu, ">HB"), 0 when it is not one. */
static int read_sdu(const aiot_mac_d2r_msg_t *m, int *node, int *value)
{
  if (m->sdu_len != 3)
    return 0;
  *node = (m->sdu[0] << 8) | m->sdu[1];
  *value = m->sdu[2];
  return 1;
}

/* One message heard from a Tag, as Tag data to the data destination. The socket does not block: a
 * datagram it cannot take is counted and lost, and the next one's seq shows the gap. */
static void send_tag_data(nr_aiot_reader_t *r, const aiot_reader_heard_t *h, const aiot_reader_answer_t *a)
{
  aiot_tag_data_t m = {.seq = r->seq++, .after = h->sent.kind, .answer = *a};
  uint8_t buf[AIOT_NODE_MSG_MAX_BYTES];
  const int n = aiot_node_msg_pack_tag_data(&m, buf, sizeof(buf));
  if (n < 0) {
    LOG_E(NR_MAC, "A-IoT Reader: cannot pack the Tag data of seq %u (%d)\n", m.seq, n);
    r->stats.data_unsent++;
    return;
  }
  if (nr_aiot_udp_send(r->sock, &r->dest, buf, n, &r->data_tx, "Tag data"))
    r->stats.data_sent++;
  else
    r->stats.data_unsent++;
}

static void record(nr_aiot_reader_t *r, const aiot_reader_heard_t *h)
{
  char detail[256];
  const char *name = describe(&h->sent, detail, sizeof(detail));
  if (r->tx_csv) {
    put_float(r->tx_csv, ms(h->r2d_start));
    fputc(',', r->tx_csv);
    put_float(r->tx_csv, ms(h->r2d_end));
    fprintf(r->tx_csv, ",%s,", name);
    put_field(r->tx_csv, detail);
    fputs("\r\n", r->tx_csv);
  }
  for (int i = 0; i < h->n_answers; i++) {
    const aiot_reader_answer_t *a = &h->answers[i];
    if (r->sock >= 0)
      send_tag_data(r, h, a);
    int node = -1, value = -1;
    char id[16] = "";
    if (a->kind == AIOT_ANSWER_MSG1) {
      snprintf(id, sizeof(id), "%d", a->message.random_id);
    } else {
      read_sdu(&a->message, &node, &value);
      if (a->as_id >= 0)
        snprintf(id, sizeof(id), "%d", a->as_id);
      LOG_D(NR_MAC,
            "A-IoT Reader: data from Tag %d (AS ID %s): value %d, R_SFS %d, %.1f dBFS, after %s\n",
            node,
            id[0] ? id : "-",
            value,
            a->r_sfs,
            dbfs(a->rssi_w),
            name);
    }
    if (r->rx_csv) {
      put_float(r->rx_csv, ms(a->start));
      fprintf(r->rx_csv,
              ",%s,%s,%d,%d,%s,%d,%d,",
              name,
              a->kind == AIOT_ANSWER_MSG1 ? "msg1" : "data",
              node,
              value,
              id,
              a->r_sfs,
              a->slot);
      put_float(r->rx_csv, dbfs(a->rssi_w));
      fputs("\r\n", r->rx_csv);
    }
  }
  for (int i = 0; i < h->collided && r->rx_csv; i++) {
    put_float(r->rx_csv, ms(h->r2d_end));
    fprintf(r->rx_csv, ",%s,collision,-1,-1,,,,\r\n", name);
  }
  r->stats.answers += h->n_answers;
  r->stats.collisions += h->collided;
}

/* The protocol's own table rows, as amber.mac.Reader.table() converts them. */
static void record_rows(nr_aiot_reader_t *r)
{
  if (!r->table_csv)
    return;
  int n;
  while ((n = aiot_reader_pop_rows(r->mac, r->rows, ROWS_AT_A_TIME)) > 0)
    for (int i = 0; i < n; i++) {
      const double *row = r->rows + (size_t)i * r->table_cols;
      put_float(r->table_csv, round(row[0] / 1000.0 * 1e6) / 1e6);
      for (int c = 1; c < r->table_cols; c++)
        fprintf(r->table_csv, ",%lld", (long long)row[c]);
      fputs("\r\n", r->table_csv);
    }
}

static void collect(nr_aiot_reader_t *r)
{
  while (aiot_reader_pop_heard(r->mac, r->heard) == 1)
    record(r, r->heard);
  record_rows(r);
}

static void flush_files(nr_aiot_reader_t *r)
{
  if (r->tx_csv)
    fflush(r->tx_csv);
  if (r->rx_csv)
    fflush(r->rx_csv);
  if (r->table_csv)
    fflush(r->table_csv);
}

/* ---------------------------------------------------------------------------------------------
 * The Reader
 * ------------------------------------------------------------------------------------------- */

nr_aiot_reader_t *nr_aiot_reader_new(const nr_aiot_reader_config_t *cfg,
                                     nr_aiot_r2d_t *r2d,
                                     nr_aiot_d2r_t *d2r,
                                     char *err,
                                     int err_len)
{
  const int remote_r2d = cfg->remote_r2d[0] != 0, remote_d2r = cfg->remote_d2r[0] != 0;
  if (!r2d || r2d->slots_per_frame == 0) {
    snprintf(err, err_len, "the Reader needs the node's R2D clock");
    return NULL;
  }
  if (!remote_r2d && r2d->cfg.prb_start < 0) {
    snprintf(err,
             err_len,
             "the Reader needs the node's R2D PRBs (aiot.r2d_prb_start), or a UE that sends the R2D (aiot.remote_r2d)");
    return NULL;
  }
  if (remote_r2d && r2d->cfg.prb_start >= 0) {
    snprintf(err, err_len, "the node has R2D PRBs (aiot.r2d_prb_start) and a UE sends the R2D (aiot.remote_r2d): give one");
    return NULL;
  }
  if (!remote_d2r && !d2r) {
    snprintf(err,
             err_len,
             "the Reader needs the node's D2R PRBs (aiot.d2r_prb_start), or a UE that receives the D2R (aiot.remote_d2r)");
    return NULL;
  }
  if (remote_d2r && d2r) {
    snprintf(err, err_len, "the node has D2R PRBs (aiot.d2r_prb_start) and a UE receives the D2R (aiot.remote_d2r): give one");
    return NULL;
  }
  if (r2d->slots_per_frame != AIOT_CELL_SLOTS_PER_FRAME || (!d2r && r2d->ofdm_symbol_size % AIOT_N_FFT != 0)) {
    snprintf(err,
             err_len,
             "the Reader needs 15 kHz slots, sampled at 5.76 Msps times a whole number (here %d slots a frame, symbols of %d)",
             r2d->slots_per_frame,
             r2d->ofdm_symbol_size);
    return NULL;
  }
  if (cfg->lead_slots < 0 || cfg->lead_slots > NR_AIOT_READER_MAX_LEAD_SLOTS) {
    snprintf(err,
             err_len,
             "a lead of %d slots: give 0 (the default) to %d (aiot.lead_slots)",
             cfg->lead_slots,
             NR_AIOT_READER_MAX_LEAD_SLOTS);
    return NULL;
  }
  nr_aiot_reader_t *r = calloc_or_fail(1, sizeof(*r));
  r->cfg = *cfg;
  for (int i = 0; i < r->cfg.n_params; i++)
    r->cfg.params[i].name = r->cfg.param_names[i];
  r->r2d = r2d;
  r->d2r = d2r;
  r->sock = -1;
  r->peer_sock = -1;
  r->decimation = d2r ? nr_aiot_d2r_decimation(d2r) : r2d->ofdm_symbol_size / AIOT_N_FFT;
  r->remote_r2d = remote_r2d;
  r->remote_d2r = remote_d2r;
  r->stats.r2d_ahead_min = INT64_MAX;
  r->stats.window_ahead_min = INT64_MAX;
  r->lead = cfg->lead_slots > 0 ? cfg->lead_slots : NR_AIOT_READER_LEAD_SLOTS;
  pthread_mutex_init(&r->lock, NULL);
  r->outcomes = malloc_or_fail(sizeof(*r->outcomes) * MAX_OUTCOMES);
  r->heard = malloc_or_fail(sizeof(*r->heard));
  r->mac =
      aiot_reader_new(&r->cfg.link, r->cfg.protocol, r->cfg.params, r->cfg.n_params, r->cfg.node_ids, r->cfg.n_nodes, err, err_len);
  if (!r->mac) {
    if (err[0] == 0)
      snprintf(err, err_len, "out of memory");
    nr_aiot_reader_free(r);
    return NULL;
  }
  r->n_rb = aiot_link_n_rb(&r->cfg.link);
  if (!remote_r2d && r->n_rb > r2d->cfg.n_rb_max) {
    snprintf(err, err_len, "the R2D needs %d PRBs, and the node reserves %d (aiot.r2d_n_rb)", r->n_rb, r2d->cfg.n_rb_max);
    nr_aiot_reader_free(r);
    return NULL;
  }
  if (r->cfg.data_dest[0]) {
    if (nr_aiot_udp_parse(r->cfg.data_dest, &r->dest) != 0) {
      snprintf(err, err_len, "the data destination \"%s\" is not ipv4:port (aiot.data_dest)", r->cfg.data_dest);
      nr_aiot_reader_free(r);
      return NULL;
    }
    r->sock = nr_aiot_udp_open(NULL, err, err_len);
    if (r->sock < 0) {
      nr_aiot_reader_free(r);
      return NULL;
    }
  }
  if (remote_r2d || remote_d2r) {
    const char *bad = remote_r2d && nr_aiot_udp_parse(r->cfg.remote_r2d, &r->r2d_peer) != 0   ? r->cfg.remote_r2d
                      : remote_d2r && nr_aiot_udp_parse(r->cfg.remote_d2r, &r->d2r_peer) != 0 ? r->cfg.remote_d2r
                                                                                              : NULL;
    if (bad) {
      snprintf(err, err_len, "the UE's address \"%s\" is not ipv4:port (aiot.remote_r2d, aiot.remote_d2r)", bad);
      nr_aiot_reader_free(r);
      return NULL;
    }
    r->msg = malloc_or_fail(AIOT_NODE_SPLIT_MAX_BYTES + 1);
    r->remote_outcome = malloc_or_fail(sizeof(*r->remote_outcome));
    r->peer_sock = nr_aiot_udp_open(NULL, err, err_len);
    if (r->peer_sock < 0) {
      nr_aiot_reader_free(r);
      return NULL;
    }
  }
  /* the Reader plans on the node's slots, also when another node sends the R2D */
  nr_aiot_r2d_start_clock(r2d);
  if (r->cfg.output_dir[0]) {
    if (mkdir(r->cfg.output_dir, 0777) != 0 && errno != EEXIST)
      LOG_W(NR_MAC, "A-IoT Reader: cannot make the folder %s: %s\n", r->cfg.output_dir, strerror(errno));
    r->tx_csv = open_csv(r->cfg.output_dir, "reader_tx.csv", "start_ms,end_ms,message,detail");
    r->rx_csv = open_csv(r->cfg.output_dir, "reader_rx.csv", "time_ms,after,kind,node_id,value,id,r_sfs,slot,rssi_dbfs");
    const char *name;
    const char *const *cols;
    int nc;
    if (aiot_reader_table(r->mac, &name, &cols, &nc) == 1 && nc > 0) {
      char header[256] = "";
      for (int c = 0; c < nc; c++) {
        if (c)
          strncat(header, ",", sizeof(header) - strlen(header) - 1);
        strncat(header, c == 0 && !strcmp(cols[0], "time_ms") ? "time_s" : cols[c], sizeof(header) - strlen(header) - 1);
      }
      r->table_csv = open_csv(r->cfg.output_dir, name, header);
      r->table_cols = nc;
      r->rows = malloc_or_fail(sizeof(double) * ROWS_AT_A_TIME * (size_t)nc);
    }
  }
  return r;
}

void nr_aiot_reader_free(nr_aiot_reader_t *r)
{
  if (!r)
    return;
  if (r->running) {
    pthread_mutex_lock(&r->lock);
    r->stop = 1;
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
  }
  if (r->mac)
    collect(r);
  if (r->sock >= 0)
    close(r->sock);
  if (r->peer_sock >= 0)
    close(r->peer_sock);
  free(r->msg);
  free(r->remote_outcome);
  if (r->tx_csv)
    fclose(r->tx_csv);
  if (r->rx_csv)
    fclose(r->rx_csv);
  if (r->table_csv)
    fclose(r->table_csv);
  aiot_reader_free(r->mac);
  free(r->outcomes);
  free(r->heard);
  free(r->rows);
  pthread_mutex_destroy(&r->lock);
  free(r);
}

/* ---------------------------------------------------------------------------------------------
 * The split: the UE that sends the R2D, or receives the D2R
 * ------------------------------------------------------------------------------------------- */

/* A radio timestamp of the node, in A-IoT samples */
static int64_t to_aiot(const nr_aiot_reader_t *r, int64_t ts)
{
  return (ts + r->decimation / 2) / r->decimation;
}

/* The Reader's now: the D2R receiver's progress, or with a remote D2R, the start of the slot the node
 * sent last. -1 before it has sent one. */
static int64_t reader_now(nr_aiot_reader_t *r)
{
  if (r->d2r)
    return nr_aiot_d2r_received_until(r->d2r);
  int64_t ts;
  return nr_aiot_r2d_now(r->r2d, &ts) < 0 || ts < 0 ? -1 : to_aiot(r, ts);
}

/* A-IoT sample t of the node as a time of the cell, on the slot the node sent last. Returns 0, or -1
 * before it has sent one. */
static int cell_time(nr_aiot_reader_t *r, int64_t t, aiot_cell_time_t *at)
{
  int64_t ts;
  const int slot = nr_aiot_r2d_now(r->r2d, &ts);
  if (slot < 0 || ts < 0)
    return -1;
  const int64_t delta = t - to_aiot(r, ts);
  const int64_t k = delta >= 0 ? delta / SLOT_SAMPLES : -((-delta + SLOT_SAMPLES - 1) / SLOT_SAMPLES);
  const int abs_slot = (int)(((slot + k) % AIOT_CELL_WRAP_SLOTS + AIOT_CELL_WRAP_SLOTS) % AIOT_CELL_WRAP_SLOTS);
  at->frame = abs_slot / AIOT_CELL_SLOTS_PER_FRAME;
  at->slot = abs_slot % AIOT_CELL_SLOTS_PER_FRAME;
  at->sample = (int)(delta - k * SLOT_SAMPLES);
  return 0;
}

static void peer_send(nr_aiot_reader_t *r, const struct sockaddr_in *to, int len, const char *what)
{
  nr_aiot_udp_send(r->peer_sock, to, r->msg, len, &r->peer_tx, what);
  r->stats.peer_unsent = r->peer_tx.unsent;
}

/* The lead from now on, at most NR_AIOT_READER_MAX_LEAD_SLOTS. */
static void set_lead(nr_aiot_reader_t *r, int64_t lead)
{
  r->lead = (int)cmin(lead, (int64_t)NR_AIOT_READER_MAX_LEAD_SLOTS);
}

static void stat_add(int64_t v, int64_t *sum, int64_t *min)
{
  *sum += v;
  *min = cmin(*min, v);
}

static void stat_add_max(int64_t v, int64_t *sum, int64_t *max)
{
  *sum += v;
  *max = cmax(*max, v);
}

/* The protocol's next R2D, to the UE that sends it for symbol `symbol` of (frame, slot). Returns its
 * symbols, as the UE's nr_aiot_r2d_schedule() gives them, or a negative AIOT_E* code. */
static int send_remote_r2d(nr_aiot_reader_t *r, int frame, int slot, int symbol, int64_t now)
{
  const int n_sym = nr_aiot_r2d_n_symbols(r->next.bits, r->next.n_bits, r->cfg.link.m_chip_symb);
  if (n_sym < 0)
    return n_sym;
  aiot_node_r2d_t m = {.seq = r->r2d_seq,
                       .frame = frame,
                       .slot = slot,
                       .symbol = symbol,
                       .m_chip_symb = r->cfg.link.m_chip_symb,
                       .n_rb = r->n_rb,
                       .a_bits = r->next.n_bits};
  memcpy(m.bits, r->next.bits, r->next.n_bits);
  const int len = aiot_node_msg_pack_r2d(&m, r->msg, AIOT_NODE_SPLIT_MAX_BYTES);
  if (len < 0)
    return len;
  r->sent_at[r->r2d_seq++ % SENT_RING] = now;
  r->probe_at = now;
  peer_send(r, &r->r2d_peer, len, "an R2D");
  return n_sym;
}

/* The MAC's window e gets an empty outcome at once: as if nothing was heard in it. */
static void give_up_window(nr_aiot_reader_t *r, const aiot_d2r_expect_t *e, int64_t now)
{
  aiot_d2r_outcome_t *out = &r->remote_outcome->outcome;
  out->expect = *e;
  out->n_decoded = 0;
  out->collided = 0;
  if (aiot_reader_d2r_outcome(r->mac, out, now) != 0)
    LOG_E(NR_MAC, "A-IoT Reader: the MAC did not take the empty outcome of the window at %ld\n", (long)e->start);
}

/* A window that opens nothing, to the UE that receives the D2R: it answers once it can. */
static void send_probe(nr_aiot_reader_t *r, int64_t now)
{
  const aiot_node_window_t w = {.expect = {.tag = AIOT_NODE_PROBE_TAG}};
  r->d2r_probe_at = now;
  r->stats.probes++;
  peer_send(r, &r->d2r_peer, aiot_node_msg_pack_window(&w, r->msg, AIOT_NODE_SPLIT_MAX_BYTES), "a probe window");
}

/* A window the MAC opened, to the UE that receives the D2R. */
static void open_remote_window(nr_aiot_reader_t *r, const aiot_d2r_expect_t *e, int64_t now)
{
  aiot_node_window_t w = {.expect = *e};
  int i = 0;
  while (i < MAX_EXPECTS && r->windows[i].used)
    i++;
  if (i == MAX_EXPECTS || cell_time(r, e->start, &w.at) != 0) {
    /* the UE's outcomes are this far behind: the protocol goes on without this window */
    r->stats.windows_lost++;
    LOG_E(NR_MAC, "A-IoT Reader: %d windows open at the UE: the window at %ld is given up as empty\n", MAX_EXPECTS, (long)e->start);
    give_up_window(r, e, now);
    return;
  }
  const int64_t end = e->start + e->te + aiot_frame_samples(e->a_bits, &e->params);
  r->windows[i] = (remote_window_t){.used = 1,
                                    .e = *e,
                                    .deadline = end + WINDOW_TIMEOUT_SLOTS * (int64_t)SLOT_SAMPLES,
                                    .lead = r->lead,
                                    .early = !r->d2r_up};
  r->stats.windows_remote++;
  peer_send(r, &r->d2r_peer, aiot_node_msg_pack_window(&w, r->msg, AIOT_NODE_SPLIT_MAX_BYTES), "a window");
}

static void on_r2d_done(nr_aiot_reader_t *r, int n, int64_t now)
{
  aiot_node_r2d_done_t d;
  if (aiot_node_msg_parse_r2d_done(r->msg, n, &d) != 0 || r->r2d_seq - d.seq > SENT_RING || d.seq >= r->r2d_seq) {
    r->stats.peer_refused++;
    return;
  }
  nr_aiot_reader_stats_t *s = &r->stats;
  s->r2d_done++;
  if (!r->peer_up) {
    r->peer_up = 1;
    r->up_seq = r->lead_seq = r->r2d_seq;
    LOG_I(NR_MAC, "A-IoT Reader: the UE that sends the R2D has answered (R2D %u, %d slots ahead)\n", d.seq, d.ahead);
  }
  if ((int32_t)(d.seq - r->up_seq) < 0) {
    /* sent before the UE answered, maybe before it had its user plane: they waited for it, and say
     * nothing of the delay or the lead */
    s->r2d_early++;
    if (d.result == NR_AIOT_R2D_ETOOLATE)
      s->r2d_early_late++;
    return;
  }
  stat_add(d.ahead, &s->r2d_ahead_sum, &s->r2d_ahead_min);
  stat_add_max(now - r->sent_at[d.seq % SENT_RING], &s->r2d_rtt_sum, &s->r2d_rtt_max);
  if (d.result == NR_AIOT_R2D_ETOOLATE) {
    s->too_late++;
    /* the UE says by how much it was late: the lead grows by that, and a margin. An R2D sent before
     * the lead last grew was planned with the shorter lead. */
    if ((int32_t)(d.seq - r->lead_seq) >= 0 && r->lead < NR_AIOT_READER_MAX_LEAD_SLOTS) {
      set_lead(r, (int64_t)r->lead + 1 - d.ahead + LEAD_MARGIN_SLOTS);
      r->lead_seq = r->r2d_seq;
    }
    if (aiot_log_due(s->too_late))
      LOG_W(NR_MAC,
            "A-IoT Reader: the UE got R2D %u too late (%d slots ahead); %d slots ahead from now on (%lu too late so far)\n",
            d.seq,
            d.ahead,
            r->lead,
            (unsigned long)s->too_late);
  } else if (d.result < 0) {
    if (aiot_log_due(++s->r2d_refused))
      LOG_E(NR_MAC,
            "A-IoT Reader: the UE cannot send R2D %u (%d; %lu refused so far)\n",
            d.seq,
            d.result,
            (unsigned long)s->r2d_refused);
  }
}

static int on_outcome(nr_aiot_reader_t *r, int n, int64_t now)
{
  aiot_node_outcome_t *o = r->remote_outcome;
  if (aiot_node_msg_parse_outcome(r->msg, n, o) != 0) {
    r->stats.peer_refused++;
    return 0;
  }
  aiot_d2r_outcome_t *out = &o->outcome;
  if (out->expect.tag == AIOT_NODE_PROBE_TAG) {
    if (!r->d2r_up) {
      r->d2r_up = 1;
      LOG_I(NR_MAC,
            "A-IoT Reader: the UE that receives the D2R has answered (%lu probe windows): the protocol starts\n",
            (unsigned long)r->stats.probes);
    }
    return 0;
  }
  if (!r->d2r_up) {
    r->d2r_up = 1;
    LOG_I(NR_MAC,
          "A-IoT Reader: the UE that receives the D2R has answered (window %ld, %.2f ms ahead)\n",
          (long)out->expect.tag,
          ms(o->ahead));
  }
  int i = 0;
  while (i < MAX_EXPECTS && !(r->windows[i].used && r->windows[i].e.tag == out->expect.tag))
    i++;
  if (i == MAX_EXPECTS) {
    LOG_W(NR_MAC, "A-IoT Reader: the outcome of window %ld came back after it was given up\n", (long)out->expect.tag);
    return 0;
  }
  remote_window_t *w = &r->windows[i];
  w->used = 0;
  nr_aiot_reader_stats_t *s = &r->stats;
  s->outcomes_remote++;
  if (w->early) {
    /* opened before the UE answered, maybe before it had its user plane: says nothing of the delay */
    s->outcomes_early++;
    if (o->ahead < 0)
      s->windows_early_late++;
  } else {
    stat_add(o->ahead, &s->window_ahead_sum, &s->window_ahead_min);
    stat_add_max(now - (w->deadline - WINDOW_TIMEOUT_SLOTS * (int64_t)SLOT_SAMPLES), &s->outcome_delay_sum, &s->outcome_delay_max);
    if (o->ahead < 0) {
      /* the UE opened it after its receiver had passed its start, and decoded what was left: the lead
       * grows by the slots it lacked, and a margin, unless it grew since the window was opened */
      s->windows_late++;
      if (w->lead == r->lead && r->lead < NR_AIOT_READER_MAX_LEAD_SLOTS)
        set_lead(r, (int64_t)r->lead + (-o->ahead + SLOT_SAMPLES - 1) / SLOT_SAMPLES + LEAD_MARGIN_SLOTS);
      if (aiot_log_due(s->windows_late))
        LOG_W(NR_MAC,
              "A-IoT Reader: the UE got the window at %.2f ms %.2f ms late; %d slots ahead from now on (%lu late so far)\n",
              ms(w->e.start),
              ms(-o->ahead),
              r->lead,
              (unsigned long)s->windows_late);
    }
  }
  /* back on the node's clock: the UE counts each D2R from the window's start */
  out->expect = w->e;
  for (int k = 0; k < out->n_decoded; k++) {
    out->decoded[k].start += w->e.start;
    out->decoded[k].tag = w->e.tag;
  }
  if (aiot_reader_d2r_outcome(r->mac, out, now) != 0)
    LOG_E(NR_MAC, "A-IoT Reader: an outcome for a window it did not open (%ld)\n", (long)w->e.start);
  return 1;
}

/* One datagram from a remote end: an answer the Reader waits for, from the UE it waits for it from. */
static void on_peer_datagram(void *ctx, const struct sockaddr_in *src, int n)
{
  nr_aiot_reader_t *r = ctx;
  const int type = aiot_node_msg_type(r->msg, n);
  if (type == AIOT_NODE_MSG_R2D_DONE && r->remote_r2d && nr_aiot_udp_same(src, &r->r2d_peer))
    on_r2d_done(r, n, r->now);
  else if (type == AIOT_NODE_MSG_OUTCOME && r->remote_d2r && nr_aiot_udp_same(src, &r->d2r_peer))
    on_outcome(r, n, r->now);
  else
    r->stats.peer_refused++;
}

/* The answers of the remote ends that came in. */
static int take_peer_messages(nr_aiot_reader_t *r, int64_t now)
{
  r->now = now;
  return nr_aiot_udp_drain(r->peer_sock, r->msg, AIOT_NODE_SPLIT_MAX_BYTES + 1, on_peer_datagram, r, &r->recv_errno);
}

/* Gives the MAC an empty outcome for each remote window whose outcome has not come back in time, so
 * that the protocol goes on, as after a window in which nothing was heard. */
static int give_up_windows(nr_aiot_reader_t *r, int64_t now)
{
  int acted = 0;
  for (int i = 0; i < MAX_EXPECTS; i++) {
    remote_window_t *w = &r->windows[i];
    if (!w->used || now <= w->deadline)
      continue;
    w->used = 0;
    if (aiot_log_due(++r->stats.windows_lost))
      LOG_W(NR_MAC,
            "A-IoT Reader: no outcome came back for the window at %.3f ms: given up as empty (%lu so far)\n",
            ms(w->e.start),
            (unsigned long)r->stats.windows_lost);
    give_up_window(r, &w->e, now);
    acted++;
  }
  return acted;
}

static void report_split(nr_aiot_reader_t *r, int64_t now)
{
  if (r->next_report == 0)
    r->next_report = now + REPORT_SAMPLES;
  if (now < r->next_report)
    return;
  r->next_report = now + REPORT_SAMPLES;
  const nr_aiot_reader_stats_t *s = &r->stats;
  const uint64_t timed = s->r2d_done - s->r2d_early;
  if (r->remote_r2d && timed)
    LOG_I(NR_MAC,
          "A-IoT Reader: split R2D: %lu sent by the UE, %lu too late, %lu refused; lead %d slots; the UE had %ld slots in hand at "
          "least, %.1f on average; %.2f ms round trip on average, %.2f at most (before it answered: %lu sent, %lu too late)\n",
          (unsigned long)timed,
          (unsigned long)s->too_late,
          (unsigned long)s->r2d_refused,
          r->lead,
          (long)s->r2d_ahead_min,
          (double)s->r2d_ahead_sum / timed,
          ms(s->r2d_rtt_sum) / timed,
          ms(s->r2d_rtt_max),
          (unsigned long)s->r2d_early,
          (unsigned long)s->r2d_early_late);
  const uint64_t timed_outcomes = s->outcomes_remote - s->outcomes_early;
  if (r->remote_d2r && s->windows_remote)
    LOG_I(NR_MAC,
          "A-IoT Reader: split D2R: %lu windows, %lu outcomes back, %lu late, %lu given up; lead %d slots; the UE had %.2f ms in "
          "hand at least, %.2f on average; an outcome came %.2f ms after its window on average, %.2f at most (before it "
          "answered: %lu outcomes back, %lu late)\n",
          (unsigned long)s->windows_remote,
          (unsigned long)timed_outcomes,
          (unsigned long)s->windows_late,
          (unsigned long)s->windows_lost,
          r->lead,
          timed_outcomes ? ms(s->window_ahead_min) : 0.0,
          timed_outcomes ? ms(s->window_ahead_sum) / timed_outcomes : 0.0,
          timed_outcomes ? ms(s->outcome_delay_sum) / timed_outcomes : 0.0,
          ms(s->outcome_delay_max),
          (unsigned long)s->outcomes_early,
          (unsigned long)s->windows_early_late);
  if (s->peer_unsent || s->peer_refused)
    LOG_W(NR_MAC,
          "A-IoT Reader: split: %lu messages not sent to the UE, %lu datagrams refused\n",
          (unsigned long)s->peer_unsent,
          (unsigned long)s->peer_refused);
}

/* ---------------------------------------------------------------------------------------------
 * The Reader's passes
 * ------------------------------------------------------------------------------------------- */

/* Puts the protocol's next R2D on the grid, on the node's clock: the slot it mapped last starts at
 * its radio timestamp, A-IoT sample ts / D of the D2R receiver. With a remote R2D, sends it to the
 * UE for the same time of the cell. Returns 1 when sent with windows to wait for, 2 when sent without
 * any (the next R2D is ready), 0 when it waits for a later pass, or -1 when the node cannot send it at
 * all. */
static int send_next(nr_aiot_reader_t *r, int64_t now)
{
  int64_t tx_ts;
  const int tx_slot = nr_aiot_r2d_now(r->r2d, &tx_ts);
  if (tx_slot < 0 || tx_ts < 0)
    return 0; /* the node has sent nothing yet */
  /* Until the UE that sends the R2D has answered (it may not have its user plane yet), one R2D at a
   * time: those the gNB would send meanwhile would wait for it, and reach it late all together. */
  if (r->remote_r2d && !r->peer_up && r->r2d_seq > 0 && now - r->probe_at < PROBE_SLOTS * (int64_t)SLOT_SAMPLES)
    return 0;
  /* Until the UE that receives the D2R has answered (it may not have its user plane yet), no R2D at
   * all: the D2Rs the Tags would send meanwhile go into windows the UE never gets, and a Tag that
   * answered a paging there may not answer it again. A probe window every PROBE_SLOTS instead. */
  if (r->remote_d2r && !r->d2r_up) {
    if (r->stats.probes == 0 || now - r->d2r_probe_at >= PROBE_SLOTS * (int64_t)SLOT_SAMPLES)
      send_probe(r, now);
    return 0;
  }
  const int64_t tx_start = to_aiot(r, tx_ts); /* the slot mapped last, in A-IoT samples */
  int64_t start = tx_start + (int64_t)(1 + r->lead) * SLOT_SAMPLES;
  if (r->next.earliest_start > start)
    start = r->next.earliest_start;
  /* the node's next half-subframe from there: symbol 0 or 7 of slot tx_slot + k / 2 */
  const int64_t k = (start - tx_start + AIOT_HALF_SUBFRAME - 1) / AIOT_HALF_SUBFRAME;
  start = tx_start + k * AIOT_HALF_SUBFRAME;
  const int slot = (int)((tx_slot + k / 2) % AIOT_CELL_WRAP_SLOTS);
  const int frame = slot / AIOT_CELL_SLOTS_PER_FRAME, sl = slot % AIOT_CELL_SLOTS_PER_FRAME;
  const int symbol = k % 2 ? NR_SYMBOLS_PER_SLOT / 2 : 0;
  int n_sym = r->remote_r2d
                  ? send_remote_r2d(r, frame, sl, symbol, now)
                  : nr_aiot_r2d_schedule(r->r2d, frame, sl, symbol, r->next.bits, r->next.n_bits, r->cfg.link.m_chip_symb, r->n_rb);
  if (n_sym == NR_AIOT_R2D_ETOOLATE) {
    r->stats.too_late++;
    set_lead(r, 2 * (int64_t)r->lead);
    if (aiot_log_due(r->stats.too_late))
      LOG_W(NR_MAC,
            "A-IoT Reader: the R2D for %d.%d was ready too late; %d slots ahead from now on (%lu too late so far)\n",
            frame,
            sl,
            r->lead,
            (unsigned long)r->stats.too_late);
    return 0;
  }
  if (n_sym == AIOT_ESPACE)
    return 0;
  if (n_sym < 0) {
    LOG_E(NR_MAC, "A-IoT Reader: the node cannot send the R2D at %d.%d.%d (%d); the Reader stops\n", frame, sl, symbol, n_sym);
    return -1;
  }
  const int64_t end = start + aiot_r2d_n_samples(n_sym);
  int n = aiot_reader_r2d_sent(r->mac, start, end, now, r->expects, MAX_EXPECTS);
  r->have_next = 0;
  r->stats.r2d_sent++;
  for (int i = 0; i < n; i++)
    if (r->remote_d2r)
      open_remote_window(r, &r->expects[i], now);
    else if (nr_aiot_d2r_expect(r->d2r, &r->expects[i]) != 0)
      LOG_E(NR_MAC, "A-IoT Reader: the D2R receiver refused a window at %ld\n", (long)r->expects[i].start);
  LOG_D(NR_MAC, "A-IoT Reader: R2D in %d.%d from symbol %d, %d symbols, %d windows\n", frame, sl, symbol, n_sym, n);
  return n > 0 ? 1 : 2; /* 2: nothing to wait for, the next R2D is ready */
}

int nr_aiot_reader_step(nr_aiot_reader_t *r)
{
  pthread_mutex_lock(&r->lock);
  const int64_t now = reader_now(r);
  if (now < 0) {
    pthread_mutex_unlock(&r->lock);
    return 0; /* the node has sent no slot yet */
  }
  int acted = 0, k;
  while (r->d2r && (k = nr_aiot_d2r_pop(r->d2r, r->outcomes, MAX_OUTCOMES)) > 0)
    for (int i = 0; i < k; i++) {
      if (aiot_reader_d2r_outcome(r->mac, &r->outcomes[i], now) != 0)
        LOG_E(NR_MAC, "A-IoT Reader: an outcome for a window it did not open (%ld)\n", (long)r->outcomes[i].expect.start);
      acted++;
    }
  if (r->peer_sock >= 0) {
    acted += take_peer_messages(r, now);
    if (r->remote_d2r)
      acted += give_up_windows(r, now);
    report_split(r, now);
  }
  collect(r);
  for (;;) {
    if (!r->have_next) {
      if (aiot_reader_next_r2d(r->mac, now, &r->next) != 1)
        break;
      r->have_next = 1;
    }
    int rc = send_next(r, now);
    if (rc < 0) {
      r->have_next = 0;
      r->ended = 1;
      break;
    }
    if (rc == 0)
      break;
    acted++;
    collect(r);
    if (rc == 1)
      break;
  }
  if (aiot_reader_done(r->mac) && aiot_reader_idle(r->mac) && !r->ended) {
    r->ended = 1;
    collect(r);
    LOG_I(NR_MAC,
          "A-IoT Reader: the protocol %s has ended: %lu R2Ds, %lu answers, %lu collisions\n",
          r->cfg.protocol,
          (unsigned long)r->stats.r2d_sent,
          (unsigned long)r->stats.answers,
          (unsigned long)r->stats.collisions);
    if (r->sock >= 0)
      LOG_I(NR_MAC,
            "A-IoT Reader: %lu Tag data messages sent to %s, %lu not sent\n",
            (unsigned long)r->stats.data_sent,
            r->cfg.data_dest,
            (unsigned long)r->stats.data_unsent);
  }
  if (acted)
    flush_files(r);
  pthread_mutex_unlock(&r->lock);
  return acted;
}

int nr_aiot_reader_done(nr_aiot_reader_t *r)
{
  pthread_mutex_lock(&r->lock);
  int d = r->ended;
  pthread_mutex_unlock(&r->lock);
  return d;
}

nr_aiot_reader_stats_t nr_aiot_reader_stats(nr_aiot_reader_t *r)
{
  pthread_mutex_lock(&r->lock);
  nr_aiot_reader_stats_t s = r->stats;
  pthread_mutex_unlock(&r->lock);
  return s;
}

static void *reader_thread(void *arg)
{
  nr_aiot_reader_t *r = arg;
  int64_t seen = -1;
  for (;;) {
    pthread_mutex_lock(&r->lock);
    int stop = r->stop;
    pthread_mutex_unlock(&r->lock);
    if (stop)
      break;
    nr_aiot_reader_step(r);
    if (r->d2r) {
      seen = nr_aiot_d2r_wait(r->d2r, seen, 20); /* the next slot received */
    } else {
      struct pollfd fd = {.fd = r->peer_sock, .events = POLLIN};
      poll(&fd, 1, 1); /* an outcome, or a millisecond for the node's next slot */
    }
  }
  return NULL;
}

int nr_aiot_reader_start(nr_aiot_reader_t *r)
{
  if (r->running)
    return -1;
  threadCreate(&r->thread, reader_thread, r, "aiot_reader", -1, OAI_PRIORITY_RT_LOW);
  r->running = 1;
  return 0;
}
