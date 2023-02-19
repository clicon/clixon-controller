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
#include "controller_custom.h"
#include "controller.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"

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
    device_state_timeout_register(dh);
    device_handle_conn_state_set(dh, CS_CONNECTING);
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

/*! Incoming rpc handler to sync from one or several devices
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. , if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
push_device(clixon_handle h,
            device_handle dh,
            cbuf         *cbret)
{
    int        retval = -1;
    cxobj     *x0;
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
    if ((x0 = device_handle_sync_xml_get(dh)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No synced device tree")< 0)
            goto done;
        goto fail;
    }
    /* 2) get current and compute diff with previous */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    name = device_handle_name_get(dh);
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
        if (device_send_edit_config_diff(h, dh,
                                         x0, x1, yspec,
                                         dvec, dlen,
                                         avec, alen,
                                         chvec0, chvec1, chlen) < 0)
            goto done;
        device_handle_conn_state_set(dh, CS_PUSH_EDIT);
        if (device_state_timeout_register(dh) < 0)
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
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
pull_device(clixon_handle h,
            device_handle dh,
            cbuf         *cbret)
{
    int  retval = -1;
    int  s;

    clicon_debug(1, "%s", __FUNCTION__);
    s = device_handle_socket_get(dh);
    if (device_send_sync(h, dh, s) < 0)
        goto done;
    device_state_timeout_register(dh);
    device_handle_conn_state_set(dh, CS_DEVICE_SYNC);
    retval = 1;
 done:
    return retval;
}

/*! Incoming rpc handler to sync from or to one or several devices
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  push    0: pull, 1: push
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync(clixon_handle h,
         cxobj        *xe,
         cbuf         *cbret,
         int           push)
{
    int           retval = -1;
    char         *pattern = NULL;
    cxobj        *xret = NULL;
    cxobj        *xn;
    cvec         *nsc = NULL;
    cxobj       **vec = NULL;
    size_t        veclen;
    int           i;
    char         *devname;
    device_handle dh;
    int           ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
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
            if ((ret = push_device(h, dh, cbret)) < 0)
                goto done;
        }
        else{
            if ((ret = pull_device(h, dh, cbret)) < 0)
                goto done;
        }
        if (ret == 0)
            goto ok;
    } /* for */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
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
    return rpc_sync(h, xe, cbret, 0);
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
    return rpc_sync(h, xe, cbret, 1);
}

/*! Get last synced configuration of a single device
 *
 * Note that this could be done by some peek in commit history.
 * Should probably be replaced by a more generic function
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_get_device_sync_config(clixon_handle h,
                           cxobj        *xe,
                           cbuf         *cbret,
                           void         *arg,
                           void         *regarg)
{
    int           retval = -1;
    device_handle dh;
    char         *devname;
    cxobj        *xc;

    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if ((devname = xml_find_body(xe, "name")) != NULL &&
        (dh = device_handle_find(h, devname)) != NULL){
        if ((xc = device_handle_sync_xml_get(dh)) != NULL){
            if (clixon_xml2cbuf(cbret, xc, 0, 0, -1, 0) < 0)
                goto done;
        }
    }
    cprintf(cbret, "</config>");
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    return retval;
}

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
    int            retval = -1;
    cxobj        **vec = NULL;
    size_t         veclen;
    cxobj         *xret = NULL;
    int            i;
    cxobj         *xn;
    char          *name;
    device_handle  dh;
    cbuf          *cb = NULL;
    conn_state_t   state;
    char          *logmsg;
    struct timeval tv;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, name)) == NULL)
            continue;
        cprintf(cb, "<devices xmlns=\"%s\"><device><name>%s</name>",
                CONTROLLER_NAMESPACE,
                name);
        state = device_handle_conn_state_get(dh);
        cprintf(cb, "<conn-state>%s</conn-state>", device_state_int2str(state));
#ifdef NOTYET // something with encoding
        {
            cxobj *xcaps;
            cxobj *x;

            if ((xcaps = device_handle_capabilities_get(dh)) != NULL){
                cprintf(cb, "<capabilities>");
                x = NULL;
                while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
                    if (xml_body(x) == NULL)
                        continue;
                    // XXX need encoding?
                    cprintf(cb, "<capability>%s</capability>", xml_body(x));
                }
                cprintf(cb, "</capabilities>");
            }
        }
#endif
        device_handle_conn_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<conn-state-timestamp>%s</conn-state-timestamp>", timestr);
        }
        device_handle_sync_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<sync-timestamp>%s</sync-timestamp>", timestr);
        }
        if ((logmsg = device_handle_logmsg_get(dh)) != NULL)
            cprintf(cb, "<logmsg>%s</logmsg>", logmsg);
        cprintf(cb, "</device></devices>");
        if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xstate, NULL) < 0)
            goto done;
        cbuf_reset(cb);
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Connect to device 
 *
 * Typically called from commit
 * @param[in] h   Clixon handle
 * @param[in] xn  XML of device config
 */
static int
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

/*! Commit generic part of controller yang
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
    int     retval = -1;
    cxobj **vec1 = NULL;
    cxobj **vec2 = NULL;
    cxobj **vec3 = NULL;
    size_t  veclen1;
    size_t  veclen2;
    size_t  veclen3;
    int     i;
    char   *body;
    //    device_handle dh;
    
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
            else
                if (controller_connect(h, xml_parent(vec2[i])) < 0)
                    goto done;
        }
    }
    /* 3) if device added, connect */
    if (xpath_vec_flag(target, nsc, "devices/device",
                       XML_FLAG_ADD,
                       &vec3, &veclen3) < 0)
        goto done;
    for (i=0; i<veclen3; i++){
        if (controller_connect(h, vec3[i]) < 0)
            goto done;
    }
    retval = 0;
 done:
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
        clicon_option_int_set(h, "controller_device_timeout", d);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}
                  
/*!
 */
static int
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
    if (stream_notify(h, "controller", "%s", cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Commit services part: detect any change then trigger a services-commit notification
 *
 * @param[in] h      Clixon handle
 * @param[in] nsc    Namespace context
 * @param[in] src  pre-existing xml tree
 * @param[in] target Post target xml tree
 * @retval   -1      Error
 * @retval    0      OK
 */
static int
controller_commit_services(clixon_handle h,
                           cvec         *nsc,
                           cxobj        *src,
                           cxobj        *target)
{
    int     retval = -1;
    cxobj **vec1 = NULL;
    cxobj **vec2 = NULL;
    cxobj **vec3 = NULL;
    size_t  veclen1;
    size_t  veclen2;
    size_t  veclen3;
    
    /* 1) check deleted
     */
    if (xpath_vec_flag(src, nsc, "services",
                       XML_FLAG_DEL,
                       &vec1, &veclen1) < 0)
        goto done;
    /* 2) Check changed
     */
    if (xpath_vec_flag(target, nsc, "services",
                       XML_FLAG_CHANGE,
                       &vec2, &veclen2) < 0)
        goto done;
    /* 3) Check added
     */
    if (xpath_vec_flag(target, nsc, "services",
                       XML_FLAG_ADD,
                       &vec3, &veclen3) < 0)
        goto done;
    if (veclen1 || veclen2 || veclen3)
        services_commit_notify(h);
    retval = 0;
 done:
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (vec3)
        free(vec3);
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
    if (controller_commit_services(h, nsc, src, target) < 0)
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
    cxobj        *xy1;

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

    if (ymod == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "ymod is NULL");
        goto done;
    }
    modname = yang_argument_get(ymod);
    if (strncmp(modname, "junos-rpc", strlen("junos-rpc")) == 0){
        if (yang_find(ymod, Y_GROUPING, "command-forwarding") == NULL){
            if ((ygr = ys_new(Y_GROUPING)) == NULL)
                goto done;
            if (yang_argument_set(ygr, "command-forwarding") < 0)
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
    /* Register callback for rpc calls 
     */
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
    if (rpc_callback_register(h, rpc_get_device_sync_config,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "get-device-sync-config"
                              ) < 0)
        goto done;
    if (stream_add(h, "controller", "Clixon controller event stream", 0, NULL) < 0)
        goto done;
    return &api;
 done:
    return NULL;
}
