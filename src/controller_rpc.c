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
    cbuf_reset(cb); /* reuse cb for event dbg str */
    cprintf(cb, "Netconf ssh %s", addr);
    if (clixon_event_reg_fd(s, device_input_cb, dh, cbuf_get(cb)) < 0)
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
 * @param[in]  h      Clixon handle
 * @param[in]  xn     XML of device config
 * @param[in]  ct     Transaction
 * @param[out] reason reason, if retval is 0
 * @retval     1      OK
 * @retval     0      Connection can not be set up, see reason
 * @retval    -1      Error
 */
static int
controller_connect(clixon_handle           h,
                   cxobj                  *xn,
                   controller_transaction *ct,
                   char                  **reason)
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
    cxobj        *xb;
    cxobj        *xdevclass = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((name = xml_find_body(xn, "name")) == NULL)
        goto ok;
    if ((enablestr = xml_find_body(xn, "enabled")) == NULL)
        goto ok;
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
    /* Find device-class object if any */
    if ((xb = xml_find_type(xn, NULL, "device-class", CX_ELMNT)) != NULL){
        xdevclass = xpath_first(xn, NULL, "../device-class[name='%s']", xml_body(xb));
    }
    if ((xb = xml_find_type(xn, NULL, "conn-type", CX_ELMNT)) == NULL)
        goto ok;
    /* If not explicit value (default value set) AND device-class set, use that */
    if (xml_flag(xb, XML_FLAG_DEFAULT) &&
        xdevclass)
        xb = xml_find_type(xdevclass, NULL, "conn-type", CX_ELMNT);
    /* Only handle netconf/ssh */
    if ((type = xml_body(xb)) == NULL ||
        strcmp(type, "NETCONF_SSH")){
        if ((*reason = strdup("Connect failed: conn-type missing or not NETCONF_SSH")) == NULL)
            goto done;
        goto failed;
    }
    if ((addr = xml_find_body(xn, "addr")) == NULL){
        if ((*reason = strdup("Connect failed: addr missing")) == NULL)
            goto done;
        goto failed;
    }
    if ((xb = xml_find_type(xn, NULL, "user", CX_ELMNT)) == NULL &&
        xdevclass){
        xb = xml_find_type(xdevclass, NULL, "user", CX_ELMNT);
    }
    if (xb != NULL)
        user = xml_body(xb);
    /* Now dh is either NULL or in closed state and with correct type 
     * First create it if still NULL
     */
    if (dh == NULL &&
        (dh = device_handle_new(h, name)) == NULL)
        goto done;
    if ((xb = xml_find_type(xn, NULL, "yang-config", CX_ELMNT)) == NULL)
        goto ok;
    if (xml_flag(xb, XML_FLAG_DEFAULT) &&
        xdevclass)
        xb = xml_find_type(xdevclass, NULL, "yang-config", CX_ELMNT);
    if ((yfstr = xml_body(xb)) == NULL){
        if ((*reason = strdup("Connect failed: yang-config missing from device config")) == NULL)
            goto done;
        goto failed;
    }
    device_handle_yang_config_set(dh, yfstr); /* Cache yang config */
    /* Point of no return: assume errors handled in device_input_cb */
    device_handle_tid_set(dh, ct->ct_id);
    if (connect_netconf_ssh(h, dh, user, addr) < 0) /* match */
        goto done;
 ok:
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Compute diff, construct edit-config and send to device
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  ct      Transaction 
 * @param[in]  db      Device datastore
 * @param[out] cberr   Error message
 * @retval     1       OK
 * @retval     0       Failed, cbret set
 * @retval    -1       Error
 * @see devices_diff  for top-level all devices
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
#if 0 // debug
    fprintf(stderr, "%s before push x1 db:%s:\n", __FUNCTION__, db);
    xml_creator_print(stderr, x1);
#endif
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
    if (controller_transaction_failed(ct->ct_h, ct->ct_id, ct, NULL, 0, "Actions", "Timeout waiting for action daemon") < 0)
        goto done;
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
 * @param[in]  td      Local diff transaction
 * @param[out] services 0: There are no service configuration
 * @param[out] cvv     Vector of changed service instances, on the form name:<service> value:<instance>
 * @retval     0       OK, cb including notify msg (or not)
 * @retval    -1       Error
 * @see devices_diff   where diff is constructed 
 */
static int
controller_actions_diff(clixon_handle           h,
                        controller_transaction *ct,
                        transaction_data_t     *td,
                        int                    *services,
                        cvec                   *cvv)
{
    int     retval = -1;
    cvec   *nsc = NULL;
    cxobj  *x0s;
    cxobj  *x1s;
    cxobj  *xn;
    char   *instance;
    cxobj  *xi;
    cbuf   *cb = NULL;
    
    x0s = xpath_first(td->td_src, nsc, "services");
    x1s = xpath_first(td->td_target, nsc, "services");
    if (x0s == NULL && x1s == NULL){
        *services = 0;
        goto ok;
    }
    *services = 1;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Check deleted */
    if (x0s){
        xn = NULL;
        while ((xn = xml_child_each(x0s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_DEL) == 0)
                continue;
            /* Assume first entry is key, Alt: get key via YANG */
            if ((xi = xml_find_type(xn, NULL, NULL, CX_ELMNT)) == NULL ||
                (instance = xml_body(xi)) == NULL)
                continue;
            /* XXX See also service_action_one where tags are also created */
            cprintf(cb, "%s[%s='%s']", xml_name(xn), xml_name(xi), instance);
            if (cvec_add_string(cvv, cbuf_get(cb), NULL) < 0){
                clicon_err(OE_UNIX, errno, "cvec_add_string");
                goto done;
            }
            cbuf_reset(cb);
        }
    }
    /* Check added */
    if (x1s){
        xn = NULL;
        while ((xn = xml_child_each(x1s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE|XML_FLAG_ADD) == 0)
                continue;
            /* Assume first entry is key, Alt: get key via YANG */
            if ((xi = xml_find_type(xn, NULL, NULL, CX_ELMNT)) == NULL ||
                (instance = xml_body(xi)) == NULL)
                continue;
            /* XXX See also service_action_one where tags are also created */
            cprintf(cb, "%s[%s='%s']", xml_name(xn), xml_name(xi), instance);
            if (cvec_add_string(cvv, cbuf_get(cb), NULL) < 0){
                clicon_err(OE_UNIX, errno, "cvec_add_string");
                goto done;
            }
            cbuf_reset(cb);
        }
    }
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! XML apply function: check if any of the creators of x is in cvv
 *
 * cvv is on the form <service><instance key>
 * 1. cvv can be NULL, then any creator applies
 * 2. Should not be removed if another creator exists, then that creator should just be removed
 * 3. Should set a flag and remove later
 * @see text_modify
 */
static int
strip_device(cxobj *x,
             void  *arg)
{
    cvec   *cvv = (cvec*)arg;
    cg_var *cv = NULL;
    size_t  len;
    char   *tag;

    if ((len = xml_creator_len(x)) == 0)
        return 0;
    if (cvec_len(cvv) == 0){
        /* 1. cvv can be NULL, then any creator applies */
        xml_flag_set(x, XML_FLAG_MARK);
    }
    else while ((cv = cvec_each(cvv, cv)) != NULL){
            tag = cv_name_get(cv);
            if (xml_creator_find(x, tag) == 0)
                continue;
            if (len == 1)
                xml_flag_set(x, XML_FLAG_MARK);
            else 
                xml_creator_rm(x, tag);
        }
    return 0;
}

/*! Strip all service data in device config
 * 
 * Read a datastore, for each device in the datastore, strip data created by services 
 * as defined by the services vector cvv. Write back the changed datasrtore
 * @param[in]  h    Clicon handle 
 * @param[in]  db   Database
 * @param[in]  cvv  Vector of services
 * @retval     0    OK
 * @retval    -1    Error
 * @note Somewhat raw algorithm to replace the top-level tree, could do with op=REMOVE
 *       of the sub-parts instead of marking and remove
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
#if 0 // debug
        fprintf(stderr, "%s before strip xd:\n", __FUNCTION__);
        xml_creator_print(stderr, xd); 
#endif
        if (xml_apply(xd, CX_ELMNT, strip_device, cvv) < 0)
            goto done;
        if (xml_tree_prune_flags(xd, XML_FLAG_MARK, XML_FLAG_MARK) < 0)
            goto done;
        if (xml_apply(xd, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK)) < 0)
            goto done;
#if 0 // debug
        fprintf(stderr, "%s after strip xd:\n", __FUNCTION__);
        xml_creator_print(stderr, xd); 

#endif
    }
    if (veclen){
        if ((cbret = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        /* XXX Somewhat raw to replace the top-level tree, could do with op=REMOVE
         * of the sub-parts instead of marking and remove
         */
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
 * Devices are removed of no device diff
 * @param[in]  h    Clixon handle
 * @param[in]  ct   Transaction
 */
static int
commit_push_after_actions(clixon_handle           h,
                          controller_transaction *ct)
{
    int           retval = -1;
    cbuf         *cberr = NULL;
    int           ret;

    if (ct->ct_push_type == PT_NONE){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
    }
    else{
        /* Compute diff of candidate + commit and trigger service 
         * If some device diff is zero, then remove device from transaction
         */
        if ((ret = controller_commit_push(h, ct, "actions", &cberr)) < 0)
            goto done;
        if (ret == 0){
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
            if (ct->ct_actions_type != AT_NONE && strcmp(ct->ct_sourcedb, "candidate")==0){
                if ((cberr = cbuf_new()) == NULL){
                    clicon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                /* What to copy to candidate and commit to running? */
                if (xmldb_copy(h, "actions", "candidate") < 0)
                    goto done;
                /* XXX: recursive creates transaction */
                if ((ret = candidate_commit(h, NULL, "candidate", 0, 0, cberr)) < 0){
                    /* Handle that candidate_commit can return < 0 if transaction ongoing */
                    cprintf(cberr, "%s", clicon_err_reason); // XXX encode
                    ret = 0;
                }
                if (ret == 0){ // XXX awkward, cb ->xml->cb
                    cxobj *xerr = NULL;
                    cbuf *cberr2 = NULL;
                    if ((cberr2 = cbuf_new()) == NULL){
                        clicon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    if (clixon_xml_parse_string(cbuf_get(cberr), YB_NONE, NULL, &xerr, NULL) < 0)
                        goto done;
                    if (netconf_err2cb(xerr, cberr2) < 0)
                        goto done;
                    if (controller_transaction_failed(h, ct->ct_id, ct, NULL, 1,
                                                      NULL,
                                                      cbuf_get(cberr2)) < 0)
                        goto done;
                    if (xerr)
                        xml_free(xerr);
                    if (cberr2)
                        cbuf_free(cberr2);
                    goto ok;
                }
            }
            if ((ct->ct_reason = strdup("No device  configuration changed, no push necessary")) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
            if (controller_transaction_notify(h, ct) < 0)
                goto done;
        }
        else{
            /* Some or all started */
        }
    }
 ok:
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
                          actions_type            actions,
                          transaction_data_t     *td)
{
    int     retval = -1;
    cbuf   *notifycb = NULL;
    cvec   *cvv = NULL;       /* Format: <service> <instance> */
    int     services = 0;
    cg_var *cv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
        clicon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    /* Get candidate and running, compute diff and get notification msg in return */
    if (controller_actions_diff(h, ct, td, &services, cvv) < 0)
        goto done;
    if (actions == AT_FORCE)
        cvec_reset(cvv);
    /* 1) copy candidate to actions and remove all device config tagged with services */
    if (xmldb_copy(h, "candidate", "actions") < 0)
        goto done;
    /* Strip service data in device config for services that changed */
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
                cprintf(notifycb, "<service>");
                xml_chardata_cbuf_append(notifycb, cv_name_get(cv));
                cprintf(notifycb, "</service>");
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

/*! Mark devices with transaction-id if name matches device pattern AND state is OPEN
 *
 * Read running config and compare configured devices with the selection pattern
 * and its state is open, then set the tid on that device
 * If state of a selected device is not open, then return first closed device
 * @param[in]  h       Clicon handle 
 * @param[in]  device  Name of device to push to, can use wildchars for several, or NULL for all
 * @param[in]  tid     Transaction id
 * @param[out] closed  Device handle of first closed device, if any
 * @retval     0       OK, note if "closed" is set, then matching was interrupted
 * @retval    -1       Error
 * @note  cleanup (unmarking) must be done by calling function, even if closed is non-NULL
 */
static int 
devices_match(clixon_handle   h,
              char           *device,
              uint64_t        tid,
              device_handle  *closed)
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
    cbuf         *reason = NULL;
    
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
        if (device_handle_conn_state_get(dh) != CS_OPEN &&
            *closed == NULL){
            *closed = dh;
        }
        /* Include device in transaction */
        device_handle_tid_set(dh, tid);        
    } /* for */
    retval = 0;
 done:
    if (reason)
        cbuf_free(reason);
    if (xret)
        xml_free(xret);
    if (vec)
        free(vec);
    return retval;
}

/*! Check if any local/meta device fields have changed in the selected device set
 *
 * These fields are ones that effect the connection to a device from clixon-controller.yang:
 * For simplicitly, these are any config leaf under the device container, EXCEPT config
 * In particular: enabled, conn-type, user, addr
 * If local diffs are made, a device push should probably not be done since a connect may be
 * necessary before the push to open/close/change device connections
 * @param[in]  h       Clixon handle
 * @param[in]  td      Transaction diff
 * @param[out] changed Device handle of changed device, if any
 * @retval     0       OK
 * @retval    -1       Error
 * @see devices_diff   where diff is constructed 
 * @see devices_match  similar selection, but based on open devices
 */
static int
devices_local_change(clixon_handle       h,
                     transaction_data_t *td,
                     device_handle      *changed)
{
    int    retval = -1;
    cxobj *x0d;
    cxobj *x1d;
    cxobj *xd = NULL;
    cxobj *xi;
    cvec  *nsc = NULL;
    char  *name;

    x0d = xpath_first(td->td_src, nsc, "devices");
    x1d = xpath_first(td->td_target, nsc, "devices");
    if (x0d && td->td_dlen){     /* Check deleted */
        xd = NULL;
        while ((xd = xml_child_each(x0d, xd,  CX_ELMNT)) != NULL){
            xi = NULL;
            while ((xi = xml_child_each(xd, xi,  CX_ELMNT)) != NULL){
                if (strcmp(xml_name(xi), "config") != 0 &&
                    xml_flag(xi, XML_FLAG_DEL) != 0){
                    break;
                }
            }
            if (xi != NULL)
                break;
        }
    }
    if (xd==NULL && x1d && (td->td_alen || td->td_clen)){ /* Check added or changed */
        xd = NULL;
        while ((xd = xml_child_each(x1d, xd,  CX_ELMNT)) != NULL){
            xi = NULL;
            while ((xi = xml_child_each(xd, xi,  CX_ELMNT)) != NULL){
                if (strcmp(xml_name(xi), "config") != 0 &&
                    xml_flag(xi, XML_FLAG_CHANGE|XML_FLAG_ADD) != 0){
                    break;
                }
            }
            if (xi != NULL)
                break;
        }
    }
    if (xd){
        name = xml_find_body(xd, "name");
        if ((*changed = device_handle_find(h, name)) == NULL){
            clicon_err(OE_XML, 0, "device %s not found in transaction", name);
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Diff candidate/running and fill in a diff transaction structure for devices in transaction
 *
 * @param[in]  h   Clixon handle
 * @param[in]  ct  Controller transaction
 * @param[out] td  diff structure
 * @retval     0   OK
 * @retval    -1   Error
 */
static int
devices_diff(clixon_handle           h,
             controller_transaction *ct,
             transaction_data_t     *td)
{
    int           retval = -1;
    cvec         *nsc = NULL;
    cxobj        *xn;
    device_handle dh;
    char         *name;
    int           i;
    
    if (xmldb_get0(h, "candidate", YB_MODULE, nsc, "/", 1, WITHDEFAULTS_EXPLICIT, &td->td_target, NULL, NULL) < 0)
        goto done;
    if (xmldb_get0(h, "running", YB_MODULE, nsc, "/", 1, WITHDEFAULTS_EXPLICIT, &td->td_src, NULL, NULL) < 0)
        goto done;
    /* Remove devices not in transaction */
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) == ct->ct_id)
            continue;
        name = device_handle_name_get(dh);
        if ((xn = xpath_first(td->td_src, nsc, "devices/device[name='%s']", name)) != NULL)
            xml_purge(xn);
        if ((xn = xpath_first(td->td_target, nsc, "devices/device[name='%s']", name)) != NULL)
            xml_purge(xn);
    }
    if (xml_diff(td->td_src,
                 td->td_target,
                 &td->td_dvec,      /* removed: only in running */
                 &td->td_dlen,
                 &td->td_avec,      /* added: only in candidate */
                 &td->td_alen,
                 &td->td_scvec,     /* changed: original values */
                 &td->td_tcvec,     /* changed: wanted values */
                 &td->td_clen) < 0)
        goto done;
    /* Mark flags, see also validate_common */
    for (i=0; i<td->td_dlen; i++){ /* Also down */
        xn = td->td_dvec[i];
        xml_flag_set(xn, XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_alen; i++){ /* Also down */
        xn = td->td_avec[i];
        xml_flag_set(xn, XML_FLAG_ADD|XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_clen; i++){ /* Also up */
        xn = td->td_scvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        xn = td->td_tcvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    retval = 0;
 done:
    return retval;
}

/*! Helpful error message if a device is closed or changed
 * 
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Controller transaction
 * @param[in]  dh     Device handle (reason=0,1)
 * @param[in]  reason 0: closed, 1: changed, 2: no devices
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     0      OK
 * @retval    -1      Error
 */
static int
device_error(clicon_handle           h,
             controller_transaction *ct,
             device_handle           dh,
             int                     reason,
             cbuf                   *cbret)
{
    int   retval = -1;
    cbuf *cb = NULL;
    char *name = NULL;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (dh)
        name = device_handle_name_get(dh);
    if (reason == 0) /* closed */
        cprintf(cb, "Device is closed: '%s' (try 'connection open' or edit, local commit, and connect)", name);
    else if (reason == 1)  /* changed */
        cprintf(cb, "Device '%s': local fields are changed (try 'commit local' instead)", name);
    else                   /* empty */
        cprintf(cb, "No devices are selected (or no devices exist) and you have requested commit PUSH");
    if (netconf_operation_failed(cbret, "application", cbuf_get(cb))< 0)
        goto done;
    if (controller_transaction_done(h, ct, TR_FAILED) < 0)
        goto done;
    if (name && (ct->ct_origin = strdup(name)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    if ((ct->ct_reason = strdup(cbuf_get(cb))) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
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
    int                     retval = -1;
    client_entry           *ce = (client_entry *)arg;
    controller_transaction *ct = NULL;
    char                   *str;
    char                   *device;
    char                   *device_group;
    char                   *sourcedb = NULL;
    actions_type            actions = AT_NONE;
    push_type               pusht = PT_NONE;
    int                     ret;
    cbuf                   *cbtr = NULL;
    cbuf                   *cberr = NULL;
    device_handle           closed = NULL;
    device_handle           changed = NULL;
    transaction_data_t     *td = NULL;
    
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
    /* Mark devices with transaction-id if name matches device pattern AND state is OPEN */
    if (devices_match(h, device, ct->ct_id, &closed) < 0)
        goto done;
    /* If device is closed and push != NONE, then error */
    if (closed != NULL && pusht != PT_NONE){
        if (device_error(h, ct, closed, 0, cbret) < 0)
            goto done;
        goto ok;
    }
    /* If there are no devices selected and push != NONE */
    if (controller_transaction_devices(h, ct->ct_id) == 0 && pusht != PT_NONE){
        if (device_error(h, ct, closed, 2, cbret) < 0)
            goto done;
        goto ok;
    }
    /* Start local commit/diff transaction */
    if ((td = transaction_new()) == NULL)
        goto done;
    /* Diff candidate/running and fill in a diff transaction structure td for future use */
    if (devices_diff(h, ct, td) < 0)
        goto done;
    /* Check if any local/meta device fields have changed of selected devices */
    if (devices_local_change(h, td, &changed) < 0)
        goto done;
    if (changed != NULL){
        if (device_error(h, ct, changed, 1, cbret) < 0)
            goto done;
        goto ok;
    }
    switch (actions){
    case AT_NONE: /* Bypass actions, directly to push */
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
        }        
        break;
    case AT_CHANGE:
    case AT_FORCE:
        if (strcmp(ct->ct_sourcedb, "candidate") != 0){
            if (netconf_operation_failed(cbret, "application", "Only candidates db supported if actions")< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;
        }
        /* Compute diff of candidate + commit and trigger service-commit notify */
        if (controller_commit_actions(h, ct, actions, td) < 0)
            goto done;
        break;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (td){
        xmldb_get0_free(h, &td->td_target);
        xmldb_get0_free(h, &td->td_src);
        transaction_free(td);
    }
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
        case DT_ACTIONS:
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
    char                   *body;
    int                     enabled;
    device_handle           dh;
    controller_transaction *ct = NULL;
    client_entry           *ce;
    char                   *operation;
    cbuf                   *cberr = NULL;
    cbuf                   *cbtr = NULL;
    char                   *reason = NULL;
    int                     ret;
    
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
        if ((body = xml_find_body(xn, "enabled")) == NULL)
            continue;
        enabled = strcmp(body, "true")==0;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        dh = device_handle_find(h, devname);
        /* @see clixon-controller.yang connection-operation */
        if (strcmp(operation, "CLOSE") == 0){
            /* Close if there is a handle and it is OPEN */
            if (dh != NULL && device_handle_conn_state_get(dh) == CS_OPEN){
                if (device_close_connection(dh, "User request") < 0)
                    goto done;
            }
        }
        else if (strcmp(operation, "OPEN") == 0){
            /* Open if enabled and handle does not exist or it exists and is closed  */
            if (enabled &&
                (dh == NULL || device_handle_conn_state_get(dh) == CS_CLOSED)){
                if ((ret = controller_connect(h, xn, ct, &reason)) < 0)
                    goto done;
                if (ret == 0){
                    if (netconf_operation_failed(cbret, "application", reason)< 0)
                        goto done;
                    goto ok;
                }
            }
        }
        else if (strcmp(operation, "RECONNECT") == 0){
            /* First close it if there is a handle and it is OPEN */
            if (dh != NULL && device_handle_conn_state_get(dh) == CS_OPEN){
                if (device_close_connection(dh, "User request") < 0)
                    goto done;
            }
            /* Then open if enabled */
            if (enabled){
                if ((ret = controller_connect(h, xn, ct, &reason)) < 0)
                    goto done;
                if (ret == 0){
                    if (netconf_operation_failed(cbret, "application", reason)< 0)
                        goto done;
                    goto ok;
                }
            }
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
    if (reason)
        free(reason);
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
        /* Start device push process: compute diff send edit-configs */
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

/*! Given two datastores and xpath, return diff in textual form
 *
 * @param[in]   h      Clixon handle
 * @param[in]   xpath  XPath
 * @param[in]   db1    First datastore
 * @param[in]   db2    Second datastore
 * @param[in]   format Format of diff
 * @param[out]  cbret  CLIgen buff with NETCONF reply
 * @see datastore_diff_device   for intra-device diff
 */
static int
datastore_diff_dsref(clixon_handle    h,
                     char            *xpath,
                     char            *db1,
                     char            *db2,
                     enum format_enum format,
                     cbuf            *cbret)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xt1 = NULL;
    cxobj  *xt2 = NULL;
    cxobj  *x1;
    cxobj  *x2;
    
    if (xmldb_get0(h, db1, YB_NONE, NULL, xpath, 1, WITHDEFAULTS_EXPLICIT, &xt1, NULL, NULL) < 0)
        goto done;
    x1 = xpath_first(xt1, NULL, "%s", xpath);
    if (xmldb_get0(h, db2, YB_NONE, NULL, xpath, 1, WITHDEFAULTS_EXPLICIT, &xt2, NULL, NULL) < 0)
        goto done;
    x2 = xpath_first(xt2, NULL, "%s", xpath);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    switch (format){
    case FORMAT_XML:
        if (clixon_xml_diff2cbuf(cb, x1, x2) < 0)
            goto done;
        break;
    case FORMAT_TEXT:
        if (clixon_text_diff2cbuf(cb, x1, x2) < 0)
            goto done;
        break;
    case FORMAT_JSON:
    case FORMAT_CLI:
    default:
        break;
    }
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

/*! Given a device pattern, return diff in textual form between different device configs
 *
 * That is diff of configs for same device, only different variants, eg synced, transient, running, etc
 * @param[in]   h       Clixon handle
 * @param[in]   xpath   XPath
 * @param[in]   pattern Glob pattern for selecting devices
 * @param[in]   dt1     Type of device config 1
 * @param[in]   dt2     Type of device config 2
 * @param[in]   format Format of diff
 * @param[out]  cbret   CLIgen buff with NETCONF reply
 * @see datastore_diff_dsref   For inter-datastore diff
 */
static int
datastore_diff_device(clixon_handle      h,
                      char              *xpath,
                      char              *pattern,
                      device_config_type dt1,
                      device_config_type dt2,
                      enum format_enum   format,
                      cbuf              *cbret)
{
    int           retval = -1;
    cbuf         *cb = NULL;
    cbuf         *cbxpath = NULL;
    cbuf         *cberr = NULL;
    cxobj        *x1;
    cxobj        *x2;
    cxobj        *x1m = NULL; /* malloced */
    cxobj        *x2m = NULL; 
    cvec         *nsc = NULL;
    cxobj        *xret = NULL;
    cxobj        *x1ret = NULL;
    cxobj        *x2ret = NULL;
    cxobj       **vec = NULL;
    size_t        veclen;
    char         *devname;
    cxobj        *xdev;
    device_handle dh;
    int           i;
    int           ret;
    char         *ct;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if ((cbxpath = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xmldb_get0(h, "running", Y_MODULE, nsc, "devices/device/name", 1, WITHDEFAULTS_EXPLICIT, &xret, NULL, NULL) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device/name", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xdev = vec[i];
        if ((devname = xml_body(xdev)) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        x1 = x1m = NULL;
        switch (dt1){
        case DT_RUNNING:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "running", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x1ret, NULL, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "devices/device/config");
            break;
        case DT_CANDIDATE:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "candidate", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x1ret, NULL, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "devices/device/config");
            break;
        case DT_ACTIONS:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "actions", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x1ret, NULL, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "devices/device/config");
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            ct = device_config_type_int2str(dt1);
            if ((ret = device_config_read(h, devname, ct, &x1m, &cberr)) < 0)
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
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "running", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x2ret, NULL, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "devices/device/config");
            break;
        case DT_CANDIDATE:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "candidate", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x2ret, NULL, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "devices/device/config");
            break;
        case DT_ACTIONS:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get0(h, "actions", Y_MODULE, nsc, cbuf_get(cbxpath), 1, WITHDEFAULTS_EXPLICIT, &x2ret, NULL, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "devices/device/config");
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            ct = device_config_type_int2str(dt2);
            if ((ret = device_config_read(h, devname, ct, &x2m, &cberr)) < 0)
                goto done;            
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            break;
        }
        switch (format){
        case FORMAT_XML:
            if (clixon_xml_diff2cbuf(cb, x1?x1:x1m, x2?x2:x2m) < 0)
                goto done;
            break;
        case FORMAT_TEXT:
            if (clixon_text_diff2cbuf(cb, x1?x1:x1m, x2?x2:x2m) < 0)
                goto done;
            break;
        case FORMAT_JSON:
        case FORMAT_CLI:
        default:
            break;
        }
        if (x1m){
            xml_free(x1m);
            x1m = NULL;
        }
        if (x2m){
            xml_free(x2m);
            x2m = NULL;
        }
        if (x1ret){
            xml_free(x1ret);
            x1ret = NULL;
        }
        if (x2ret){
            xml_free(x2ret);
            x2ret = NULL;
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
    if (x1ret)
        xml_free(x1ret);
    if (x2ret)
        xml_free(x2ret);
    if (cberr)
        cbuf_free(cberr);
    if (cb)
        cbuf_free(cb);
    if (cbxpath)
        cbuf_free(cbxpath);
    return retval;
}

/*! Compare two data-stores by returning a diff-list in XML
 *
 * Compare two data-stores by returning a diff-list in XML.
 * There are two variants: 
 *  1) Regular datastore references, such as running/candidate according to ietf-datastores YANG
 *  2) Clixon-controller specific device datastores
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg
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
    char              *formatstr;
    enum format_enum   format = FORMAT_XML;
                
    clicon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    xpath = xml_find_body(xe, "xpath");
    if ((formatstr = xml_find_body(xe, "format")) != NULL){
        if ((int)(format = format_str2int(formatstr)) < 0){
            clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
            goto done;
        }
        if (format != FORMAT_XML && format != FORMAT_TEXT){
            if (netconf_operation_failed(cbret, "application", "Format not supported")< 0)
                goto done;
            goto ok;
        }
    }

    if ((ds1 = xml_find_body(xe, "dsref1")) != NULL){ /* Regular datastores */
        if (nodeid_split(ds1, NULL, &id1) < 0)
            goto done;
        if ((ds2 = xml_find_body(xe, "dsref2")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No dsref2")< 0)
                goto done;
            goto ok;
        }
        if (nodeid_split(ds2, NULL, &id2) < 0)
            goto done;
        if (datastore_diff_dsref(h, xpath, id1, id2, format, cbret) < 0)
            goto done;
    }
    else{ /* Device specific datastores */
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
        if (datastore_diff_device(h, xpath, devname, dt1, dt2, format, cbret) < 0)
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

/*! Intercept services-commit create-subscription and deny if there is already one
 *
 * The registration should be made from plugin-init to ensure the check is made before
 * the regular from_client_create_subscription callback
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * @see clixon-controller.yang notification services-commit
 */
int
check_services_commit_subscription(clixon_handle h,
                                   cxobj        *xe,
                                   cbuf         *cbret,
                                   void         *arg,  
                                   void         *regarg)
{
    int                  retval = -1;
    //    struct client_entry *ce = (struct client_entry *)arg;
    char                *stream = "NETCONF";
    cxobj               *x; /* Generic xml tree */
    cvec                *nsc = NULL;
    event_stream_t      *es;
    struct stream_subscription *ss;
    int                         i;
        
    clicon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    /* XXX should use prefix cf edit_config */
    if ((nsc = xml_nsctx_init(NULL, EVENT_RFC5277_NAMESPACE)) == NULL)
        goto done;
    if ((x = xpath_first(xe, nsc, "//stream")) == NULL ||
        (stream = xml_find_value(x, "body")) == NULL ||
        (es = stream_find(h, stream)) == NULL)
        goto ok;
    if (strcmp(stream, "services-commit") != 0)
        goto ok;
    if ((ss = es->es_subscription) != NULL){
        i = 0;
        do {
            struct client_entry *ce = (struct client_entry *)ss->ss_arg;
            fprintf(stderr, "%s %d\n", __FUNCTION__, ce->ce_nr);
            ss = NEXTQ(struct stream_subscription *, ss);
            i++;
        } while (ss && ss != es->es_subscription);
        if (i>0){
            cbuf_reset(cbret);
            if (netconf_operation_failed(cbret, "application", "services-commit client already registered")< 0)
                goto done;
        }
    }
 ok:
    retval = 0;
 done:
    if (nsc)
        xml_nsctx_free(nsc);
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
    
    /* Check that services subscriptions is just done once */
    if (rpc_callback_register(h,
                              check_services_commit_subscription,
                              NULL,
                              EVENT_RFC5277_NAMESPACE, "create-subscription") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
