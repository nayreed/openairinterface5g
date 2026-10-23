/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file rfsim_hub.c
 * \brief rfsim_hub: reads the room from the configuration, then serves it.
 *
 *   rfsim_hub -O hub.conf
 *
 * See radio/rfsimulator/hub/README.md for the configuration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/assertions.h"
#include "common/utils/utils.h"
#include "rfsim_hub.h"

#define HUB_SECTION "rfsim_hub"

configmodule_interface_t *uniqCfg = NULL;

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  LOG_E(HW, "called by %s:%d %s(): exiting: %s\n", file, line, function, s ? s : "no message");
  if (assert)
    abort();
  exit(EXIT_SUCCESS);
}

static int node_index(const hub_conf_t *conf, const char *name)
{
  for (int i = 0; i < conf->num_nodes; i++)
    if (strcmp(conf->nodes[i].name, name) == 0)
      return i;
  return -1;
}

static void read_nodes(hub_conf_t *conf)
{
  enum { NAME, PORT, LOOKAHEAD, LEAD };
  // clang-format off
  paramdef_t desc[] = {
    [NAME]      = STRINGPARAM("name",      "node name, used by the links\n",                                   PARAMFLAG_MANDATORY, NULL, NULL),
    [PORT]      = INTPARAM("port",         "port the node connects to as an rfsim client\n",                   PARAMFLAG_MANDATORY, NULL, 0),
    [LOOKAHEAD] = INTPARAM("lookahead",    "samples the node writes ahead of what it has read\n",              0,                   NULL, 0),
    [LEAD]      = BOOLPARAM("lead",        "the node's first write is already lookahead samples ahead\n",      0,                   NULL, 0),
  };
  // clang-format on
  paramlist_def_t list = {"nodes", NULL, 0};
  config_getlist(config_get_if(), &list, desc, sizeofArray(desc), HUB_SECTION);
  conf->num_nodes = list.numelt;
  conf->nodes = calloc_or_fail(list.numelt > 0 ? list.numelt : 1, sizeof(*conf->nodes));
  for (int i = 0; i < list.numelt; i++) {
    paramdef_t *p = list.paramarray[i];
    hub_node_conf_t *n = &conf->nodes[i];
    snprintf(n->name, sizeof(n->name), "%s", *p[NAME].strptr);
    n->port = *p[PORT].iptr;
    n->lookahead = *p[LOOKAHEAD].iptr;
    n->lead = *p[LEAD].iptr != 0;
    AssertFatal(node_index(conf, n->name) == i, "two nodes are named %s\n", n->name);
  }
}

static void read_links(hub_conf_t *conf)
{
  enum { TX, RX, GAIN, DELAY, SHIFT };
  // clang-format off
  paramdef_t desc[] = {
    [TX]    = STRINGPARAM("tx",      "transmitting node\n",                                     PARAMFLAG_MANDATORY, NULL, NULL),
    [RX]    = STRINGPARAM("rx",      "receiving node\n",                                        PARAMFLAG_MANDATORY, NULL, NULL),
    [GAIN]  = DOUBLEPARAM("gain_dB", "gain from tx to rx, in dB\n",                             0,                   NULL, 0.0),
    [DELAY] = INTPARAM("delay",      "delay from tx to rx, in samples\n",                       0,                   NULL, 0),
    [SHIFT] = DOUBLEPARAM("shift_Hz", "where tx's signal lands from rx's centre frequency, Hz\n", 0,                   NULL, 0.0),
  };
  // clang-format on
  paramlist_def_t list = {"links", NULL, 0};
  config_getlist(config_get_if(), &list, desc, sizeofArray(desc), HUB_SECTION);
  conf->num_links = list.numelt;
  conf->links = calloc_or_fail(list.numelt > 0 ? list.numelt : 1, sizeof(*conf->links));
  for (int l = 0; l < list.numelt; l++) {
    paramdef_t *p = list.paramarray[l];
    hub_link_conf_t *k = &conf->links[l];
    k->tx = node_index(conf, *p[TX].strptr);
    k->rx = node_index(conf, *p[RX].strptr);
    AssertFatal(k->tx >= 0, "link %d: no node is named %s\n", l, *p[TX].strptr);
    AssertFatal(k->rx >= 0, "link %d: no node is named %s\n", l, *p[RX].strptr);
    k->gain_dB = *p[GAIN].dblptr;
    k->delay = *p[DELAY].iptr;
    k->shift_Hz = *p[SHIFT].dblptr;
  }
}

int main(int argc, char **argv)
{
  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == NULL || CONFIG_ISFLAGSET(CONFIG_ABORT)) {
    fprintf(stderr, "configuration module init failed\n");
    exit(EXIT_FAILURE);
  }
  logInit();

  hub_conf_t conf = {0};
  int quantum, history;
  double idle_timeout;
  // clang-format off
  paramdef_t desc[] = {
    DOUBLEPARAM("sample_rate",  "sample rate of every node, Hz\n",                          0, &conf.sample_rate, 0.0),
    INTPARAM("quantum",         "samples per block the hub sends each node\n",              0, &quantum,          1440),
    INTPARAM("history",         "samples kept per node, a power of two\n",                  0, &history,          262144),
    DOUBLEPARAM("idle_timeout", "seconds without any sample moving before the hub stops\n", 0, &idle_timeout,     60.0),
  };
  // clang-format on
  AssertFatal(config_get(config_get_if(), desc, sizeofArray(desc), HUB_SECTION) >= 0, "cannot read section %s\n", HUB_SECTION);
  conf.quantum = quantum;
  conf.history = history;
  read_nodes(&conf);
  read_links(&conf);
  config_check_unknown_cmdlineopt(uniqCfg, CONFIG_CHECKALLSECTIONS);

  char why[256];
  hub_room_t *room = hub_room_create(&conf, why, sizeof(why));
  AssertFatal(room != NULL, "the room cannot run: %s\n", why);
  int ret = hub_serve(room, idle_timeout);

  hub_room_free(room);
  free(conf.nodes);
  free(conf.links);
  end_configmodule(uniqCfg);
  return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
