/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiotsim.c
 * \brief A-IoT link-level simulator: R2D detection and D2R BLER of openair1/PHY/AIOT against SNR,
 * and a check at the sensitivity points of 38.191 and 38.194. Run aiot_sim -h for the options.
 */

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openair1/PHY/AIOT/aiot_phy.h"
#include "SIMULATION/TOOLS/sim.h"

/* ---------------------------------------------------------------------------------------------
 * Noise and random bits: OAI's generators (randominit, gaussZiggurat, taus), seeded as the NR
 * physims are, from --seed through OAI_RNGSEED and set_taus_seed
 * ------------------------------------------------------------------------------------------- */

/* every OAI executable provides this; AssertFatal in OAI's code ends here */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "Exiting at: %s:%d %s(), %s\n", file, line, function, s ? s : "no comment");
  exit(1);
}

static void seed_rng(uint64_t seed)
{
  char s[32];
  snprintf(s, sizeof(s), "%llu", (unsigned long long)seed);
  setenv("OAI_RNGSEED", s, 1);
  randominit();
  set_taus_seed((unsigned int)seed);
}

static void add_awgn(cd_t *x, int n, double noise_w)
{
  double s = sqrt(noise_w / 2);
  for (int i = 0; i < n; i++) {
    x[i].r += s * gaussZiggurat(0.0, 1.0);
    x[i].i += s * gaussZiggurat(0.0, 1.0);
  }
}

static void random_bits(uint8_t *b, int n)
{
  for (int i = 0; i < n; i++)
    b[i] = taus() & 1;
}

/* ---------------------------------------------------------------------------------------------
 * Phase noise of the Reader's carrier as it leaks into its receiver
 *
 * phi(t) = a white part (flat L(f) = floor) + a random walk (L(f) = floor * (corner / f)^2), so
 * L(f) falls as 1/f^2 up to the corner and is flat beyond it. For small phi, L(f) is the power of
 * one sideband per Hz relative to the carrier:
 *   white:       variance per sample  = L * fs
 *   random walk: increment variance   = 4 pi^2 f^2 L(f) / fs = 4 pi^2 corner^2 floor / fs
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  double sigma_white; /* rad per sample */
  double sigma_step; /* rad per sample of the random walk */
  double walk; /* the random walk's phase now */
} phase_noise_t;

static void phase_noise_init(phase_noise_t *pn, double floor_dbc_hz, double corner_hz, double phase0)
{
  double L = floor_dbc_hz > -300 ? pow(10.0, floor_dbc_hz / 10) : 0;
  pn->sigma_white = sqrt(L * AIOT_SAMPLE_RATE_HZ);
  pn->sigma_step = sqrt(4 * M_PI * M_PI * corner_hz * corner_hz * L / AIOT_SAMPLE_RATE_HZ);
  pn->walk = phase0;
}

/* Add the leak cw * exp(j phi) to y[0..n-1]. */
static void add_leak(phase_noise_t *pn, double cw, cd_t *y, int n)
{
  for (int i = 0; i < n; i++) {
    double phi = pn->walk;
    if (pn->sigma_white > 0)
      phi += pn->sigma_white * gaussZiggurat(0.0, 1.0);
    if (pn->sigma_step > 0)
      pn->walk += pn->sigma_step * gaussZiggurat(0.0, 1.0);
    y[i].r += cw * cos(phi);
    y[i].i += cw * sin(phi);
  }
}

/* ---------------------------------------------------------------------------------------------
 * R2D
 * ------------------------------------------------------------------------------------------- */

/* Fraction of trials in which the device decodes the transmitted TB, at snr_db. */
static double r2d_detection(int m, int n_rb, int tbs, double snr_db, int trials)
{
  const int lead = 1000, tail = 3000;
  int cap = 1 << 22;
  cd_t *tx = malloc(sizeof(*tx) * cap);
  cd_t *y = malloc(sizeof(*y) * cap);
  aiot_r2d_frame_t *frames = malloc(sizeof(*frames) * 4);
  uint8_t tb[AIOT_MAX_TB_BITS];
  double noise_w = pow(10.0, -snr_db / 10); /* an "on" chip has power 1 */
  int ok = 0;
  for (int t = 0; t < trials; t++) {
    random_bits(tb, tbs);
    int n = aiot_r2d_transmission(tb, tbs, m, n_rb, tx, cap);
    if (n < 0) {
      fprintf(stderr, "r2d: invalid parameters\n");
      exit(2);
    }
    int total = lead + n + tail;
    memset(y, 0, sizeof(*y) * total);
    memcpy(y + lead, tx, sizeof(*tx) * n);
    add_awgn(y, total, noise_w);
    aiot_r2d_rx_t *rx = aiot_r2d_rx_new();
    int got = 0;
    for (int at = 0; at < total; at += AIOT_SAMPLES_PER_MS) {
      int len = total - at < AIOT_SAMPLES_PER_MS ? total - at : AIOT_SAMPLES_PER_MS;
      int k = aiot_r2d_rx_receive(rx, y + at, len, frames, 4);
      for (int i = 0; i < k; i++)
        if (frames[i].n_bits == tbs && !memcmp(frames[i].bits, tb, tbs))
          got = 1;
    }
    aiot_r2d_rx_free(rx);
    ok += got;
  }
  free(tx);
  free(y);
  free(frames);
  return (double)ok / trials;
}

/* ---------------------------------------------------------------------------------------------
 * D2R
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  const char *name;
  aiot_d2r_params_t p;
  double refsens_dbm; /* 38.194 Table 7.2.2-1 */
} frc_t;

/* 38.194 Annex B.1: 96-bit TB, 31-bit preamble, midamble codepoint 48 (scaled by S), R_SFS 1. */
static const frc_t frcs[] = {
    {"A1-1", {2, 1, 1, 1, 1, 31, 48, 0, AIOT_BPSK}, -95.2},
    {"A1-2", {2, 1, 1, 1, 1, 31, 48, 0, AIOT_OOK}, -92.2},
    {"A1-3", {1, 96, 1, 1, 0, 31, 9216, 0, AIOT_BPSK}, -72.4},
    {"A1-4", {1, 96, 1, 1, 0, 31, 9216, 0, AIOT_OOK}, -69.4},
};
#define FRC_TB_BITS 96

static const frc_t *find_frc(const char *name)
{
  for (size_t i = 0; i < sizeof(frcs) / sizeof(frcs[0]); i++)
    if (!strcmp(frcs[i].name, name))
      return &frcs[i];
  return NULL;
}

static double dbm_to_w(double dbm)
{
  return pow(10.0, (dbm - 30) / 10);
}

/* Thermal noise over the 5.76 MHz the receiver samples, at noise figure nf_db. */
static double thermal_noise_w(double nf_db)
{
  return 1.380649e-23 * 290.0 * AIOT_SAMPLE_RATE_HZ * pow(10.0, nf_db / 10);
}

/* BLER of the FRC at a mean received backscatter power of power_dbm. The Reader expects the frame
 * at a known start within +-Te, Te = 10% of T_R->D for a data D2R; the true start is drawn in
 * that window. */
static double d2r_bler(const frc_t *f, double power_dbm, double nf_db, double cw_dbm, double pn_floor, double pn_corner, int trials)
{
  const aiot_d2r_params_t *p = &f->p;
  int n = aiot_frame_samples(FRC_TB_BITS, p);
  int te = aiot_timing_tolerance(aiot_t_r_to_d(p, 6, AIOT_DATA, FRC_TB_BITS / 8, NULL, 0, 0, 0));
  int lead = te + 2 * AIOT_SAMPLES_PER_MS, total = lead + n + te + AIOT_SAMPLES_PER_MS;
  double *gamma = malloc(sizeof(*gamma) * n);
  cd_t *y = malloc(sizeof(*y) * total);
  aiot_d2r_outcome_t *out = malloc(sizeof(*out) * 2);
  uint8_t tb[FRC_TB_BITS];
  double noise_w = thermal_noise_w(nf_db);
  /* mean power of the reflection: 1 for BPSK (+-1), 1/2 for OOK (0/1) */
  double amp = sqrt(dbm_to_w(power_dbm) / (p->modulation == AIOT_OOK ? 0.5 : 1.0));
  double cw = sqrt(dbm_to_w(cw_dbm));
  int errors = 0;
  for (int t = 0; t < trials; t++) {
    random_bits(tb, FRC_TB_BITS);
    aiot_d2r_waveform(tb, FRC_TB_BITS, p, gamma, n);
    int start = lead + (int)(taus() % (unsigned)(2 * te + 1)) - te;
    double ph = 2 * M_PI * uniformrandom();
    phase_noise_t pn;
    phase_noise_init(&pn, pn_floor, pn_corner, 2 * M_PI * uniformrandom());
    memset(y, 0, sizeof(*y) * total);
    add_leak(&pn, cw, y, total);
    for (int i = 0; i < n; i++) {
      y[start + i].r += amp * gamma[i] * cos(ph);
      y[start + i].i += amp * gamma[i] * sin(ph);
    }
    add_awgn(y, total, noise_w);
    aiot_d2r_rx_t *rx = aiot_d2r_rx_new(noise_w, 0);
    aiot_d2r_expect_t e = {.start = lead, .te = te, .a_bits = FRC_TB_BITS, .params = *p, .n_r_sfs = 0, .tag = 0};
    aiot_d2r_rx_expect(rx, &e);
    int good = 0;
    for (int at = 0; at < total; at += AIOT_SAMPLES_PER_MS) {
      int len = total - at < AIOT_SAMPLES_PER_MS ? total - at : AIOT_SAMPLES_PER_MS;
      int k = aiot_d2r_rx_receive(rx, y + at, len, out, 2);
      for (int i = 0; i < k; i++)
        for (int j = 0; j < out[i].n_decoded; j++)
          if (!memcmp(out[i].decoded[j].bits, tb, FRC_TB_BITS))
            good = 1;
    }
    aiot_d2r_rx_free(rx);
    errors += !good;
  }
  free(gamma);
  free(y);
  free(out);
  return (double)errors / trials;
}

/* ---------------------------------------------------------------------------------------------
 * Command line
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  int m, n_rb, tbs, trials;
  double from, to, step, nf, cw, pn_floor, pn_corner;
  const char *frc;
  uint64_t seed;
} opts_t;

static void usage(FILE *f)
{
  fprintf(f,
          "usage: aiot_sim r2d|d2r|check [options]\n"
          "\n"
          "  r2d    R2D detection rate against SNR (power of an \"on\" chip over the device's noise)\n"
          "         -m M          chips per symbol, 2, 6, 12 or 24 (default 6)\n"
          "         -n N_RB       resource blocks (default 1)\n"
          "         -b BITS       transport block size (default 96: A-FR1-B1-1 of 38.194 B.2)\n"
          "         -f/-t/-s DB   SNR from, to, step (default 0, 16, 1)\n"
          "\n"
          "  d2r    D2R BLER against the mean received backscatter power, with thermal noise over\n"
          "         5.76 MHz and the Reader's carrier leaking into its receiver with phase noise\n"
          "         -F FRC        A1-1, A1-2, A1-3 or A1-4 of 38.194 B.1 (default A1-1)\n"
          "         -f/-t/-s DBM  power from, to, step (default REFSENS - 8 dB to REFSENS + 4 dB, 1 dB)\n"
          "         -N DB         noise figure (default 6)\n"
          "         -c DBM        carrier leak (default -38, the note of 38.194 Table 7.2.2-1)\n"
          "         -p DBC_HZ     phase noise floor (default -103; -1000 turns phase noise off)\n"
          "         -k HZ         phase noise corner: 1/f^2 below it (default 5300)\n"
          "         With the default leak and phase noise, each FRC reaches 10%% BLER 1 to 1.5 dB\n"
          "         below its REFSENS.\n"
          "\n"
          "  check  pass or fail at the spec points, with the d2r defaults; exits 1 on a failure:\n"
          "         the device detects each A-FR1-B1-x FRC at least 90%% of the time at 10 dB SNR,\n"
          "         and the Reader reaches BLER <= 10%% at each A-FR1-A1-x REFSENS of 38.194\n"
          "         Table 7.2.2-1, but not 3 dB below it\n"
          "\n"
          "  common -T N          trials per point (default 200)\n"
          "         -S SEED       random seed (default 1)\n"
          "         -h            this help\n");
}

static double number(int opt, const char *v)
{
  char *end;
  double d = strtod(v, &end);
  if (end == v || *end != '\0') {
    fprintf(stderr, "aiot_sim: -%c needs a number, not \"%s\"\n", opt, v);
    exit(2);
  }
  return d;
}

static void parse(int argc, char **argv, opts_t *o)
{
  int c;
  optind = 2;
  while ((c = getopt(argc, argv, "m:n:b:f:t:s:F:N:c:p:k:T:S:h")) != -1) {
    switch (c) {
      case 'm':
        o->m = (int)number(c, optarg);
        break;
      case 'n':
        o->n_rb = (int)number(c, optarg);
        break;
      case 'b':
        o->tbs = (int)number(c, optarg);
        break;
      case 'f':
        o->from = number(c, optarg);
        break;
      case 't':
        o->to = number(c, optarg);
        break;
      case 's':
        o->step = number(c, optarg);
        break;
      case 'F':
        o->frc = optarg;
        break;
      case 'N':
        o->nf = number(c, optarg);
        break;
      case 'c':
        o->cw = number(c, optarg);
        break;
      case 'p':
        o->pn_floor = number(c, optarg);
        break;
      case 'k':
        o->pn_corner = number(c, optarg);
        break;
      case 'T':
        o->trials = (int)number(c, optarg);
        break;
      case 'S':
        o->seed = (uint64_t)number(c, optarg);
        break;
      case 'h':
        usage(stdout);
        exit(0);
      default:
        usage(stderr);
        exit(2);
    }
  }
  if (optind < argc || o->trials <= 0 || o->step <= 0) {
    usage(stderr);
    exit(2);
  }
}

/* 38.194 Annex B.2 R2D FRCs: (M, PRB) with a 96-bit TB */
static const int r2d_frcs[3][2] = {{6, 1}, {12, 2}, {24, 3}};

static int check(const opts_t *o)
{
  seed_rng(o->seed);
  int fail = 0;
  printf("R2D: device detection at 10 dB SNR (38.191 7.2.2: >= 90%%)\n");
  for (int i = 0; i < 3; i++) {
    double d = r2d_detection(r2d_frcs[i][0], r2d_frcs[i][1], 96, 10.0, o->trials);
    int ok = d >= 0.9;
    fail |= !ok;
    printf("  A-FR1-B1-%d (M=%d, %d PRB): %.1f%%  %s\n", i + 1, r2d_frcs[i][0], r2d_frcs[i][1], 100 * d, ok ? "pass" : "FAIL");
  }
  printf(
      "D2R: Reader BLER at REFSENS (38.194 7.2.2: <= 10%%) and 3 dB below it (> 10%%: no better than a\n"
      "     real Reader), NF %.1f dB, CW %.1f dBm, phase noise floor %.1f dBc/Hz, 1/f^2 below %.0f Hz\n",
      o->nf,
      o->cw,
      o->pn_floor,
      o->pn_corner);
  for (size_t i = 0; i < sizeof(frcs) / sizeof(frcs[0]); i++) {
    double b = d2r_bler(&frcs[i], frcs[i].refsens_dbm, o->nf, o->cw, o->pn_floor, o->pn_corner, o->trials);
    double b3 = d2r_bler(&frcs[i], frcs[i].refsens_dbm - 3, o->nf, o->cw, o->pn_floor, o->pn_corner, o->trials);
    int ok = b <= 0.1 && b3 > 0.1;
    fail |= !ok;
    printf("  A-FR1-%s: BLER %.1f%% at %.1f dBm, %.1f%% at %.1f dBm  %s\n",
           frcs[i].name,
           100 * b,
           frcs[i].refsens_dbm,
           100 * b3,
           frcs[i].refsens_dbm - 3,
           ok ? "pass" : "FAIL");
  }
  return fail;
}

int main(int argc, char **argv)
{
  /* the defaults of aiot_sim -h; with them the four A-FR1-A1-x FRCs reach 10% BLER just below REFSENS */
  opts_t o = {6, 1, 96, 200, 0, 0, 1, 6.0, -38.0, -103.0, 5300.0, "A1-1", 1};
  if (argc < 2 || !strcmp(argv[1], "-h")) {
    usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? 2 : 0;
  }
  if (!strcmp(argv[1], "r2d")) {
    o.from = 0, o.to = 16;
    parse(argc, argv, &o);
    seed_rng(o.seed);
    printf("# R2D M=%d N_RB=%d TB=%d bits, %d trials\n# snr_db detection\n", o.m, o.n_rb, o.tbs, o.trials);
    for (double s = o.from; s <= o.to + 1e-9; s += o.step)
      printf("%.2f %.4f\n", s, r2d_detection(o.m, o.n_rb, o.tbs, s, o.trials));
    return 0;
  }
  if (!strcmp(argv[1], "d2r")) {
    parse(argc, argv, &o);
    const frc_t *f = find_frc(o.frc);
    if (!f) {
      fprintf(stderr, "aiot_sim: unknown FRC %s (A1-1, A1-2, A1-3 or A1-4)\n", o.frc);
      return 2;
    }
    if (o.from == 0 && o.to == 0)
      o.from = f->refsens_dbm - 8, o.to = f->refsens_dbm + 4;
    seed_rng(o.seed);
    printf(
        "# D2R A-FR1-%s, REFSENS %.1f dBm, NF %.1f dB, CW %.1f dBm, phase noise floor %.1f dBc/Hz corner %.0f Hz, %d trials\n# "
        "power_dbm bler\n",
        f->name,
        f->refsens_dbm,
        o.nf,
        o.cw,
        o.pn_floor,
        o.pn_corner,
        o.trials);
    for (double pw = o.from; pw <= o.to + 1e-9; pw += o.step)
      printf("%.2f %.4f\n", pw, d2r_bler(f, pw, o.nf, o.cw, o.pn_floor, o.pn_corner, o.trials));
    return 0;
  }
  if (!strcmp(argv[1], "check")) {
    parse(argc, argv, &o);
    return check(&o);
  }
  fprintf(stderr, "aiot_sim: unknown command %s\n", argv[1]);
  usage(stderr);
  return 2;
}
