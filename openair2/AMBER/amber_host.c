/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "amber_host.h"
#include <stdlib.h>
#include <string.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/assertions.h"
#include "common/utils/load_module_shlib.h"
#include "common/utils/utils.h"

#define AMBER_SECTION "amber"
#define AMBER_SESSION_MODULE "amber.split.session"
#define MS_PER_SECOND 1000
#define NOT_SET (-1)

struct amber_session_s {
  PyObject *obj;
  size_t block_bytes;
};

typedef int (*amber_devfunc_t)(openair0_device_t *, openair0_config_t *);

void amber_read_params(amber_params_t *params, bool with_node_id)
{
  // clang-format off
  paramdef_t desc[] = {
    STRINGPARAM("scenario",   "path of the Amber scenario module\n",        0, &params->scenario,    NULL),
    STRINGPARAM("output_dir", "directory for the CSV logs\n",               0, &params->output_dir,  "."),
    INTPARAM("block_bytes",   "bytes per rfsim block (one simulated ms)\n", 0, &params->block_bytes, AMBER_DEFAULT_BLOCK_BYTES),
    INTPARAM("node_id",       "scenario node run by this amber-tag\n",      0, &params->node_id,     NOT_SET),
  };
  // clang-format on
  int num = with_node_id ? sizeofArray(desc) : sizeofArray(desc) - 1;
  int ret = config_get(config_get_if(), desc, num, AMBER_SECTION);
  AssertFatal(ret >= 0, "cannot read the %s configuration section\n", AMBER_SECTION);
  if (params->scenario == NULL || params->scenario[0] == '\0') {
    LOG_E(UTIL, "set %s.scenario to the scenario module to run\n", AMBER_SECTION);
    exit(EXIT_FAILURE);
  }
  if (params->block_bytes <= 0 || params->block_bytes % sizeof(c16_t) != 0) {
    LOG_E(UTIL, "%s.block_bytes must be a positive multiple of %zu\n", AMBER_SECTION, sizeof(c16_t));
    exit(EXIT_FAILURE);
  }
  if (with_node_id && params->node_id == NOT_SET) {
    LOG_E(UTIL, "set %s.node_id to the scenario node this program runs\n", AMBER_SECTION);
    exit(EXIT_FAILURE);
  }
}

void amber_radio_start(amber_radio_t *radio, int block_bytes, bool reader)
{
  memset(radio, 0, sizeof(*radio));
  radio->block_samples = block_bytes / sizeof(c16_t);
  radio->block = calloc_or_fail(radio->block_samples, sizeof(c16_t));
  openair0_config_t *cfg = &radio->config;
  cfg->tx_num_channels = 1;
  cfg->rx_num_channels = 1;
  // Nominal only: rfsim paces nothing by sample rate, but this makes one block one millisecond.
  cfg->sample_rate = (double)radio->block_samples * MS_PER_SECOND;

  loader_shlibfunc_t init = {.fname = "device_init"};
  int ret = load_module_shlib(OAI_RFSIM_LIBNAME, &init, 1, NULL);
  AssertFatal(ret >= 0, "cannot load library %s\n", OAI_RFSIM_LIBNAME);
  ret = ((amber_devfunc_t)init.fptr)(&radio->device, cfg);
  AssertFatal(ret == 0, "%s device_init() failed\n", OAI_RFSIM_LIBNAME);
  if (radio->device.trx_start_func(&radio->device) != 0) {
    LOG_E(HW, "cannot start the rfsimulator device\n");
    exit(EXIT_FAILURE);
  }
  if (reader) {
    if (radio->device.trx_read_peers_func == NULL) {
      LOG_E(HW,
            "amber-reader must be the rfsim server in msgmode: --rfsimulator.serveraddr server --rfsimulator.options msgmode\n");
      exit(EXIT_FAILURE);
    }
    radio->tag_blocks = calloc_or_fail(AMBER_MAX_TAGS, sizeof(*radio->tag_blocks));
    c16_t *samples = calloc_or_fail((size_t)AMBER_MAX_TAGS * radio->block_samples, sizeof(c16_t));
    for (int i = 0; i < AMBER_MAX_TAGS; i++)
      radio->tag_blocks[i] = samples + (size_t)i * radio->block_samples;
  }
}

openair0_timestamp_t amber_radio_read(amber_radio_t *radio, int nsamps)
{
  DevAssert(nsamps > 0 && nsamps <= radio->block_samples);
  openair0_timestamp_t ts;
  void *buf[1] = {radio->block};
  int ret = radio->device.trx_read_func(&radio->device, &ts, buf, nsamps, 1);
  AssertFatal(ret == nsamps, "rfsim read returned %d samples, expected %d\n", ret, nsamps);
  return ts;
}

int amber_radio_read_tags(amber_radio_t *radio, openair0_timestamp_t *ts)
{
  DevAssert(radio->tag_blocks != NULL);
  int tags = radio->device.trx_read_peers_func(&radio->device, ts, radio->tag_blocks, radio->block_samples, AMBER_MAX_TAGS);
  if (tags < 0) {
    LOG_E(HW, "more than %d Tags are connected\n", AMBER_MAX_TAGS);
    exit(EXIT_FAILURE);
  }
  return tags;
}

void amber_radio_write(amber_radio_t *radio, openair0_timestamp_t ts, int flags)
{
  void *buf[1] = {radio->block};
  int ret = radio->device.trx_write_func(&radio->device, ts, buf, radio->block_samples, 1, flags);
  AssertFatal(ret == radio->block_samples, "rfsim write returned %d samples, expected %d\n", ret, radio->block_samples);
}

void amber_radio_stop(amber_radio_t *radio)
{
  radio->device.trx_end_func(&radio->device);
  free(radio->block);
  if (radio->tag_blocks != NULL) {
    free(radio->tag_blocks[0]);
    free(radio->tag_blocks);
  }
}

static void python_failed(const char *what)
{
  LOG_E(UTIL, "%s failed in Python:\n", what);
  PyErr_Print();
  exit(EXIT_FAILURE);
}

static amber_session_t *session_create(const amber_params_t *params, const char *class_name, PyObject *args)
{
  if (args == NULL)
    python_failed("building the session arguments");
  PyObject *module = PyImport_ImportModule(AMBER_SESSION_MODULE);
  if (module == NULL)
    python_failed("importing " AMBER_SESSION_MODULE " (is it on PYTHONPATH?)");
  PyObject *cls = PyObject_GetAttrString(module, class_name);
  Py_DECREF(module);
  if (cls == NULL)
    python_failed(class_name);
  PyObject *obj = PyObject_CallObject(cls, args);
  Py_DECREF(cls);
  Py_DECREF(args);
  if (obj == NULL)
    python_failed(class_name);
  amber_session_t *session = calloc_or_fail(1, sizeof(*session));
  session->obj = obj;
  session->block_bytes = params->block_bytes;
  return session;
}

amber_session_t *amber_session_create_reader(const amber_params_t *params)
{
  // No Python signal handlers: Ctrl-C stops the program as for any other OAI executable.
  Py_InitializeEx(0);
  PyObject *args = Py_BuildValue("(sis)", params->scenario, params->block_bytes, params->output_dir);
  return session_create(params, "ReaderSession", args);
}

amber_session_t *amber_session_create_tag(const amber_params_t *params)
{
  Py_InitializeEx(0);
  PyObject *args = Py_BuildValue("(siis)", params->scenario, params->node_id, params->block_bytes, params->output_dir);
  return session_create(params, "TagSession", args);
}

/* Copy a bytes object of exactly the block size into out. */
static void copy_block(const amber_session_t *session, PyObject *bytes, const char *what, uint8_t *out)
{
  if (!PyBytes_Check(bytes) || (size_t)PyBytes_GET_SIZE(bytes) != session->block_bytes) {
    LOG_E(UTIL, "%s did not return %zu bytes\n", what, session->block_bytes);
    exit(EXIT_FAILURE);
  }
  memcpy(out, PyBytes_AS_STRING(bytes), session->block_bytes);
}

void amber_session_next_block(amber_session_t *session, uint8_t *out)
{
  PyObject *ret = PyObject_CallMethod(session->obj, "next_block", NULL);
  if (ret == NULL)
    python_failed("ReaderSession.next_block()");
  copy_block(session, ret, "ReaderSession.next_block()", out);
  Py_DECREF(ret);
}

bool amber_session_on_blocks(amber_session_t *session, void *const *blocks, int n)
{
  PyObject *list = PyList_New(n);
  if (list == NULL)
    python_failed("building the list of Tag blocks");
  for (int i = 0; i < n; i++) {
    PyObject *block = PyBytes_FromStringAndSize((const char *)blocks[i], session->block_bytes);
    if (block == NULL)
      python_failed("building the list of Tag blocks");
    PyList_SET_ITEM(list, i, block);
  }
  PyObject *ret = PyObject_CallMethod(session->obj, "on_blocks", "(O)", list);
  Py_DECREF(list);
  if (ret == NULL)
    python_failed("ReaderSession.on_blocks()");
  int done = PyObject_IsTrue(ret);
  Py_DECREF(ret);
  if (done < 0)
    python_failed("ReaderSession.on_blocks() result");
  return done;
}

bool amber_session_reply(amber_session_t *session, const uint8_t *in, uint8_t *out)
{
  PyObject *ret = PyObject_CallMethod(session->obj, "reply", "y#", (const char *)in, (Py_ssize_t)session->block_bytes);
  if (ret == NULL)
    python_failed("TagSession.reply()");
  if (!PyTuple_Check(ret) || PyTuple_GET_SIZE(ret) != 2) {
    LOG_E(UTIL, "TagSession.reply() did not return (block, done)\n");
    exit(EXIT_FAILURE);
  }
  copy_block(session, PyTuple_GET_ITEM(ret, 0), "TagSession.reply()", out);
  int done = PyObject_IsTrue(PyTuple_GET_ITEM(ret, 1));
  Py_DECREF(ret);
  if (done < 0)
    python_failed("TagSession.reply() result");
  return done;
}

void amber_session_end(amber_session_t *session)
{
  PyObject *ret = PyObject_CallMethod(session->obj, "finish", NULL);
  if (ret == NULL)
    python_failed("writing the logs");
  Py_DECREF(ret);
  Py_DECREF(session->obj);
  free(session);
  if (Py_FinalizeEx() < 0)
    LOG_W(UTIL, "Python did not shut down cleanly\n");
}
