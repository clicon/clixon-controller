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
 * @param[in]    h        Clicon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPATH syntax. or NULL for all
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
    if (controller_transactions_statedata(h, nsc, xpath, xstate) < 0)
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
    device_handle dh;
    
    if ((name = xml_find_body(xn, "name")) != NULL &&
        (dh = device_handle_find(h, name)) != NULL &&
        device_handle_conn_state_get(dh) != CS_CLOSED)
        device_close_connection(dh, NULL); /* Regular disconnect, no reason */
    return 0;
}

/*! Changes in processes
 *
 * Start/stop services daemon
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
            if (clixon_process_operation(h, ACTION_PROCESS, PROC_OP_START, 0) < 0)
                goto done;
        }
        else {
            if (clixon_process_operation(h, ACTION_PROCESS, PROC_OP_STOP, 0) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Commit device config
 *
 * @param[in] h    Clixon handle
 * @param[in] nsc  Namespace context
 * @param[in] src  pre-existing xml tree
 * @param[in] target  Post target xml tree
 * @retval   -1    Error
 * @retval    0    OK
 * Logic:
 * 1) if device removed, disconnect
 * 2a) if enable changed to false, disconnect
 * 2b) if enable changed to true, connect
 * 2c) if device changed addr,user,type changed, disconnect + connect (NYI)
 * 3) if device added, connect
 */
static int
controller_commit_device(clixon_handle h,
                         cvec         *nsc,
                         cxobj        *src,
                         cxobj        *target)
{
    int       retval = -1;
    cxobj   **vec0= NULL;
    cxobj   **vec1 = NULL;
    cxobj   **vec2 = NULL;
    size_t    veclen0;
    size_t    veclen1;
    size_t    veclen2;
    int       i;
    char     *body;
    uint32_t  dt;
    
    if (xpath_vec_flag(target, nsc, "devices/device-timeout",
                       XML_FLAG_ADD | XML_FLAG_CHANGE,
                       &vec0, &veclen0) < 0)
        goto done;
    for (i=0; i<veclen0; i++){
        if ((body = xml_body(vec0[i])) == NULL)
            continue;
        if (parse_uint32(body, &dt, NULL) < 1){
            clicon_err(OE_UNIX, errno, "error parsing limit:%s", body);
            goto done;
        }
        clicon_data_int_set(h, "controller-device-timeout", dt);
    }

    /* 1) if device removed, disconnect */
    if (xpath_vec_flag(src, nsc, "devices/device",
                       XML_FLAG_DEL,
                       &vec1, &veclen1) < 0)
        goto done;
    for (i=0; i<veclen1; i++){
        if (disconnect_device_byxml(h, vec1[i]) < 0)
            goto done;
    }
    /* 2a) if enable changed to false, disconnect, to true connect
     */
    if (xpath_vec_flag(target, nsc, "devices/device/enabled",
                       XML_FLAG_CHANGE,
                       &vec2, &veclen2) < 0)
        goto done;
    for (i=0; i<veclen2; i++){
        if ((body = xml_body(vec2[i])) != NULL){
            if (strcmp(body, "false") == 0){
                if (disconnect_device_byxml(h, xml_parent(vec2[i])) < 0)
                    goto done;
            }
        }
    }
    retval = 0;
 done:
    if (vec0)
        free(vec0);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);

    return retval;
}

/*! Transaction commit
 */
int
controller_commit(clixon_handle    h,
                  transaction_data td)
{
    int     retval = -1;
    cxobj  *src;
    cxobj  *target;
    cvec   *nsc = NULL;

    clicon_debug(CLIXON_DBG_DEFAULT, "controller commit");
    src = transaction_src(td);    /* existing XML tree */
    target = transaction_target(td); /* wanted XML tree */
    if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (controller_commit_device(h, nsc, src, target) < 0)
        goto done;
    if (controller_commit_processes(h, nsc, src, target) < 0)
        goto done;
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! Callback for yang extensions controller 
 * 
 * @param[in] h    Clixon handle
 * @param[in] yext Yang node of extension 
 * @param[in] ys   Yang node of (unknown) statement belonging to extension
 * @retval     0   OK
 * @retval    -1   Error
 */
int
controller_unknown(clicon_handle h,
                     yang_stmt    *yext,
                     yang_stmt    *ys)
{
    return 0;
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
 * @param[out] vallevel Do or dont do full RFC 7950 validation
 * @param[out] yanglib XML yang-lib module-set tree. Freed by caller.
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC 8528 (schema-mount) and RFC 8525 (yang-lib)
 * @see device_state_recv_schema_list  where yang-lib is received from device
 */
int
controller_yang_mount(clicon_handle   h,
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
    if ((devname = xml_find_body(xml_parent(xt), "name")) != NULL &&
        (dh = device_handle_find(h, devname)) != NULL){
        if (yanglib && (xy0 = device_handle_yang_lib_get(dh)) != NULL){
            /* copy it */
            if ((xy1 = xml_new("new", NULL, xml_type(xy0))) == NULL)
                goto done;
            if (xml_copy(xy0, xy1) < 0)
                goto done;
            *yanglib = xy1;
            xy1 = NULL;
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

/*! YANG module patch
 *
 * Given a parsed YANG module, give the ability to patch it before import recursion,
 * grouping/uses checks, augments, etc
 * Can be useful if YANG in some way needs modification.
 * Deviations could be used as alternative (probably better)
 * @param[in]  h       Clixon handle
 * @param[in]  ymod    YANG module
 * @retval     0       OK
 * @retval    -1       Error
 */
int
controller_yang_patch(clicon_handle h,
                      yang_stmt    *ymod)
{
    int         retval = -1;
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    char       *modname;
    yang_stmt  *ygr;
    char       *arg = NULL;

    if (ymod == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "ymod is NULL");
        goto done;
    }
    modname = yang_argument_get(ymod);
    if (strncmp(modname, "junos-rpc", strlen("junos-rpc")) == 0){
        if (yang_find(ymod, Y_GROUPING, "command-forwarding") == NULL){
            if ((ygr = ys_new(Y_GROUPING)) == NULL)
                goto done;
            if ((arg = strdup("command-forwarding")) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (yang_argument_set(ygr, arg) < 0)
                goto done;
            if (yn_insert(ymod, ygr) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
#else
    retval = 0;
#endif
    return retval;
}


/*! Process rpc callback function 
 *
 * @param[in]     h   Clixon handle
 * @param[in]     pe  Process entry
 * @param[in,out] op  Process operation
 */
int
controller_action_proc_cb(clicon_handle    h,
                          process_entry_t *pe,
                          proc_operation  *operation)
{
    int    retval = -1;
    
    clicon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
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

/*! Register action daemon (eg pyapi)
 *
 * Need generic options for other solutions
 */
static int
action_daemon_register(clicon_handle h)
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
    
    clicon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if ((cmd = clicon_option_str(h, "CONTROLLER_ACTION_COMMAND")) == NULL)
        goto ok;
    if ((argv0 = clicon_strsep(cmd, " \t", &argc0)) == NULL)
        goto done;
    if (argc0 == 0)
        goto ok;
    pgm = argv0[0];
    /* Sanity check of executable */
    if (stat(pgm, &fstat) < 0){   
        clicon_err(OE_XML, 0, "%s not found", pgm);
        goto done;
    }
    else if (S_ISREG(fstat.st_mode) == 0){
        clicon_err(OE_XML, 0, "%s not regulare device", pgm);
        goto done;
    }
    /* Get user id, kludge: assume clixon sock group has an associated user */
    if ((group = clicon_sock_group(h)) != NULL){
        if (group_name2gid(group, &gid) < 0){
            clicon_err(OE_DAEMON, errno, "'%s' is not a valid group\n", group);
            goto done;
        }
    }
    if ((user = clicon_backend_user(h)) != NULL){
        if (name2uid(user, &uid) < 0){
            clicon_err(OE_DAEMON, errno, "'%s' is not a valid user .\n", user);
            goto done;
        }
    }
    nr = argc0 + 1;
    if ((argv1 = calloc(nr, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    for (j=0; j<argc0; j++)
        argv1[i++] = argv0[j];
    argv1[i++] = NULL;
    if (i > nr){
        clicon_err(OE_UNIX, 0, "calloc mismatatch i:%d nr:%d", i, nr);
        goto done;
    }
    /* The actual fork/exec is made in clixon_process_operation/clixon_proc_background */
    if (clixon_process_register(h, ACTION_PROCESS,
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
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database name (eg "running")
 * @retval     0   OK
 * @retval    -1   Fatal error
*/
static int
controller_reset(clicon_handle h,
                 const char   *db)
{
    int    retval = -1;
    char  *xpath = "processes/services/enabled";
    cxobj *xtop = NULL;
    cxobj *xse = NULL;
    cvec  *nsc = NULL;
    
    if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (xmldb_get(h, "running", nsc, xpath, &xtop) < 0)
        goto done;
    if ((xse = xpath_first(xtop, 0, "processes/services/enabled")) != NULL){
        if (strcmp(xml_body(xse), "true") == 0)
            if (clixon_process_operation(h, ACTION_PROCESS, PROC_OP_START, 0) < 0)
                goto done;            
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/* Called when application is "started", (almost) all initialization is complete 
 * Backend: daemon is in the background. If daemon privileges are dropped 
 *          this callback is called *before* privileges are dropped.
 * @param[in] h    Clixon handle
 */
static int
controller_start(clixon_handle h)
{
    return 0;
}

/* Called just before plugin unloaded. 
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

/* Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "controller backend",
    .ca_start        = controller_start,
    .ca_exit         = controller_exit,
    .ca_reset        = controller_reset,
    .ca_extension    = controller_unknown,
    .ca_statedata    = controller_statedata,
    .ca_trans_commit = controller_commit,
    .ca_yang_mount   = controller_yang_mount,
    .ca_yang_patch   = controller_yang_patch,
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    if (!clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        clicon_err(OE_YANG, 0, "The clixon controller requires CLICON_YANG_SCHEMA_MOUNT set to true");
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
    if (action_daemon_register(h) < 0)
        goto done;
    return &api;
 done:
    return NULL;
}
