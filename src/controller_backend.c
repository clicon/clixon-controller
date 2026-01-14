/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2023 Olof Hagsand

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  ***** END LICENSE BLOCK *****
  *
  * Main backend plugin file
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/time.h>
#include <sys/stat.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_rpc.h"

/*! Called to get state data from plugin by programmatically adding state
 *
 * @param[in]    h        Clixon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPath syntax. or NULL for all
 * @param[out]   xstate   XML tree, <config/> on entry.
 * @retval       0        OK
 * @retval      -1        Error
 */
int
controller_statedata(clixon_handle   h,
                     cvec           *nsc,
                     char           *xpath,
                     cxobj          *xstate)
{
    int retval = -1;

    if (devices_statedata(h, nsc, xpath, xstate) < 0)
        goto done;
    if (controller_transaction_statedata(h, nsc, xpath, xstate) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Disconnect device
 */
static int
disconnect_device_byxml(clixon_handle h,
                        cxobj        *xn)
{
    char         *name;
    device_handle dh = NULL;

    if ((name = xml_find_body(xn, "name")) != NULL &&
        (dh = device_handle_find(h, name)) != NULL &&
        device_handle_conn_state_get(dh) != CS_CLOSED){
        device_close_connection(dh, NULL); /* Regular disconnect, no reason */
    }
    if (dh)
        device_handle_free(dh);
    return 0;
}

/*! Changes in processes config
 *
 * Start/stop services daemon
 * @retval    0    OK
 * @retval   -1    Error
 * @see clixon-controller.yang: processes/services
 */
static int
controller_commit_processes(clixon_handle h,
                            cvec         *nsc,
                            cxobj        *src,
                            cxobj        *target)
{
    int      retval = -1;
    cxobj  **vec = NULL;
    size_t   veclen;
    char    *body;

    if (xpath_vec_flag(target, nsc, "processes/services/enabled",
                       XML_FLAG_CHANGE|XML_FLAG_ADD,
                       &vec, &veclen) < 0)
        goto done;
    if (veclen){
        body = xml_body(vec[0]);
        if (strcmp(body, "true") == 0){
            if (clixon_process_operation(h, SERVICES_PROCESS, PROC_OP_START, 0) < 0)
                goto done;
        }
        else {
            if (clixon_process_operation(h, SERVICES_PROCESS, PROC_OP_STOP, 0) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Changes in services config
 *
 * Start/stop services daemon
 * @retval    0    OK
 * @retval   -1    Error
 * @see clixon-controller.yang: processes/services
 */
static int
controller_commit_services(clixon_handle h,
                           cvec         *nsc,
                           cxobj        *src,
                           cxobj        *target)
{
    int      retval = -1;
    cxobj  **vec = NULL;
    size_t   veclen;
    cxobj   *x;
    char    *body;
    uint32_t dt;
    int      i;

    if (xpath_vec_flag(target, nsc, "services/service-timeout",
                       XML_FLAG_ADD | XML_FLAG_CHANGE,
                       &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){ /* veclen should be 1 */
        x = vec[i];
        if ((body = xml_body(x)) == NULL)
            continue;
        if (parse_uint32(body, &dt, NULL) < 1){
            clixon_err(OE_UNIX, errno, "error parsing limit:%s", body);
            goto done;
        }
        clixon_debug(CLIXON_DBG_CTRL, "controller-service-timeout: %u", dt);
        clicon_data_int_set(h, "controller-service-timeout", dt);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Changes in devices config
 *
 * @param[in] h    Clixon handle
 * @param[in] nsc  Namespace context
 * @param[in] src  pre-existing xml tree
 * @param[in] target  Post target xml tree
 * @retval    0    OK
 * @retval   -1    Error
 * Logic:
 * 1)  if device removed, disconnect
 * 2a) if enable changed to false, disconnect
 * 2b) if enable changed to true, connect
 * 2c) if device changed addr,user,conn-type: disconnect
 * 2d) if device changed domain,profile: disconnect, reset xml, yang
 */
static int
controller_commit_devices(clixon_handle h,
                          cvec         *nsc,
                          cxobj        *src,
                          cxobj        *target)
{
    int       retval = -1;
    cxobj   **vec0 = NULL;
    cxobj   **vec1 = NULL;
    cxobj   **vec2 = NULL;
    cxobj   **vec3 = NULL;
    cxobj   **vec4 = NULL;
    size_t    veclen0;
    size_t    veclen1;
    size_t    veclen2;
    size_t    veclen3;
    size_t    veclen4;
    int       i;
    cxobj    *x;
    char     *body;
    uint32_t  dt;

    if (xpath_vec_flag(target, nsc, "devices/device-timeout",
                       XML_FLAG_ADD | XML_FLAG_CHANGE,
                       &vec0, &veclen0) < 0)
        goto done;
    for (i=0; i<veclen0; i++){ /* veclen0 should be 1 */
        x = vec0[i];
        if ((body = xml_body(x)) == NULL)
            continue;
        if (parse_uint32(body, &dt, NULL) < 1){
            clixon_err(OE_UNIX, errno, "error parsing limit:%s", body);
            goto done;
        }
        clixon_debug(CLIXON_DBG_CTRL, "controller-device-timeout: %u", dt);
        clicon_data_int_set(h, "controller-device-timeout", dt);
    }

    /* 1) if device removed, disconnect */
    if (xpath_vec_flag(src, nsc, "devices/device",
                       XML_FLAG_DEL,
                       &vec1, &veclen1) < 0)
        goto done;
    for (i=0; i<veclen1; i++){
        x = vec1[i];
        if (disconnect_device_byxml(h, x) < 0)
            goto done;
    }
    /* 2a) if enable changed to false, disconnect, to true connect
     */
    if (xpath_vec_flag(target, nsc, "devices/device/enabled",
                       XML_FLAG_CHANGE,
                       &vec2, &veclen2) < 0)
        goto done;
    for (i=0; i<veclen2; i++){
        x = vec2[i];
        if ((body = xml_body(x)) != NULL){
            if (strcmp(body, "false") == 0){
                if (disconnect_device_byxml(h, xml_parent(x)) < 0)
                    goto done;
            }
        }
    }
    /* 2c) if device changed addr,user,conn-type:
     *   - disconnect device
     */
    if (xpath_vec_flag(target, nsc, "devices/device/user | devices/device/conn-type | devices/device/addr",
                       XML_FLAG_CHANGE,
                       &vec3, &veclen3) < 0)
        goto done;
    for (i=0; i<veclen3; i++){
        x = vec3[i];
        if (disconnect_device_byxml(h, xml_parent(x)) < 0)
            goto done;
    }
    /* 2d) if device changed domain,profile:
     *   - disconnect device
     */
    if (xpath_vec_flag(target, nsc, "devices/device/device-domain | devices/device/device-profile",
                       XML_FLAG_CHANGE,
                       &vec4, &veclen4) < 0)
        goto done;
    for (i=0; i<veclen4; i++){
        x = vec4[i];
        if (disconnect_device_byxml(h, xml_parent(x)) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (vec0)
        free(vec0);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (vec3)
        free(vec3);
    if (vec4)
        free(vec4);
    return retval;
}

/*! Transaction commit
 */
int
controller_commit(clixon_handle    h,
                  transaction_data td)
{
    int        retval = -1;
    cxobj     *src;
    cxobj     *target;
    yang_stmt *yspec;
    cvec      *nsc = NULL;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    src = transaction_src(td);    /* existing XML tree */
    target = transaction_target(td); /* wanted XML tree */
    yspec = clicon_dbspec_yang(h);
    if (xml_nsctx_yangspec(yspec, &nsc) < 0)
        goto done;
    if (xml_nsctx_add(nsc, NULL, CONTROLLER_NAMESPACE) < 0)
        goto done;
    if (controller_commit_services(h, nsc, src, target) < 0)
        goto done;
    if (controller_commit_devices(h, nsc, src, target) < 0)
        goto done;
    if (controller_commit_processes(h, nsc, src, target) < 0)
        goto done;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! YANG schema mount
 *
 * Given an XML mount-point xt, return XML yang-lib modules-set
 * Return yanglib as XML tree on the RFC8525 form:
 *   <yang-library>
 *      <module-set>
 *         <module>...</module>
 *         ...
 *      </module-set>
 *   </yang-library>
 * No need to YANG bind.
 * @param[in]  h       Clixon handle
 * @param[in]  xt      XML mount-point in XML tree
 * @param[out] config  If '0' all data nodes in the mounted schema are read-only
 * @param[out] cl      Do or dont do full RFC 7950 validation
 * @param[out] yanglib XML yang-lib module-set tree. Freed by caller.
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC 8528 (schema-mount) and RFC 8525 (yang-lib)
 * @see device_recv_schema_list  where yang-lib is received from device
 * @see controller_connect  where yang-lib is iniated from local yangs
 */
int
controller_yang_mount(clixon_handle   h,
                      cxobj          *xt,
                      int            *config,
                      validate_level *vl,
                      cxobj         **yanglib)
{
    int           retval = -1;
    device_handle dh;
    char         *devname;
    cxobj        *xy0;
    cxobj        *xy1 = NULL;

    /* Return yangs only if device connection is open.
     * This could be discussed: one could want to mount also in a
     * disconnected state.
     * But there is an error case where there is YANG parse error in which
     * case it will re-try mounting repeatedy.
     */
    if ((devname = xml_find_body(xml_parent(xt), "name")) != NULL){
        if ((dh = device_handle_find(h, devname)) == NULL){
            if ((dh = device_handle_new(h, devname)) == NULL)
                goto done;
        }
        if (yanglib){
            if ((xy0 = device_handle_yang_lib_get(dh)) != NULL){
                if ((xy1 = xml_new("new", NULL, CX_ELMNT)) == NULL)
                    goto done;
                /* copy it */
                if (xml_copy(xy0, xy1) < 0)
                    goto done;
                *yanglib = xy1;
                xy1 = NULL;
            }
        }
        if (config)
            *config = 1;
        if (vl){
            if (device_handle_yang_config_get(dh) == YF_VALIDATE)
                *vl =  VL_FULL;
            else
                *vl =  VL_NONE;
        }
    }
    retval = 0;
 done:
    if (xy1)
        xml_free(xy1);
    return retval;
}

/*! Process rpc callback function
 *
 * @param[in]     h   Clixon handle
 * @param[in]     pe  Process entry
 * @param[in,out] op  Process operation
 * @retval        0   OK
 * @retval       -1   Error
 */
int
controller_action_proc_cb(clixon_handle    h,
                          process_entry_t *pe,
                          proc_operation  *operation)
{
    int    retval = -1;

    clixon_debug(CLIXON_DBG_CTRL, "");
    switch (*operation){
    case PROC_OP_STOP:
        /* if RPC op is stop, stop the service */
        break;
    case PROC_OP_START:
        /* RPC op is start & enable is true, then start the service,
                           & enable is false, error or ignore it */
        break;
    default:
        break;
    }
    retval = 0;
    // done:
    return retval;
}

/*! Init service daemon: register services daemon, add NACM proxy user
 *
 * @retval    0    OK
 * @retval   -1    Error
 */
static int
services_daemon_init(clixon_handle h)
{
    int         retval = -1;
    char       *cmd;
    char       *pgm;
    struct stat fstat;
    int         i;
    int         j;
    int         nr;
    char      **argv0 = NULL;
    int         argc0;
    char      **argv1 = NULL;
    gid_t       gid = -1;
    uid_t       uid = -1;
    char       *group;
    char       *user;

    /* Add pyapi user as NACM proxy user */
    user = clicon_backend_user(h);
    if (user && nacm_proxyuser_add(h, user) < 0)
        goto done;
    if ((cmd = clicon_option_str(h, "CONTROLLER_ACTION_COMMAND")) == NULL)
        goto ok;
    if ((argv0 = clicon_strsep(cmd, " \t", &argc0)) == NULL)
        goto done;
    if (argc0 == 0)
        goto ok;
    pgm = argv0[0];
    /* Sanity check of executable */
    if (stat(pgm, &fstat) < 0){
        clixon_err(OE_XML, 0, "%s not found", pgm);
        goto done;
    }
    else if (S_ISREG(fstat.st_mode) == 0){
        clixon_err(OE_XML, 0, "%s not regulare device", pgm);
        goto done;
    }
    /* Get user id, kludge: assume clixon sock group has an associated user */
    if ((group = clicon_sock_group(h)) != NULL){
        if (group_name2gid(group, &gid) < 0){
            clixon_err(OE_DAEMON, errno, "'%s' is not a valid group\n", group);
            goto done;
        }
    }
    if (user){
        if (name2uid(user, &uid) < 0){
            clixon_err(OE_DAEMON, errno, "'%s' is not a valid user .\n", user);
            goto done;
        }
    }
    nr = argc0 + 1;
    if ((argv1 = calloc(nr, sizeof(char *))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    for (j=0; j<argc0; j++)
        argv1[i++] = argv0[j];
    argv1[i++] = NULL;
    if (i > nr){
        clixon_err(OE_UNIX, 0, "calloc mismatatch i:%d nr:%d", i, nr);
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "%s %s", SERVICES_PROCESS, cmd);
    /* The actual fork/exec is made in clixon_process_operation/clixon_proc_background */
    if (clixon_process_register(h, SERVICES_PROCESS,
                                "Controller action daemon process",
                                NULL,
                                uid, gid, 2,
                                controller_action_proc_cb,
                                argv1,
                                i) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    if (argv0 != NULL)
        free(argv0);
    if (argv1 != NULL)
        free(argv1);
    return retval;
}

/*! Reset system status
 *
 * Add xml or set state in backend system.
 * plugin_reset in each backend plugin after all plugins have been initialized.
 * This gives the application a chance to reset system state back to a base state.
 * This is generally done when a system boots up to make sure the initial system state
 * is well defined.
 * This involves creating default configuration files for various daemons, set interface
 * flags etc.
 * In particular for the controller, check if the services daemon is configured up and
 * if so, ensure it is started.
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database name (eg "running")
 * @retval     0   OK
 * @retval    -1   Fatal error
*/
static int
controller_reset(clixon_handle h,
                 const char   *db)
{
    int    retval = -1;
    char  *xpath = "processes/services/enabled";
    cxobj *xtop = NULL;
    cxobj *xse = NULL;
    cvec  *nsc = NULL;
    int    ret;

    if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if ((ret = xmldb_get_cache(h, "running", &xtop, NULL)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_DB, 0, "Error when reading from running_db, unknown error");
        goto done;
    }
    if ((xse = xpath_first(xtop, 0, "%s", xpath)) != NULL){
        if (strcmp(xml_body(xse), "true") == 0)
            if (clixon_process_operation(h, SERVICES_PROCESS, PROC_OP_START, 0) < 0)
                goto done;
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/* Forward */
static int periodic_timer_setup(clixon_handle h);

/*! Handle input data from device, whole or part of a frame, called by event loop
 *
 * FOr now only cleanup transactions
 * @param[in] s    Socket
 * @param[in] arg  Device handle
 * @retval    0    OK
 * @retval   -1    Error
 */
static int
periodic_timer(int   s,
               void *arg)
{
    int           retval = -1;
    clixon_handle h = (clixon_handle)arg;

    if (controller_transaction_periodic(h) < 0)
        goto done;
    if (periodic_timer_setup(h) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

static int
periodic_timer_setup(clixon_handle h)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;

    gettimeofday(&t, NULL);
    t1.tv_sec = CONTROLLER_PERIODIC_TIMER;
    t1.tv_usec = 0;
    timeradd(&t, &t1, &t);
    if (clixon_event_reg_timeout(t, periodic_timer, h, "controller periodic timer") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Called when application is "started", (almost) all initialization is complete
 *
 * Backend: daemon is in the background. If daemon privileges are dropped
 *          this callback is called *before* privileges are dropped.
 * @param[in] h    Clixon handle
 */
static int
controller_start(clixon_handle h)
{
    int            retval = -1;
    char          *backend_user;
    char          *backend_group;
    uid_t          uid;
    gid_t          gid;

    if ((backend_user = clicon_backend_user(h)) == NULL){
        clixon_err(OE_DAEMON, EPERM, "Privileges cannot be dropped without specifying CLICON_BACKEND_USER\n");
        goto done;
    }
    if ((backend_group = clicon_sock_group(h)) == NULL){
        clixon_err(OE_FATAL, 0, "clicon_sock_group option not set");
        goto done;
    }
    if (name2uid(backend_user, &uid) < 0){
        clixon_err(OE_DAEMON, errno, "'%s' is not a valid user", backend_user);
        goto done;
    }
    if (group_name2gid(backend_group, &gid) < 0){
        clixon_err(OE_DAEMON, errno, "'%s' is not a valid group", backend_group);
        goto done;
    }
    /* Used by controller for action process */
    if (xmldb_exists(h, "actions") != 1)
        if (xmldb_create(h, "actions") < 0)
            goto done;
    if (xmldb_drop_priv(h, "actions", uid, gid) < 0)
        goto done;
    /* Used by controller must drop priv on datastore dir */
    if (xmldb_exists(h, "tmpdev") != 1)
        if (xmldb_create(h, "tmpdev") < 0)
            goto done;
    if (xmldb_drop_priv(h, "tmpdev", uid, gid) < 0)
        goto done;
    if (periodic_timer_setup(h) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Called just before plugin unloaded.
 *
 * @param[in] h    Clixon handle
 */
static int
controller_exit(clixon_handle h)
{
    device_handle dh = NULL;

    controller_transaction_free_all(h);
    while ((dh = device_handle_each(h, dh)) != NULL)
        device_close_connection(dh, "controller exit");
    device_handle_free_all(h);
    return 0;
}

/*! Lock database status has changed status, check transaction lock
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database name (eg "running")
 * @param[in]  lock Lock status: 0: unlocked, 1: locked
 * @param[in]  id   Session id (of locker/unlocker)
 * @retval     0    OK
 * @retval    -1    Fatal error
*/
static int
controller_lockdb(clixon_handle h,
                  char         *db,
                  int           lock,
                  int           id)
{
    int                     retval = -1;
    controller_transaction *ct = NULL;
    controller_transaction *ct_list = NULL;

    clixon_debug(CLIXON_DBG_APP, "Lock callback: db%s: locked:%d", db, lock);
    /* If client releases lock while transaction ongoing,
     * then create a new per-transaction lock */
    if (lock == 0 &&
        clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            if (ct->ct_state != TS_DONE &&
                ct->ct_client_id == id){
                if (xmldb_lock(h, db, TRANSACTION_CLIENT_ID) < 0)
                    goto done;
                /* user callback */
                if (clixon_plugin_lockdb_all(h, db, 1, TRANSACTION_CLIENT_ID) < 0)
                    goto done;
                break;
            }
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    retval = 0;
 done:
    return retval;
}

/*! Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "controller backend",
    .ca_start        = controller_start,
    .ca_exit         = controller_exit,
    .ca_reset        = controller_reset,
    .ca_statedata    = controller_statedata,
    .ca_trans_commit = controller_commit,
    .ca_yang_mount   = controller_yang_mount,
    .ca_version      = controller_version,
    .ca_lockdb       = controller_lockdb,
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    if (!clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        clixon_err(OE_YANG, 0, "The clixon controller requires CLICON_YANG_SCHEMA_MOUNT set to true");
        goto done;
    }
    /* Register callback for rpc calls */
    if (controller_rpc_init(h) < 0)
        goto done;
    /* Register notifications
     * see controller_commit_actions */
    if (stream_add(h, "services-commit",
                   "A commit has been made that changes the services declaration",
                   0, NULL) < 0)
        goto done;
    /* see controller_transaction_notify */
    if (stream_add(h, "controller-transaction",
                   "A transaction has been completed.",
                   0, NULL) < 0)
        goto done;
    /* Register pyapi sub-process */
    if (services_daemon_init(h) < 0)
        goto done;
    /* Reset dynamic device handle flag plugin allocation */
    clicon_data_int_set(h, "controller-device-flags", 0);

    /* Register "ctrl" as a debug key */
    if (clixon_debug_key_add("ctrl", CLIXON_DBG_APP) < 0)
        goto done;
    /* Set explicit debug limit */
    clixon_debug_explicit_trunc_set(320);

    return &api;
 done:
    return NULL;
}
