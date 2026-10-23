/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file nr_aiot_reader.c
 * \brief An NR node as an Ambient IoT Reader: the Reader MAC on the node's R2D and D2R (nr_aiot_reader.h).
 */

#include "nr_aiot_reader.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "common/utils/LOG/log.h"
#include "common/utils/system.h"

#define SLOT_SAMPLES AIOT_SAMPLES_PER_MS /* one 15 kHz slot */
#define SLOTS_PER_FRAME 10
#define FRAMES 1024
#define WRAP_SLOTS (FRAMES * SLOTS_PER_FRAME)
#define MAX_LEAD_SLOTS 64
#define MAX_EXPECTS 64
#define MAX_OUTCOMES 8
#define FULL_SCALE2 (32767.0 * 32767.0) /* counts squared of a full-scale sample */

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
  snprintf(detail, len, "%s", "");
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
    int node = -1, value = -1;
    char id[16] = "";
    if (a->kind == AIOT_ANSWER_MSG1) {
      snprintf(id, sizeof(id), "%d", a->message.random_id);
    } else {
      read_sdu(&a->message, &node, &value);
      if (a->as_id >= 0)
        snprintf(id, sizeof(id), "%d", a->as_id);
      LOG_I(NR_MAC,
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
  while ((n = aiot_reader_pop_rows(r->mac, r->rows, 64)) > 0)
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
  if (!r2d || r2d->cfg.prb_start < 0 || !d2r) {
    snprintf(err, err_len, "the Reader needs the node's R2D PRBs and D2R PRBs (aiot.r2d_prb_start, aiot.d2r_prb_start)");
    return NULL;
  }
  nr_aiot_reader_t *r = calloc(1, sizeof(*r));
  if (!r) {
    snprintf(err, err_len, "out of memory");
    return NULL;
  }
  r->cfg = *cfg;
  for (int i = 0; i < r->cfg.n_params; i++)
    r->cfg.params[i].name = r->cfg.param_names[i];
  r->r2d = r2d;
  r->d2r = d2r;
  r->lead = cfg->lead_slots > 0 ? cfg->lead_slots : 2;
  pthread_mutex_init(&r->lock, NULL);
  r->outcomes = malloc(sizeof(*r->outcomes) * MAX_OUTCOMES);
  r->heard = malloc(sizeof(*r->heard));
  r->mac =
      aiot_reader_new(&r->cfg.link, r->cfg.protocol, r->cfg.params, r->cfg.n_params, r->cfg.node_ids, r->cfg.n_nodes, err, err_len);
  if (!r->outcomes || !r->heard || !r->mac) {
    if (r->mac == NULL && err[0] == 0)
      snprintf(err, err_len, "out of memory");
    nr_aiot_reader_free(r);
    return NULL;
  }
  r->n_rb = aiot_link_n_rb(&r->cfg.link);
  if (r->n_rb > r2d->cfg.n_rb_max) {
    snprintf(err, err_len, "the R2D needs %d PRBs, and the node reserves %d (aiot.r2d_n_rb)", r->n_rb, r2d->cfg.n_rb_max);
    nr_aiot_reader_free(r);
    return NULL;
  }
  if (r->cfg.output_dir[0]) {
    mkdir(r->cfg.output_dir, 0777);
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
      r->rows = malloc(sizeof(double) * 64 * (size_t)nc);
      if (!r->rows) {
        fclose(r->table_csv);
        r->table_csv = NULL;
      }
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

/* Puts the protocol's next R2D on the grid, on the node's clock: the slot it mapped last starts at
 * its radio timestamp, A-IoT sample ts / D of the D2R receiver. Returns 1 when sent, 0 when it waits
 * for a later pass, or -1 when the node cannot send it at all. */
static int send_next(nr_aiot_reader_t *r, int64_t now)
{
  int64_t tx_ts;
  const int tx_slot = nr_aiot_r2d_now(r->r2d, &tx_ts);
  if (tx_slot < 0 || tx_ts < 0)
    return 0; /* the node has sent nothing yet */
  const int d = nr_aiot_d2r_decimation(r->d2r);
  const int64_t tx_start = (tx_ts + d / 2) / d; /* the slot mapped last, in A-IoT samples */
  int64_t start = tx_start + (int64_t)(1 + r->lead) * SLOT_SAMPLES;
  if (r->next.earliest_start > start)
    start = r->next.earliest_start;
  /* the node's next half-subframe from there: symbol 0 or 7 of slot tx_slot + k / 2 */
  const int64_t k = (start - tx_start + AIOT_HALF_SUBFRAME - 1) / AIOT_HALF_SUBFRAME;
  start = tx_start + k * AIOT_HALF_SUBFRAME;
  const int slot = (int)((tx_slot + k / 2) % WRAP_SLOTS);
  const int frame = slot / SLOTS_PER_FRAME, sl = slot % SLOTS_PER_FRAME, symbol = k % 2 ? 7 : 0;
  int n_sym = nr_aiot_r2d_schedule(r->r2d, frame, sl, symbol, r->next.bits, r->next.n_bits, r->cfg.link.m_chip_symb, r->n_rb);
  if (n_sym == NR_AIOT_R2D_ETOOLATE) {
    r->stats.too_late++;
    if (r->lead < MAX_LEAD_SLOTS)
      r->lead *= 2;
    LOG_W(NR_MAC, "A-IoT Reader: the R2D for %d.%d was ready too late; %d slots ahead from now on\n", frame, sl, r->lead);
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
    if (nr_aiot_d2r_expect(r->d2r, &r->expects[i]) != 0)
      LOG_E(NR_MAC, "A-IoT Reader: the D2R receiver refused a window at %ld\n", (long)r->expects[i].start);
  LOG_D(NR_MAC, "A-IoT Reader: R2D in %d.%d from symbol %d, %d symbols, %d windows\n", frame, sl, symbol, n_sym, n);
  return n > 0 ? 1 : 2; /* 2: nothing to wait for, the next R2D is ready */
}

int nr_aiot_reader_step(nr_aiot_reader_t *r)
{
  pthread_mutex_lock(&r->lock);
  const int64_t now = nr_aiot_d2r_received_until(r->d2r);
  int acted = 0, k;
  while ((k = nr_aiot_d2r_pop(r->d2r, r->outcomes, MAX_OUTCOMES)) > 0)
    for (int i = 0; i < k; i++) {
      if (aiot_reader_d2r_outcome(r->mac, &r->outcomes[i], now) != 0)
        LOG_E(NR_MAC, "A-IoT Reader: an outcome for a window it did not open (%ld)\n", (long)r->outcomes[i].expect.start);
      acted++;
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
    seen = nr_aiot_d2r_wait(r->d2r, seen, 20);
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
