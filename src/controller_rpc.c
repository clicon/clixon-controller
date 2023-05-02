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
    device_handle_framing_type_set(dh, NETCONF_SSH_EOM);
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
 * @param[in]  cb      From where to compute diffs and push
 * @param[out] cbmsg   Error message
 * @retval     1       OK
 * @retval     0       Failed, cbret set
 * @retval    -1       Error
 */
static int 
push_device_one(clixon_handle           h,
                device_handle           dh,
                controller_transaction *ct,
                char                   *db,
                cbuf                  **cberr)
{
    int        retval = -1;
    cxobj     *x0 = NULL;
    cxobj     *x1;
    cxobj     *x1t = NULL;
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
    int        s;
    cbuf      *cbmsg = NULL;
    int        ret;
    cvec      *nsc = NULL;

    /* 1) get previous device synced xml */
    name = device_handle_name_get(dh);
    if ((ret = device_config_read(h, name, "SYNCED", &x0, cberr)) < 0)
        goto done;
    if (ret == 0)
        goto failed;
    /* 2) get current and compute diff with previous */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "devices/device[name='%s']/config", name);
    if (xmldb_get0(h, db, YB_MODULE, nsc, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &x1t, NULL, NULL) < 0)
        goto done;
    if ((x1 = xpath_first(x1t, nsc, "%s", cbuf_get(cb))) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }   
        cprintf(*cberr, "Device not configured");
        goto failed;
    }
    if ((yspec = device_handle_yspec_get(dh)) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }   
        cprintf(*cberr, "No YANGs in device");
        goto failed;
    }
    /* What to push to device? diff between synced and actionsdb */
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
        if (device_send_get_config(h, dh, s) < 0)
            goto done;
        device_handle_tid_set(dh, ct->ct_id);
        if (device_state_set(dh, CS_PUSH_CHECK) < 0)
            goto done;
    }
    else{
        device_handle_tid_set(dh, 0);
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
 failed:
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
    if (device_send_get_config(h, dh, s) < 0)
        goto done;
    if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
        goto done;
    device_handle_tid_set(dh, tid);
    retval = 1;
 done:
    return retval;
}

/*! Read the config of one or several remote devices
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
rpc_config_pull(clixon_handle h,
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
    if ((ret = controller_transaction_new(h, "pull", &ct, &cberr)) < 0)
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

/*! Timeout callback of service actions
 *
 * @param[in] s     Socket
 * @param[in] arg   Tranaction handle
 */
static int
actions_timeout(int   s,
                void *arg)
{
    int                     retval = -1;
    controller_transaction *ct = (controller_transaction *)arg;
    
    clicon_debug(1, "%s", __FUNCTION__);
    //    if (controller_transaction_failed(h, ct->ct_id, ct, dh, 2, name, "Timeout waiting for remote peer") < 0)
    //        goto done;
    if (ct->ct_state == TS_INIT){ /* 1.3 The transition is not in an error state */
        controller_transaction_state_set(ct, TS_RESOLVED, TR_FAILED);
        if ((ct->ct_origin = strdup("actions")) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        if ((ct->ct_reason = strdup("Timeout waiting for service actions to complete")) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        if (controller_transaction_notify(ct->ct_h, ct) < 0)
            goto done;
        if (controller_transaction_done(ct->ct_h, ct, TR_FAILED) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Set timeout of services action
 *
 * @param[in] h   Clixon handle
 */
static int
actions_timeout_register(controller_transaction *ct)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    int            d;
    
    clicon_debug(1, "%s", __FUNCTION__);
    gettimeofday(&t, NULL);
    d = clicon_data_int_get(ct->ct_h, "controller-device-timeout");
    if (d != -1)
        t1.tv_sec = d;
    else
        t1.tv_sec = 60;
    t1.tv_usec = 0;
    clicon_debug(1, "%s timeout:%ld s", __FUNCTION__, t1.tv_sec);
    timeradd(&t, &t1, &t);
    if (clixon_event_reg_timeout(t, actions_timeout, ct, "Controller service actions") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Cancel timeout 
 *
 * @param[in] h   Clixon handle
 */
static int
actions_timeout_unregister(controller_transaction *ct)
{
    (void)clixon_event_unreg_timeout(actions_timeout, ct);
    return 0;
}

/*! Get candidate and running, compute diff and return notification
 *
 * @param[in]  h       Clixon handle
 * @param[in]  ct      Transaction
 * @param[out] services 0: There are no service configuration
 * @param[out] cvv     Vector of services that have been changed.
 * @retval     0       OK, cb including notify msg (or not)
 * @retval    -1       Error
 */
static int
controller_actions_diff(clixon_handle           h,
                        controller_transaction *ct,
                        int                    *services,
                        cvec                   *cvv)
{
    int     retval = -1;
    cxobj  *x0t = NULL;
    cxobj  *x1t = NULL;
    cvec   *nsc = NULL;
    cxobj **dvec = NULL;
    int     dlen;
    cxobj **avec = NULL;
    int     alen;
    cxobj **chvec0 = NULL;
    cxobj **chvec1 = NULL;
    int     chlen;
    cxobj  *x0s;
    cxobj  *x1s;
    cxobj  *xn;
    int     i;
    
    if (xmldb_get0(h, "running", YB_MODULE, nsc, "services", 1, WITHDEFAULTS_EXPLICIT, &x0t, NULL, NULL) < 0)
        goto done;
    if (xmldb_get0(h, "candidate", YB_MODULE, nsc, "services", 1, WITHDEFAULTS_EXPLICIT, &x1t, NULL, NULL) < 0)
        goto done;
    x0s = xpath_first(x0t, nsc, "services");
    x1s = xpath_first(x1t, nsc, "services");
    if (x0s == NULL && x1s == NULL){
        *services = 0;
        goto ok;
    }
    *services = 1;
    if (xml_diff(x0t, x1t,
                 &dvec, &dlen,
                 &avec, &alen,
                 &chvec0, &chvec1, &chlen) < 0)
        goto done;    
    for (i=0; i<dlen; i++){ /* Also down */
        xn = dvec[i];
        xml_flag_set(xn, XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<alen; i++){ /* Also down */
        xn = avec[i];
        xml_flag_set(xn, XML_FLAG_ADD|XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<chlen; i++){ /* Also up */
        xn = chvec0[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        xn = chvec1[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* Check deleted */
    if (x0s){
        xn = NULL;
        while ((xn = xml_child_each(x0s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_DEL) == 0)
                continue;
            cvec_add_string(cvv, xml_name(xn), NULL);
        }
    }
    /* Check added  */
    if (x1s){
        xn = NULL;
        while ((xn = xml_child_each(x1s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE|XML_FLAG_ADD) == 0)
                continue;
            cvec_add_string(cvv, xml_name(xn), NULL);
        }
    }
 ok:
    retval = 0;
 done:
    if (x0t)
        xml_free(x0t);
    if (x1t)
        xml_free(x1t);
    if (dvec)
        free(dvec);
    if (avec)
        free(avec);
    if (chvec0)
        free(chvec0);
    if (chvec1)
        free(chvec1);
    return retval;
}

/*! XML apply function: check if any of the creators of x is in cvv
 *
 * 1. cvv can be NULL, then any creator applies
 * 2. Should not be removed if another creator exists, then that creator should just be removed
 * 3. Should set a flag and remove later
 */
static int
strip_device(cxobj *x,
             void  *arg)
{
    cvec   *cvv = (cvec*)arg;
    cg_var *cv = NULL;
    size_t  len;
    char   *name;

    if ((len = xml_creator_len(x)) == 0)
        return 0;
    if (cvec_len(cvv) == 0){
        /* 1. cvv can be NULL, then any creator applies */
        xml_flag_set(x, XML_FLAG_MARK);
    }
    else while ((cv = cvec_each(cvv, cv)) != NULL){
            name = cv_name_get(cv);
            if (xml_creator_find(x, name) == 0)
                continue;
            if (len == 1)
                xml_flag_set(x, XML_FLAG_MARK);
            else 
                xml_creator_rm(x, name);
        }
   return 0;
}

/*! Strip all service data in device config
 * 
 * Read a datastore, for each device in the datastore, strip data created by services 
 * as defined by the services vector cvv. Write back the changed datasrtore
 */
static int
strip_service_data_from_device_config(clixon_handle h,
                                      char         *db,
                                      cvec         *cvv)
{
    int     retval = -1;
    cxobj  *xt = NULL;
    cxobj  *xd;
    cbuf   *cbret = NULL;
    int     ret;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;

    if (xmldb_get0(h, db, YB_NONE, NULL, NULL, 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
        goto done;
    if (xpath_vec(xt, NULL, "devices/device/config", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xd = vec[i];
        if (xml_apply(xd, CX_ELMNT, strip_device, cvv) < 0)
            goto done;
        if (xml_tree_prune_flags(xd, XML_FLAG_MARK, XML_FLAG_MARK) < 0)
            goto done;
        if (xml_apply(xd, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK)) < 0)
        goto done;
    }
    if (veclen){
        if ((cbret = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if ((ret = xmldb_put(h, db, OP_REPLACE, xt, NULL, cbret)) < 0)
            goto done;
        if (ret == 0){
            clicon_err(OE_XML, 0, "xmldb_ut failed");
            goto done;
        }
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (cbret)
        cbuf_free(cbret);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Compute diff of candidate + commit and trigger service-commit notify
 *
 * @param[in]  h       Clixon handle 
 * @param[in]  ct      Transaction
 * @param[in]  db      From where to compute diffs and push
 * @param[out] cberr   Error message (if retval = 0)
 * @retval     1       OK
 * @retval     0       Failed
 * @retval    -1       Error
 */
static int 
controller_commit_push(clixon_handle           h,
                       controller_transaction *ct,
                       char                   *db,
                       cbuf                  **cberr)
{
    int           retval = -1;
    device_handle dh = NULL;
    int           ret;

    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) != ct->ct_id)
            continue;
        if ((ret = push_device_one(h, dh, ct, db, cberr)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto failed;        
    }
    retval = 1;
 done:
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Push commit after actions completed, potentially start device push process
 *
 * @param[in]  h    Clixon handle
 * @param[in]  ct   Transaction
 */
static int
commit_push_after_actions(clixon_handle           h,
                          controller_transaction *ct)
{
    int           retval = -1;
    device_handle dh;
    cbuf         *cberr = NULL;
    int           ret;

    if (ct->ct_push_type == PT_NONE){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    }
    else{
        if ((ret = controller_commit_push(h, ct, "actions", &cberr)) < 0)
            goto done;
        if (ret == 0){
            dh = NULL; /* unmark devices */
            while ((dh = device_handle_each(h, dh)) != NULL){
                if (device_handle_tid_get(dh) == ct->ct_id)
                    device_handle_tid_set(dh, 0);
            }
            if ((ct->ct_origin = strdup("controller")) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if ((ct->ct_reason = strdup(cbuf_get(cberr))) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            if (controller_transaction_notify(ct->ct_h, ct) < 0)
                goto done;                
        }
        /* No device started, close transaction */
        else if (controller_transaction_devices(h, ct->ct_id) == 0){
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
            if (controller_transaction_notify(h, ct) < 0)
                goto done;
        }        
        else{
            /* Some or all started */
        }
    }
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    return retval;
}

/*! Compute diff of candidate + commit and trigger service-commit notify
 *
 * @param[in]  h         Clicon handle 
 * @param[in]  ct        Transaction
 * @param[in]  actions   How to trigger service-commit notifications
 * @retval     0         OK
 * @retval    -1         Error
 */
static int 
controller_commit_actions(clixon_handle           h,
                          controller_transaction *ct,
                          actions_type            actions)
{
    int     retval = -1;
    cbuf   *notifycb = NULL;
    cvec   *cvv = NULL;
    int     services = 0;
    cg_var *cv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
        clicon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    /* Get candidate and running, compute diff and get notification msg in return */
    if (controller_actions_diff(h, ct, &services, cvv) < 0)
        goto done;
    actions = AT_FORCE; // XXX
    if (actions == AT_FORCE)
        cvec_reset(cvv);
    /* 1) copy candidate to actions and remove all device config tagged with services */
    if (xmldb_copy(h, "candidate", "actions") < 0)
        goto done;
    if (strip_service_data_from_device_config(h, "actions", cvv) < 0)
        goto done;
    if (services){
        if (actions == AT_FORCE || cvec_len(cvv) > 0){     /* There are service changes */
            if ((notifycb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(notifycb, "<services-commit xmlns=\"%s\">", CONTROLLER_NAMESPACE);
            cprintf(notifycb, "<tid>%" PRIu64"</tid>", ct->ct_id);
            cprintf(notifycb, "<source>actions</source>");
            cprintf(notifycb, "<target>actions</target>");
            while ((cv = cvec_each(cvv, cv)) != NULL){
                cprintf(notifycb, "<service>%s</service>", cv_name_get(cv));
            }
            cprintf(notifycb, "</services-commit>");

            clicon_debug(1, "%s stream_notify: services-commit: %" PRIu64, __FUNCTION__, ct->ct_id);
            if (stream_notify(h, "services-commit", "%s", cbuf_get(notifycb)) < 0)
                goto done;
            controller_transaction_state_set(ct, TS_ACTIONS, -1);
            if (actions_timeout_register(ct) < 0)
                goto done;
        }
        else { /* There are services, but no service changes, close transaction */
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
            if (controller_transaction_notify(h, ct) < 0)
                goto done;
        }
    }
    else{ /* No services, proceed to next step */
        if (commit_push_after_actions(h, ct) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (cvv)
        cvec_free(cvv);
    if (notifycb)
        cbuf_free(notifycb);
    return retval;
}


/*! Mark devices based on its name matching device glob field AND its state is OPEN
 *
 * Read running config and compare configured devices with the selection pattern
 * NYI: device-groups
 *
 * @param[in]  h         Clicon handle 
 * @param[in]  ct        Transaction
 * @param[in]  device    Name of device to push to, can use wildchars for several, or NULL for all
 * @param[out] cbret     Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     1         OK
 * @retval     0         Failed, one selected device not open
 * @retval    -1         Error
 */
static int 
controller_select_devices(clixon_handle           h,
                          controller_transaction *ct,
                          char                   *device)
{
    int           retval = -1;
    cxobj       **vec = NULL;
    size_t        veclen;
    cvec         *nsc = NULL;
    cxobj        *xn;
    cxobj        *xret = NULL;
    char         *name;
    device_handle dh;
    int           i;
    int           failed = 0;
    
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        /* Name of device or device-group */
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        if (device != NULL && fnmatch(device, name, 0) != 0)
            continue;
        if ((dh = device_handle_find(h, name)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN){
            failed++;
            break;
        }
        /* Mark if device matches and is open */
        device_handle_tid_set(dh, ct->ct_id);        
    } /* for */
    if (failed){     /* Reset marks */
        dh = NULL;
        while ((dh = device_handle_each(h, dh)) != NULL){
            if (device_handle_tid_get(dh) == ct->ct_id)
                device_handle_tid_set(dh, 0);
        }
        goto failed;
    }
    retval = 1;
 done:
    if (xret)
        xml_free(xret);
    if (vec)
        free(vec);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Extended commit: trigger actions and device push
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * TODO: device-groups
 */
static int 
rpc_controller_commit(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,  
                      void         *regarg)
{
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    controller_transaction *ct = NULL;
    char                   *str;
    char                   *device;
    char                   *device_group;
    char                   *sourcedb;
    actions_type            actions = AT_NONE;
    push_type               pusht = PT_NONE;
    int                     ret;
    cbuf                   *cbtr = NULL;
    cbuf                   *cberr = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    device = xml_find_body(xe, "device");
    if ((device_group = xml_find_body(xe, "device-group")) != NULL){
        if (netconf_operation_failed(cbret, "application", "Device-groups NYI")< 0)
            goto done;
        goto ok;
    }
    if ((str = xml_find_body(xe, "source")) == NULL){ /* on the form ds:running */
        if (netconf_operation_failed(cbret, "application", "sourcedb not supported")< 0)
            goto done;
        goto ok;
    }
    /* strip prefix, eg ds: */
    if (nodeid_split(str, NULL, &sourcedb) < 0)
        goto done;
    if (sourcedb == NULL ||
        (strcmp(sourcedb, "candidate") != 0 && strcmp(sourcedb, "running") != 0)){
        if (netconf_operation_failed(cbret, "application", "sourcedb not supported")< 0)
            goto done;
        goto ok;
    }
    if ((cbtr = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cbtr, "Controller commit");
    if ((str = xml_find_body(xe, "actions")) != NULL){
        actions = actions_type_str2int(str);
        cprintf(cbtr, " actions:%s", str);
    }
    if ((str = xml_find_body(xe, "push")) != NULL){
        pusht = push_type_str2int(str);
        cprintf(cbtr, " push:%s", str);
    }
    /* Initiate new transaction. 
     * NB: this locks candidate, which always needs to be unlocked, eg by controller_transaction_done
     */
    if ((ret = controller_transaction_new(h, cbuf_get(cbtr), &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    ct->ct_client_id = ce->ce_id;
    ct->ct_push_type = pusht;
    ct->ct_actions_type = actions;
    ct->ct_sourcedb = sourcedb;
    sourcedb = NULL;
    if ((ret = controller_select_devices(h, ct, device)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "Device closed")< 0)
            goto done;
        if (controller_transaction_done(h, ct, TR_FAILED) < 0)
            goto done;
        goto ok;
    }
    if (actions == AT_NONE){ /* Bypass actions, directly to push */
        if ((ret = controller_commit_push(h, ct, "running", &cberr)) < 0)
            goto done;
        if (ret == 0){
            if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;        
        }
        if (controller_transaction_devices(h, ct->ct_id) == 0){
            /* No device started, close transaction */
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
            if (controller_transaction_notify(h, ct) < 0)
                goto done;
        }        
    }
    else { /* Trigger actions */
        if (strcmp(ct->ct_sourcedb, "candidate") != 0){
            if (netconf_operation_failed(cbret, "application", "Only candidates db supported if actions")< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;
        }
        /* Compute diff of candidate + commit and trigger service-commit notify */
        if (controller_commit_actions(h, ct, actions) < 0)
            goto done;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (sourcedb)
        free(sourcedb);
    if (cbtr)
        cbuf_free(cbtr);
    if (cberr)
        cbuf_free(cberr);
    return retval;
}

/*! Get configuration db of a single device of name 'device-<devname>-<postfix>.xml'
 *
 * Typically this db is retrieved by the pull rpc
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
    cbuf              *cb = NULL;
    cbuf              *cberr = NULL;
    device_config_type dt;
    int                ret;
    int                i;
    
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
            xroot1 = xpath_first(xn, nsc, "config");
            if (clixon_xml2cbuf(cb, xroot1, 0, 0, NULL, -1, 0) < 0)
                goto done;
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            if ((ret = device_config_read(h, devname, config_type, &xroot, &cberr)) < 0)
                goto done;
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            if (clixon_xml2cbuf(cb, xroot, 0, 0, NULL, -1, 0) < 0)
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
    if (cberr)
        cbuf_free(cberr);
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
    cbuf                   *cbtr = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    ce = (client_entry *)arg;
    if ((cbtr = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cbtr, "Controller commit");
    pattern = xml_find_body(xe, "devname");
    operation = xml_find_body(xe, "operation");
    cprintf(cbtr, " %s", operation);
    if ((ret = controller_transaction_new(h, cbuf_get(cbtr), &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    ct->ct_client_id = ce->ce_id;
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
    if (cbtr)
        cbuf_free(cbtr);
    if (cberr)
        cbuf_free(cberr);
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
    if (controller_transaction_done(h, ct, TR_FAILED) < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Action scripts signal to backend that all actions are completed
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
rpc_transactions_actions_done(clixon_handle h,
                              cxobj        *xe,
                              cbuf         *cbret,
                              void         *arg,  
                              void         *regarg)
{
    int                     retval = -1;
    char                   *tidstr;
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
    switch (ct->ct_state){
    case TS_RESOLVED:
    case TS_INIT:
        if (netconf_operation_failed(cbret, "application", "Transaction in unexpected state") < 0)
            goto done;
        break;
    case TS_ACTIONS:
        actions_timeout_unregister(ct);
        cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
        cprintf(cbret, "<ok/>");
        cprintf(cbret, "</rpc-reply>");
        controller_transaction_state_set(ct, TS_INIT, -1); /* Multiple actions */
        if (commit_push_after_actions(h, ct) < 0)
            goto done;
        break;
    case TS_DONE:
        if (netconf_operation_failed(cbret, "application", "Transaction already completed(timeout?)") < 0)
            goto done;
        break;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*!
 */
static int
datastore_diff_dsref(clixon_handle h,
                     char         *xpath,
                     char         *id1,
                     char         *id2,
                     cbuf         *cbret)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xt1 = NULL;
    cxobj  *xt2 = NULL;
    cxobj  *x1;
    cxobj  *x2;
    
    if (xmldb_get0(h, id1, YB_NONE, NULL, xpath, 1, WITHDEFAULTS_EXPLICIT, &xt1, NULL, NULL) < 0)
        goto done;
    x1 = xpath_first(xt1, NULL, "%s", xpath);
    if (xmldb_get0(h, id2, YB_NONE, NULL, xpath, 1, WITHDEFAULTS_EXPLICIT, &xt2, NULL, NULL) < 0)
        goto done;
    x2 = xpath_first(xt2, NULL, "%s", xpath);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xml_tree_diff_print(cb, 0, x1, x2, FORMAT_XML) < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    xml_chardata_cbuf_append(cbret, cbuf_get(cb));
    cprintf(cbret, "</diff>");
    cprintf(cbret, "</rpc-reply>");    
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xt1)
        xml_free(xt1);
    if (xt2)
        xml_free(xt2);
    return retval;
}

/*!
 */
static int
datastore_diff_device(clixon_handle      h,
                      char              *xpath,
                      char              *pattern,
                      device_config_type dt1,
                      device_config_type dt2,
                      cbuf              *cbret)
{
    int           retval = -1;
    cbuf         *cb = NULL;
    cbuf         *cberr = NULL;
    cxobj        *x1;
    cxobj        *x2;
    cxobj        *x1m = NULL; /* malloced */
    cxobj        *x2m = NULL; 
    cvec         *nsc = NULL;
    cxobj        *xret = NULL;
    cxobj       **vec = NULL;
    size_t        veclen;
    char         *devname;
    cxobj        *xn;
    device_handle dh;
    int           i;
    int           ret;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xmldb_get0(h, "running", Y_MODULE, nsc, "devices/device", 1, WITHDEFAULTS_EXPLICIT, &xret, NULL, NULL) < 0)
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
        x1 = x1m = NULL;
        switch (dt1){
        case DT_RUNNING:
        case DT_CANDIDATE:
            x1 = xpath_first(xn, nsc, "config");
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            if ((ret = device_config_read(h, devname,
                                          device_config_type_int2str(dt1),
                                          &x1m, &cberr)) < 0)
                goto done;            
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            break;
        }
        x2 = x2m = NULL;
        switch (dt2){
        case DT_RUNNING:
        case DT_CANDIDATE:
            x2 = xpath_first(xn, nsc, "config");
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            if ((ret = device_config_read(h, devname,
                                          device_config_type_int2str(dt2),
                                          &x2m, &cberr)) < 0)
                goto done;            
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            break;
        }
        if (xml_tree_diff_print(cb, 0, x1?x1:x1m, x2?x2:x2m, FORMAT_XML) < 0)
            goto done;        
        if (x1m){
            xml_free(x1m);
            x1m = NULL;
        }
        if (x2m){
            xml_free(x2m);
            x2m = NULL;
        }
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    xml_chardata_cbuf_append(cbret, cbuf_get(cb));
    cprintf(cbret, "</diff>");
    cprintf(cbret, "</rpc-reply>");
    cprintf(cbret, "%s", cbuf_get(cb));
 ok:
    retval = 0;
 done:
    if (x1m)
        xml_free(x1m);
    if (x2m)
        xml_free(x2m);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (cberr)
        cbuf_free(cberr);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compare two data-storesby returning a diff-list in XML
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
rpc_datastore_diff(clixon_handle h,
                   cxobj        *xe,
                   cbuf         *cbret,
                   void         *arg,  
                   void         *regarg)
{
    int                retval = -1;
    char              *ds1;
    char              *ds2;
    char              *id1 = NULL;
    char              *id2 = NULL;
    device_config_type dt1;
    device_config_type dt2;
    char              *xpath;
    char              *devname;
                
    xpath = xml_find_body(xe, "xpath");
    if ((ds1 = xml_find_body(xe, "dsref1")) != NULL){
        if (nodeid_split(ds1, NULL, &id1) < 0)
            goto done;
        if ((ds2 = xml_find_body(xe, "dsref2")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No dsref2")< 0)
                goto done;
            goto ok;
        }
        if (nodeid_split(ds2, NULL, &id2) < 0)
            goto done;
        if (datastore_diff_dsref(h, xpath, id1, id2, cbret) < 0)
            goto done;
    }
    else{
        if ((devname = xml_find_body(xe, "devname")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No devname")< 0)
                goto done;
            goto ok;
        }
        if ((ds1 = xml_find_body(xe, "config-type1")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No config-type1")< 0)
                goto done;
            goto ok;
        }
        if ((dt1 = device_config_type_str2int(ds1)) == -1){
            if (netconf_operation_failed(cbret, "application", "Unexpected config-type")< 0)
                goto done;
            goto ok;
        }
        if ((ds2 = xml_find_body(xe, "config-type2")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No config-type1")< 0)
                goto done;
            goto ok;
        }
        if ((dt2 = device_config_type_str2int(ds2)) == -1){
            if (netconf_operation_failed(cbret, "application", "Unexpected config-type")< 0)
                goto done;
            goto ok;
        }
        if (datastore_diff_device(h, xpath, devname, dt1, dt2, cbret) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (id1)
        free(id1);
    if (id2)
        free(id2);
    return retval;
}

/*! Register callback for rpc calls */
int
controller_rpc_init(clicon_handle h)
{
    int retval = -1;
    
    if (rpc_callback_register(h, rpc_config_pull,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "config-pull"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_controller_commit,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "controller-commit"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_connection_change,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "connection-change"
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
    if (rpc_callback_register(h, rpc_transactions_actions_done,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "transaction-actions-done"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_datastore_diff,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "datastore-diff"
                              ) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
