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
controller_disconnect(clixon_handle h,
                      cxobj        *xn)
{
    char         *name;
    device_handle dh;
    
    if ((name = xml_find_body(xn, "name")) != NULL &&
        (dh = device_handle_find(h, name)) != NULL)
        device_close_connection(dh, NULL); /* Regular disconnect, no reason */
    return 0;
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
    int                     retval = -1;
    cxobj                 **vec1 = NULL;
    cxobj                 **vec2 = NULL;
    cxobj                 **vec3 = NULL;
    size_t                  veclen1;
    size_t                  veclen2;
    size_t                  veclen3;
    int                     i;
    char                   *body;
    controller_transaction *ct = NULL; /* created lazy/on-demand */
    cxobj                  *xn;
    int                     ret;
    char                   *enablestr;
    cbuf                   *cberr = NULL;
    
    /* 1) if device removed, disconnect */
    if (xpath_vec_flag(src, nsc, "devices/device",
                       XML_FLAG_DEL,
                       &vec1, &veclen1) < 0)
        goto done;
    for (i=0; i<veclen1; i++){
        if (controller_disconnect(h, vec1[i]) < 0)
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
                if (controller_disconnect(h, xml_parent(vec2[i])) < 0)
                    goto done;
            }
            else{
                if (ct == NULL){
                    if ((ret = controller_transaction_new(h, "commit", &ct, &cberr)) < 0)
                        goto done;
                    if (ret == 0){
                        clicon_err(OE_XML, 0, "%s", cbuf_get(cberr));
                        goto done;
                    }
                }
                if (controller_connect(h, xml_parent(vec2[i]), ct) < 0)
                    goto done;
            }
        }
    }
    /* 3) if device added, connect */
    if (xpath_vec_flag(target, nsc, "devices/device",
                       XML_FLAG_ADD,
                       &vec3, &veclen3) < 0)
        goto done;
    for (i=0; i<veclen3; i++){
        xn = vec3[i];
        if ((enablestr = xml_find_body(xn, "enabled")) != NULL &&
            strcmp(enablestr, "false") == 0)
            continue;
        if (ct == NULL){
            if ((ret = controller_transaction_new(h, "commit", &ct, &cberr)) < 0)
                goto done;
            if (ret == 0){
                clicon_err(OE_XML, 0, "%s", cbuf_get(cberr));
                goto done;
            }
        }
        if (controller_connect(h, xn, ct) < 0)
            goto done;
    }
    /* No device started, close transaction */
    if (ct && controller_transaction_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (vec3)
        free(vec3);
    return retval;
}

/*! Commit generic part of controller yang
 * @param[in] h    Clixon handle
 * @param[in] nsc  Namespace context
 * @param[in] target  Post target xml tree
 * @retval   -1    Error
 * @retval    0    OK
 */
static int
controller_commit_generic(clixon_handle h,
                          cvec         *nsc,
                          cxobj        *target)
{
    int      retval = -1;
    char    *body;
    cxobj  **vec = NULL;
    size_t   veclen;
    int      i;
    uint32_t d;
    
    if (xpath_vec_flag(target, nsc, "generic/device-timeout",
                       XML_FLAG_ADD | XML_FLAG_CHANGE,
                       &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((body = xml_body(vec[i])) == NULL)
            continue;
        if (parse_uint32(body, &d, NULL) < 1){
            clicon_err(OE_UNIX, errno, "error parsing limit:%s", body);
            goto done;
        }
        clicon_data_int_set(h, "controller-device-timeout", d);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
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

    clicon_debug(1, "controller commit");
    src = transaction_src(td);    /* existing XML tree */
    target = transaction_target(td); /* wanted XML tree */
    if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (controller_commit_generic(h, nsc, target) < 0)
        goto done;
    if (controller_commit_device(h, nsc, src, target) < 0)
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


/* Called just before plugin unloaded. 
 * @param[in] h    Clixon handle
 */
static int
controller_exit(clixon_handle h)
{
    device_handle_free_all(h);
    return 0;
}

/* Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "controller backend",
    .ca_exit         = controller_exit,
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
     * see services_commit_notify */
    if (stream_add(h, "services-commit",
                   "A commit has been made that changes the services declaration",
                   0, NULL) < 0)
        goto done;
    /* see controller_transaction_notify */
    if (stream_add(h, "controller-transaction",
                   "A transaction has been completed.",
                   0, NULL) < 0)
        goto done;
    return &api;
 done:
    return NULL;
}
