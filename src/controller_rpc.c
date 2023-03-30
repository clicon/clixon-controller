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
#include "controller_lib.h"
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
    clicon_data_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
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
 * @param[in] h    Clixon handle
 * @param[in] xn   XML of device config
 * @param[in] ct   Transaction
 * @retval    0    OK
 * @retval    -1   Error
 */
int
controller_connect(clixon_handle           h,
                   cxobj                  *xn,
                   controller_transaction *ct)
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
    /* Point of no return: assume errors handled in device_input_cb */
    device_handle_tid_set(dh, ct->ct_id);
    if (connect_netconf_ssh(h, dh, user, addr) < 0) /* match */
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Initiate a push to a singel device
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  ct      Transaction 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. , if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
push_device_one(clixon_handle           h,
                device_handle           dh,
                controller_transaction *ct,
                cbuf                   *cbret)
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
    cbuf      *cbmsg = NULL;
    int        s;

    /* 1) get previous device synced xml */
    name = device_handle_name_get(dh);
    if (device_config_read(h, name, "SYNCED", &x0, cbret) < 0)
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
    if (xml_diff(x0, x1,
                 &dvec, &dlen,
                 &avec, &alen,
                 &chvec0, &chvec1, &chlen) < 0)
        goto done;
    /* 3) construct an edit-config, send it and validate it */
    if (dlen || alen || chlen){
        if (device_create_edit_config_diff(h, dh,
                                           x0, x1, yspec,
                                           dvec, dlen,
                                           avec, alen,
                                           chvec0, chvec1, chlen,
                                           &cbmsg) < 0)
            goto done;
        device_handle_outmsg_set(dh, cbmsg);
        s = device_handle_socket_get(dh);
        if (device_send_sync(h, dh, s) < 0)
            goto done;
        device_handle_tid_set(dh, ct->ct_id);
        if (device_state_set(dh, CS_PUSH_CHECK) < 0)
            goto done;
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
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
pull_device_one(clixon_handle h,
                device_handle dh,
                uint64_t      tid,
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
    device_handle_tid_set(dh, tid);
    retval = 1;
 done:
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
    client_entry           *ce = (client_entry *)arg;
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
    char                   *str;
    cbuf                   *cberr = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* Initiate new transaction */
    if ((ret = controller_transaction_new(h, "sync pull", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    ct->ct_client_id = ce->ce_id;
    pattern = xml_find_body(xe, "devname");
    if ((str = xml_find_body(xe, "transient")) != NULL)
        ct->ct_pull_transient = strcmp(str, "true") == 0;
    if ((str = xml_find_body(xe, "merge")) != NULL)
        ct->ct_pull_merge = strcmp(str, "true") == 0;
    if (xmldb_get(h, "running", nsc, "devices/device", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN) /* maybe this is an error? */
            continue;
        if ((ret = pull_device_one(h, dh, ct->ct_id, cbret)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto ok;
    } /* for */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
    /* No device started, close transaction */
    if (controller_transaction_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    } 
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Push the config to one or several devices
 *
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
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    char                   *pattern = NULL;
    controller_transaction *ct = NULL;
    char                   *str;
    cxobj                  *xn;
    cvec                   *nsc = NULL;
    device_handle           dh;
    char                   *devname;
    cxobj                 **vec = NULL;
    size_t                  veclen;
    cxobj                  *xret = NULL;
    int                     i;
    int                     ret;
    cbuf                   *cberr = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* Initiate new transaction */
    if ((ret = controller_transaction_new(h, "sync push", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
            goto done;
        goto failed;
    }
    ct->ct_client_id = ce->ce_id;
    pattern = xml_find_body(xe, "devname");
    if ((str = xml_find_body(xe, "validate")) != NULL)
        ct->ct_push_validate = strcmp(str, "true") == 0;
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
        if ((ret = push_device_one(h, dh, ct, cbret)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto failed;
    } /* for */

    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
    /* No device started, close transaction */
    if (controller_transaction_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
 failed:
    retval = 0;
    goto done;
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
    int                retval = -1;
    char              *pattern;
    char              *devname;
    cxobj             *xroot = NULL;
    cxobj             *xroot1; /* dont free */
    char              *config_type;
    cvec              *nsc = NULL;
    cxobj             *xret = NULL;
    cxobj            **vec = NULL;
    size_t             veclen;
    cxobj             *xn;
    device_handle      dh;
    int                ret;
    int                i;
    cbuf              *cb = NULL;
    device_config_type dt;
    
    pattern = xml_find_body(xe, "devname");
    config_type = xml_find_body(xe, "config-type");
    dt = device_config_type_str2int(config_type);
    if (dt == DT_CANDIDATE){
        if (xmldb_get(h, "candidate", nsc, "devices/device", &xret) < 0)
            goto done;
    }
    else{
        if (xmldb_get(h, "running", nsc, "devices/device", &xret) < 0)
            goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cb, "<config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        switch (dt){
        case DT_RUNNING:
        case DT_CANDIDATE:
            xroot1 = xpath_first(xn, nsc, "root");
            if (clixon_xml2cbuf(cb, xroot1, 0, 0, -1, 0) < 0)
                goto done;
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            if ((ret = device_config_read(h, devname, config_type, &xroot, cbret)) < 0)
                goto done;
            if (ret == 0)
                goto ok;
            if (clixon_xml2cbuf(cb, xroot, 0, 0, -1, 0) < 0)
                goto done;
            if (xroot){
                xml_free(xroot);
                xroot = NULL;
            }
            break;
        }
    }
    cprintf(cb, "</config>");
    cprintf(cb, "</rpc-reply>");
    cprintf(cbret, "%s", cbuf_get(cb));
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
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
rpc_connection_change(clixon_handle h,
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
    client_entry           *ce;
    char                   *operation;
    int                     ret;
    cbuf                   *cberr = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    ce = (client_entry *)arg;
    if ((ret = controller_transaction_new(h, "connection change", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    ct->ct_client_id = ce->ce_id;
    pattern = xml_find_body(xe, "devname");
    operation = xml_find_body(xe, "operation");
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
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        /* @see clixon-controller.yang connection-operation */
        if (strcmp(operation, "CLOSE") == 0){
            if (device_handle_conn_state_get(dh) != CS_OPEN)
                continue;
            device_close_connection(dh, "User request");
        }
        else if (strcmp(operation, "OPEN") == 0){
            if (device_handle_conn_state_get(dh) != CS_CLOSED)
                continue;
            if (controller_connect(h, xn, ct) < 0)
                goto done;
        }
        else if (strcmp(operation, "RECONNECT") == 0){
            if (device_handle_conn_state_get(dh) != CS_CLOSED)
                device_close_connection(dh, "User request");
            if (controller_connect(h, xn, ct) < 0)
                goto done;
        }
        else {
            clicon_err(OE_NETCONF, 0, "%s is not a conenction-operation", operation);
            goto done;
        }
    } /* for */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
    /* No device started, close transaction */
    if (controller_transaction_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    } 
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Send a controller services-commit notification message
 */
int
services_commit_notify(clixon_handle h)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<services-commit xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "</services-commit>");
    if (stream_notify(h, "services-commit", "%s", cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Apply service scripts: trigger services-commit notification, 
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_services_apply(clixon_handle h,
                   cxobj        *xe,
                   cbuf         *cbret,
                   void         *arg,  
                   void         *regarg)
{
    int                     retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    if (services_commit_notify(h) < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
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
    int                     retval = -1;
    char                   *tidstr;
    char                   *origin;
    char                   *reason;
    uint64_t                tid;
    int                     ret;
    controller_transaction *ct;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((tidstr = xml_find_body(xe, "tid")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No tid")< 0)
            goto done;
        goto ok;
    }
    if ((ret = parse_uint64(tidstr, &tid, NULL)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "Invalid tid")< 0)
            goto done;
        goto ok;
    }
    if ((ct = controller_transaction_find(h, tid)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No such transaction")< 0)
            goto done;
        goto ok;
    }
    origin = xml_find_body(xe, "origin");
    reason = xml_find_body(xe, "reason");
    if (controller_transaction_failed(h, tid, ct, NULL, 0, origin, reason) < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
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
    if (rpc_callback_register(h, rpc_connection_change,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "connection-change"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_services_apply,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "services-apply"
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
