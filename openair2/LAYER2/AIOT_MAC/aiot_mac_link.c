/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file aiot_mac_link.c
 * \brief A Reader's link settings, and the D2R Scheduling Info (38.391 6.2.1.6) of each message
 * built from them. */

#include <string.h>

#include "aiot_mac.h"

int aiot_link_n_rb(const aiot_link_config_t *cfg)
{
  return cfg->n_rb ? cfg->n_rb : aiot_n_rb_min(cfg->m_chip_symb);
}

int aiot_link_max_entries(const aiot_link_config_t *cfg)
{
  int allowed[8];
  const int n = aiot_allowed_r_sfs(cfg->t_bit_num, cfg->t_bit_den, allowed);
  if (n < 0)
    return n;
  return n < AIOT_MAC_MAX_ID_ENTRIES ? n : AIOT_MAC_MAX_ID_ENTRIES;
}

/* What every Scheduling Info of the link shares. */
static int common(const aiot_link_config_t *cfg, aiot_sched_variant_t variant, aiot_sched_info_t *out)
{
  int i_bits[4];
  if (cfg->interval_cp < 0 || cfg->interval_cp > 3)
    return AIOT_EINVAL;
  if (aiot_i_bit_values(cfg->t_bit_num, cfg->t_bit_den, i_bits) != 4)
    return AIOT_EINVAL;
  memset(out, 0, sizeof(*out));
  out->variant = variant;
  out->t_bit_num = cfg->t_bit_num;
  out->t_bit_den = cfg->t_bit_den;
  out->r_block = cfg->r_block;
  out->r_code = cfg->r_code;
  out->i_bit = i_bits[cfg->interval_cp];
  out->l_amble = cfg->l_amble;
  out->i_add = cfg->i_add;
  return 0;
}

/* Copies n R_SFS values, sorted ascending. */
static int set_r_sfs(aiot_sched_info_t *s, const int *r_sfs, int n)
{
  if (n < 1 || n > 8)
    return AIOT_EINVAL;
  for (int i = 0; i < n; i++) {
    int v = r_sfs[i], j = i;
    while (j > 0 && s->r_sfs[j - 1] > v) {
      s->r_sfs[j] = s->r_sfs[j - 1];
      j--;
    }
    s->r_sfs[j] = v;
  }
  s->n_sfs = n;
  return 0;
}

int aiot_link_cbra(const aiot_link_config_t *cfg, const int *r_sfs, int n_r_sfs, int x, aiot_sched_info_t *out)
{
  if (common(cfg, AIOT_SCHED_PAGING_CBRA, out) < 0)
    return AIOT_EINVAL;
  if (n_r_sfs == 0) {
    r_sfs = cfg->r_sfs;
    n_r_sfs = cfg->n_r_sfs;
  }
  if (set_r_sfs(out, r_sfs, n_r_sfs) < 0)
    return AIOT_EINVAL;
  out->x = x ? x : cfg->x;
  out->d2r_tbs = 0;
  return aiot_mac_sched_validate(out);
}

int aiot_link_ridr(const aiot_link_config_t *cfg, int n_entries, int d2r_tbs, aiot_sched_info_t *out)
{
  int allowed[8];
  const int n = aiot_allowed_r_sfs(cfg->t_bit_num, cfg->t_bit_den, allowed);
  if (n < 0 || n_entries < 1 || n_entries > n)
    return AIOT_EINVAL;
  if (common(cfg, AIOT_SCHED_RANDOM_ID_RESPONSE, out) < 0 || set_r_sfs(out, allowed, n_entries) < 0)
    return AIOT_EINVAL;
  out->d2r_tbs = d2r_tbs ? d2r_tbs : cfg->d2r_tbs;
  return aiot_mac_sched_validate(out);
}

static int single(const aiot_link_config_t *cfg,
                  aiot_sched_variant_t variant,
                  int r_sfs,
                  int d2r_tbs,
                  aiot_sched_info_t *out)
{
  if (cfg->n_r_sfs < 1 || common(cfg, variant, out) < 0)
    return AIOT_EINVAL;
  int v = r_sfs;
  if (!v) { /* the link's smallest */
    v = cfg->r_sfs[0];
    for (int i = 1; i < cfg->n_r_sfs; i++)
      if (cfg->r_sfs[i] < v)
        v = cfg->r_sfs[i];
  }
  if (set_r_sfs(out, &v, 1) < 0)
    return AIOT_EINVAL;
  out->d2r_tbs = d2r_tbs ? d2r_tbs : cfg->d2r_tbs;
  return aiot_mac_sched_validate(out);
}

int aiot_link_cfa(const aiot_link_config_t *cfg, int r_sfs, int d2r_tbs, aiot_sched_info_t *out)
{
  return single(cfg, AIOT_SCHED_PAGING_CFA, r_sfs, d2r_tbs, out);
}

int aiot_link_unicast(const aiot_link_config_t *cfg, int r_sfs, int d2r_tbs, aiot_sched_info_t *out)
{
  return single(cfg, AIOT_SCHED_R2D_DATA, r_sfs, d2r_tbs, out);
}

int aiot_link_config_check(const aiot_link_config_t *cfg)
{
  if (!aiot_m_valid(cfg->m_chip_symb))
    return AIOT_EINVAL;
  if (cfg->n_rb && cfg->n_rb < aiot_n_rb_min(cfg->m_chip_symb))
    return AIOT_EINVAL;
  if (cfg->interval_cp < 0 || cfg->interval_cp > 3)
    return AIOT_EINVAL;
  if (cfg->d2r_tbs < 1 || cfg->d2r_tbs > AIOT_MAC_MAX_TBS)
    return AIOT_EINVAL;
  for (int i = 1; i < cfg->n_r_sfs; i++)
    if (cfg->r_sfs[i] == cfg->r_sfs[i - 1])
      return AIOT_EINVAL;
  aiot_sched_info_t s;
  return aiot_link_cbra(cfg, NULL, 0, 0, &s);
}

void aiot_paging_id_of_node(int node_id, uint8_t bits[16])
{
  for (int i = 0; i < 16; i++)
    bits[i] = (node_id >> (15 - i)) & 1;
}
