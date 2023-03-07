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
  * Backend rpc callbacks, see clixon-controller.yang for declarations
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
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_rpc.h"

/*! Connect to device via Netconf SSH
 *
 * @param[in]  h  Clixon handle
 * @param[in]  dh Device handle, either NULL or in closed state
 * @param[in]  name Device name
 * @param[in]  user Username for ssh login
 * @param[in]  addr Address for ssh to connect to
 */
static int
connect_netconf_ssh(clixon_handle h,
                    device_handle dh,
                    char         *user,
                    char         *addr)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   s;

    if (addr == NULL || dh == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xn, addr or dh is NULL");
        goto done;
    }
    if (device_handle_conn_state_get(dh) != CS_CLOSED){
        clicon_err(OE_PLUGIN, EINVAL, "dh is not closed");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    if (device_handle_connect(dh, CLIXON_CLIENT_SSH, cbuf_get(cb)) < 0)
        goto done;
    if (device_state_set(dh, CS_CONNECTING) < 0)
        goto done;
    s = device_handle_socket_get(dh);    
    clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
    if (clixon_event_reg_fd(s, device_input_cb, dh, "netconf socket") < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Connect to device 
 *
 * Typically called from commit
 * @param[in] h   Clixon handle
 * @param[in] xn  XML of device config
 * @retval    0   OK
 * @retval    -1  Error
 */
int
controller_connect(clixon_handle h,
                   cxobj        *xn)
{
    int           retval = -1;
    char         *name;
    device_handle dh;
    cbuf         *cb = NULL;
    char         *type;
    char         *addr;
    char         *user;
    char         *enablestr;
    char         *yfstr;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((name = xml_find_body(xn, "name")) == NULL)
        goto ok;
    if ((enablestr = xml_find_body(xn, "enabled")) == NULL){
        goto ok;
    }
    dh = device_handle_find(h, name); /* can be NULL */
    if (strcmp(enablestr, "false") == 0){
        if ((dh = device_handle_new(h, name)) == NULL)
            goto done;
        device_handle_logmsg_set(dh, strdup("Configured down"));
        goto ok;
    }
    if (dh != NULL &&
        device_handle_conn_state_get(dh) != CS_CLOSED)
        goto ok;
    /* Only handle netconf/ssh */
    if ((type = xml_find_body(xn, "conn-type")) == NULL ||
        strcmp(type, "NETCONF_SSH"))
        goto ok;
    if ((addr = xml_find_body(xn, "addr")) == NULL)
        goto ok;
    user = xml_find_body(xn, "user");
    /* Now dh is either NULL or in closed state and with correct type 
     * First create it if still NULL
     */
    if (dh == NULL &&
        (dh = device_handle_new(h, name)) == NULL)
        goto done;
    if ((yfstr = xml_find_body(xn, "yang-config")) != NULL)
        device_handle_yang_config_set(dh, yfstr); /* Cache yang config */    
    if (connect_netconf_ssh(h, dh, user, addr) < 0) /* match */
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Get local (cached) device datastore
 *
 * @param[in] h        Clixon handle
 * @param[in] devname  Name of device
 * @param[in] extended Extended name
 * @param[out] xrootp  Device config XML (if retval=1)
 * @param[out] cbret   Error message (if retval=0)
 * @retval    1        OK
 * @retval    0        Failed
 * @retval   -1        Error
 */
static int
get_device_db(clicon_handle h,
              char         *devname,
              char         *extended,
              cxobj       **xrootp,
              cbuf         *cbret)
{
    int    retval = -1;
    cbuf  *cbdb = NULL;
    char  *db;
    cxobj *xt = NULL;
    cxobj *xroot;
    
    if ((cbdb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }   
    if (extended)
        cprintf(cbdb, "device-%s-%s", devname, extended);
    else
        cprintf(cbdb, "device-%s", devname);
    db = cbuf_get(cbdb);
    if (xmldb_get(h, db, NULL, NULL, &xt) < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/root")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No such device tree")< 0)
            goto done;
        goto failed;
    }
    if (xrootp){
        xml_rm(xroot);
        *xrootp = xroot;
    }
    retval = 1;
 done:
    if (cbdb)
        cbuf_free(cbdb);
    if (xt)
        xml_free(xt);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Incoming rpc handler to sync from one or several devices
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  tid     Transaction id
 * @param[out] 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. , if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
push_device(clixon_handle h,
            device_handle dh,
            uint64_t      tid,
            int          *changed,
            cbuf         *cbret)
{
    int        retval = -1;
    cxobj     *x0 = NULL;
    cxobj     *x1;
    cxobj     *x1t = NULL;
    cvec      *nsc = NULL;
    cbuf      *cb = NULL;
    char      *name;
    cxobj    **dvec = NULL;
    int        dlen;
    cxobj    **avec = NULL;
    int        alen;
    cxobj    **chvec0 = NULL;
    cxobj    **chvec1 = NULL;
    int        chlen;
    yang_stmt *yspec;

    /* 1) get previous device synced xml */
    name = device_handle_name_get(dh);
    if (get_device_db(h, name, NULL, &x0, cbret) < 0)
        goto done;
    /* 2) get current and compute diff with previous */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }

    cprintf(cb, "devices/device[name='%s']/root", name);
    if (xmldb_get0(h, "running", YB_MODULE, nsc, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &x1t, NULL, NULL) < 0)
        goto done;
    if ((x1 = xpath_first(x1t, nsc, "%s", cbuf_get(cb))) == NULL){
        if (netconf_operation_failed(cbret, "application", "Device not configured")< 0)
            goto done;
        goto fail;
    }
    if ((yspec = device_handle_yspec_get(dh)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No YANGs in device")< 0)
            goto done;
        goto fail;
    }
    if (xml_diff(yspec, 
                 x0, x1,
                 &dvec, &dlen,
                 &avec, &alen,
                 &chvec0, &chvec1, &chlen) < 0)
        goto done;
    /* 3) construct an edit-config, send it and validate it */
    if (dlen || alen || chlen){
        (*changed)++;
        if (device_send_edit_config_diff(h, dh,
                                         x0, x1, yspec,
                                         dvec, dlen,
                                         avec, alen,
                                         chvec0, chvec1, chlen) < 0)
            goto done;
        device_handle_tid_set(dh, tid);
        if (device_state_set(dh, CS_PUSH_EDIT) < 0)
            goto done;
        /* 4) phase 2 commit (XXX later) */
    }
    retval = 1;
 done:
    if (dvec)
        free(dvec);
    if (avec)
        free(avec);
    if (chvec0)
        free(chvec0);
    if (chvec1)
        free(chvec1);
    if (cb)
        cbuf_free(cb);
    if (x0)
        xml_free(x0);
    if (x1t)
        xml_free(x1t);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Incoming rpc handler to sync from one or several devices
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  tid     Transaction id
 * @param[in]  dryrun  If set, dont install (commit) retreived config
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
pull_device(clixon_handle h,
            device_handle dh,
            uint64_t      tid,
            int           dryrun,
            cbuf         *cbret)
{
    int  retval = -1;
    int  s;

    clicon_debug(1, "%s", __FUNCTION__);
    s = device_handle_socket_get(dh);
    if (device_send_sync(h, dh, s) < 0)
        goto done;
    if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
        goto done;
    device_handle_dryrun_set(dh, dryrun);
    device_handle_tid_set(dh, tid);
    retval = 1;
 done:
    return retval;
}

/*! Incoming rpc handler to sync from or to one or several devices
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  ce      Client entry
 * @param[in]  push    0: pull, 1: push
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync_common(clixon_handle        h,
                cxobj               *xe,
                struct client_entry *ce,
                int                  push,
                cbuf                *cbret)
{
    int                     retval = -1;
    char                   *pattern = NULL;
    cxobj                  *xret = NULL;
    cxobj                  *xn;
    cvec                   *nsc = NULL;
    cxobj                 **vec = NULL;
    size_t                  veclen;
    int                     i;
    char                   *devname;
    device_handle           dh;
    int                     ret;
    controller_transaction *ct = NULL;
    int                     touched = 0;
    char                   *bd;
    int                     dryrun = 0; /* only pull */
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* Initiate new transaction */
    if (controller_transaction_new(h, &ct) < 0)
        goto done;
    ct->ct_client_id = ce->ce_id;
    pattern = xml_find_body(xe, "devname");
    if ((bd = xml_find_body(xe, "dryrun")) != NULL)
        dryrun = strcmp(bd, "true") == 0;
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (push == 1){
            if ((ret = push_device(h, dh, ct->ct_id, &touched, cbret)) < 0)
                goto done;
        }
        else{
            if ((ret = pull_device(h, dh, ct->ct_id, dryrun, cbret)) < 0)
                goto done;
        }
        if (ret == 0)  /* Failed but cbret set */
            goto ok;
        touched++;
    } /* for */
    if (touched == 0){
        if (netconf_operation_failed(cbret, "application", "No syncs activated")< 0)
            goto done;
        goto ok;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}
/*! Read the config of one or several devices
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync_pull(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    struct client_entry *ce = (struct client_entry *)arg;

    return rpc_sync_common(h, xe, ce, 0, cbret);
}

/*! Push the config to one or several devices
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync_push(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    struct client_entry *ce = (struct client_entry *)arg;
    
    return rpc_sync_common(h, xe, ce, 1, cbret);
}

/*! Get configuration db of a single device of name 'device-<devname>-<postfix>.xml'
 *
 * Typically this db is retrieved by the sync-pull rpc
 * Should probably be replaced by a more generic function.
 * Possibly just extend get-config with device dbs?";
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_get_device_config(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,
                      void         *regarg)
{
    int           retval = -1;
    char         *devname;
    cxobj        *xroot = NULL;
    char         *extended;
    int           ret;

    devname = xml_find_body(xe, "devname");
    extended = xml_find_body(xe, "extended");
    if ((ret = get_device_db(h, devname, extended, &xroot, cbret)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if (clixon_xml2cbuf(cbret, xroot, 0, 0, -1, 0) < 0)
        goto done;
    cprintf(cbret, "</config>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (xroot)
        xml_free(xroot);
    return retval;
}

/*! (Re)connect try an enabled device in CLOSED state.
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_reconnect(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    int                     retval = -1;
    char                   *pattern = NULL;
    cxobj                  *xret = NULL;
    cxobj                  *xn;
    cvec                   *nsc = NULL;
    cxobj                 **vec = NULL;
    size_t                  veclen;
    int                     i;
    char                   *devname;
    device_handle           dh;
    controller_transaction *ct = NULL;
    struct client_entry    *ce;
    int                     touched = 0;
    
    clicon_debug(1, "%s", __FUNCTION__);
    ce = (struct client_entry *)arg;
    if (controller_transaction_new(h, &ct) < 0)
        goto done;
    ct->ct_client_id = ce->ce_id;
    pattern = xml_find_body(xe, "devname");
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    touched=0;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_CLOSED)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (controller_connect(h, xn) < 0)
            goto done;
        touched++;
    } /* for */
    if (touched == 0){
        if (netconf_operation_failed(cbret, "application", "No reconnects activated")< 0)
            goto done;
        goto ok;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Terminate an ongoing transaction with an error condition
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_transaction_error(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,  
                      void         *regarg)
{
    int           retval = -1;

    retval = 0;
    // done:
    return retval;
}

/*! Register callback for rpc calls */
int
controller_rpc_init(clicon_handle h)
{
    int retval = -1;
    
    if (rpc_callback_register(h, rpc_sync_pull,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "sync-pull"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_sync_push,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "sync-push"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_reconnect,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "reconnect"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_get_device_config,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "get-device-config"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_transaction_error,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "transaction-error"
                              ) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
