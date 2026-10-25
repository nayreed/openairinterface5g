/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "amber_host.h"
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/assertions.h"
#include "common/utils/load_module_shlib.h"
#include "common/utils/utils.h"

#define AMBER_SECTION "amber"
#define AMBER_SESSION_MODULE "amber.session"
#define MS_PER_SECOND 1000
#define NOT_SET (-1)
#define ROLE_READER "reader"
#define ROLE_CARRIER "carrier"

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
  };
  paramdef_t tag_desc[] = {
    INTPARAM("node_id",       "scenario node run by this amber-tag\n",      0, &params->node_id,     NOT_SET),
  };
  paramdef_t reader_desc[] = {
    STRINGPARAM("role",
                "what amber-reader runs: \"" ROLE_READER "\", the scenario's Reader, or \"" ROLE_CARRIER "\", its carrier node\n",
                0, &params->role, ROLE_READER),
  };
  // clang-format on
  params->node_id = NOT_SET;
  params->role = NULL;
  int ret = config_get(config_get_if(), desc, sizeofArray(desc), AMBER_SECTION);
  if (ret >= 0)
    ret = with_node_id ? config_get(config_get_if(), tag_desc, sizeofArray(tag_desc), AMBER_SECTION)
                       : config_get(config_get_if(), reader_desc, sizeofArray(reader_desc), AMBER_SECTION);
  if (ret < 0) {
    LOG_E(UTIL, "cannot read the %s configuration section\n", AMBER_SECTION);
    exit(EXIT_FAILURE);
  }
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
  if (!with_node_id && strcmp(params->role, ROLE_READER) != 0 && strcmp(params->role, ROLE_CARRIER) != 0) {
    LOG_E(UTIL, "%s.role is \"%s\": it must be \"" ROLE_READER "\" or \"" ROLE_CARRIER "\"\n", AMBER_SECTION, params->role);
    exit(EXIT_FAILURE);
  }
}

void amber_radio_start(amber_radio_t *radio, int block_bytes)
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
  if (ret < 0) {
    LOG_E(HW, "cannot load library %s\n", OAI_RFSIM_LIBNAME);
    exit(EXIT_FAILURE);
  }
  if (((amber_devfunc_t)init.fptr)(&radio->device, cfg) != 0) {
    LOG_E(HW, "%s device_init() failed\n", OAI_RFSIM_LIBNAME);
    exit(EXIT_FAILURE);
  }
  if (radio->device.trx_start_func(&radio->device) != 0) {
    LOG_E(HW, "cannot start the rfsimulator device\n");
    exit(EXIT_FAILURE);
  }
}

openair0_timestamp_t amber_radio_read(amber_radio_t *radio, int offset, int nsamps)
{
  DevAssert(offset >= 0 && nsamps > 0 && offset + nsamps <= radio->block_samples);
  openair0_timestamp_t ts;
  void *buf[1] = {radio->block + offset};
  int ret = radio->device.trx_read_func(&radio->device, &ts, buf, nsamps, 1);
  AssertFatal(ret == nsamps, "rfsim read returned %d samples, expected %d\n", ret, nsamps);
  return ts;
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

/* Start Python. Amber's R2D and D2R signal processing is OAI's A-IoT PHY (openair1/PHY/AIOT),
 * which Amber loads from AMBER_AIOT_LIB. Unless the environment already sets it, point it at the
 * libaiot_phy.so next to this program, which the build puts there. */
static void start_python(void)
{
  if (getenv("AMBER_AIOT_LIB") == NULL) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = 0;
      char lib[PATH_MAX];
      snprintf(lib, sizeof(lib), "%s/libaiot_phy.so", dirname(exe));
      if (access(lib, R_OK) == 0)
        setenv("AMBER_AIOT_LIB", lib, 0);
    }
  }
  const char *lib = getenv("AMBER_AIOT_LIB");
  if (lib)
    LOG_I(UTIL, "A-IoT PHY: %s\n", lib);
  else
    LOG_W(UTIL, "no libaiot_phy.so next to this program and AMBER_AIOT_LIB not set: Amber looks for its own copy\n");
  // No Python signal handlers: Ctrl-C stops the program as for any other OAI executable.
  Py_InitializeEx(0);
}

amber_session_t *amber_session_create_reader(const amber_params_t *params)
{
  start_python();
  PyObject *args = Py_BuildValue("(sis)", params->scenario, params->block_bytes, params->output_dir);
  return session_create(params, strcmp(params->role, ROLE_CARRIER) == 0 ? "CarrierSession" : "ReaderSession", args);
}

amber_session_t *amber_session_create_tag(const amber_params_t *params)
{
  start_python();
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

bool amber_session_on_block(amber_session_t *session, const uint8_t *block)
{
  PyObject *ret = PyObject_CallMethod(session->obj, "on_block", "y#", (const char *)block, (Py_ssize_t)session->block_bytes);
  if (ret == NULL)
    python_failed("ReaderSession.on_block()");
  int done = PyObject_IsTrue(ret);
  Py_DECREF(ret);
  if (done < 0)
    python_failed("ReaderSession.on_block() result");
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
