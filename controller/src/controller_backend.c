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
#include "clixon_client2.h"

#ifdef XXX
/* 
 * Forward declarations
 */
static int device_input_cb(int s, void *arg);
#endif

/*! Connect to device via Netconf SSH
 * @param[in]  h  Clixon handle
 * @param[in]  ch Clixon client handle, either NULL or in closed state
 */
static int
connect_netconf_ssh(clicon_handle h,
                    clixon_client_handle ch,
                    cxobj        *xn,
                    char         *name,
                    char         *user,
                    char         *addr)
{
    int                  retval = -1;
    cbuf                *cb;
    int                  s;

    if (xn == NULL || addr == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xn or addr is NULL");
        return -1;
    }
    if (ch != NULL && clixon_client2_conn_state_get(ch) != CS_CLOSED){
        clicon_err(OE_PLUGIN, EINVAL, "ch is not closed");
        return -1;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        return -1;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    if (ch == NULL &&
        (ch = clixon_client2_new(h, name)) == NULL)
        goto done;
    if (clixon_client2_connect(ch, CLIXON_CLIENT_SSH, cbuf_get(cb)) < 0)
        goto done;
    device_state_timeout_register(ch);
    clixon_client2_conn_state_set(ch, CS_CONNECTING);
    s = clixon_client2_socket_get(ch);    
    clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
    if (clixon_event_reg_fd(s, device_input_cb, ch, "netconf socket") < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Connect to one or several devices.
 */
static int 
sync_rpc(clicon_handle h,            /* Clicon handle */
         cxobj        *xe,           /* Request: <rpc><xn></rpc> */
         cbuf         *cbret,        /* Reply eg <rpc-reply>... */
         void         *arg,          /* client_entry */
         void         *regarg)       /* Argument given at register */
{
    int                  retval = -1;
    cxobj               *xret = NULL;
    cxobj               *xn;
    cvec                *nsc = NULL;
    cxobj              **vec = NULL;
    size_t               veclen;
    int                  i;
    char                *pattern = NULL;
    char                *name;
    clixon_client_handle ch;
    conn_state_t         state;
    cbuf                *cb = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    pattern = xml_find_body(xe, "name");
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((ch = clixon_client2_find(h, name)) == NULL)
            continue;
        if ((state = clixon_client2_conn_state_get(ch)) != CS_OPEN)
            continue;
        if (pattern != NULL && fnmatch(pattern, name, 0) != 0)
            continue;
        cprintf(cbret, "<name xmlns=\"%s\">%s</name>",  CONTROLLER_NAMESPACE, name);
        if (device_sync(h, ch) < 0)
            goto done;
        device_state_timeout_register(ch);
        clixon_client2_conn_state_set(ch, CS_DEVICE_SYNC);
    } /* for */
    cprintf(cbret, "</rpc-reply>");
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
controller_statedata(clicon_handle   h, 
                     cvec           *nsc,
                     char           *xpath,
                     cxobj          *xstate)
{
    int                  retval = -1;
    cxobj              **vec = NULL;
    size_t               veclen;
    cxobj               *xret = NULL;
    int                  i;
    cxobj               *xn;
    char                *name;
    clixon_client_handle ch;
    cbuf                *cb = NULL;
    conn_state_t         state;
    char                *logmsg;
    struct timeval       tv;
    
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
        if ((ch = clixon_client2_find(h, name)) == NULL)
            continue;
        cprintf(cb, "<devices xmlns=\"%s\"><device><name>%s</name>",
                CONTROLLER_NAMESPACE,
                name);
        state = clixon_client2_conn_state_get(ch);
        cprintf(cb, "<conn-state>%s</conn-state>", controller_state_int2str(state));
#ifdef NOTYET // something with encoding
        {
            cxobj *xcaps;
            cxobj *x;

            if ((xcaps = clixon_client2_capabilities_get(ch)) != NULL){
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
        clixon_client2_conn_time_get(ch, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<conn-state-timestamp>%s</conn-state-timestamp>", timestr);
        }
        clixon_client2_sync_time_get(ch, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<sync-timestamp>%s</sync-timestamp>", timestr);
        }
        if ((logmsg = clixon_client2_logmsg_get(ch)) != NULL)
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
 * via commit
 */
static int
controller_connect(clicon_handle h,
                   cxobj        *xn)
{
    int                  retval = -1;
    char                *name;
    clixon_client_handle ch;
    cbuf                *cb = NULL;
    char                *type;
    char                *addr;
    char                *enable;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((name = xml_find_body(xn, "name")) == NULL)
        goto ok;
    if ((enable = xml_find_body(xn, "enable")) == NULL){
        goto ok;
    }
    if (strcmp(enable, "true") != 0){
        if ((ch = clixon_client2_new(h, name)) == NULL)
            goto done;
        clixon_client2_logmsg_set(ch, strdup("Configured down"));
        goto ok;
    }
    ch = clixon_client2_find(h, name); /* can be NULL */
    if (ch != NULL &&
        clixon_client2_conn_state_get(ch) != CS_CLOSED)
        goto ok;
    /* Only handle netconf/ssh */
    if ((type = xml_find_body(xn, "type")) == NULL ||
        strcmp(type, "NETCONF_SSH"))
        goto ok;
    if ((addr = xml_find_body(xn, "addr")) == NULL)
        goto ok;
    if (connect_netconf_ssh(h, ch, xn,
                            name,
                            xml_find_body(xn, "user"),
                            addr) < 0) /* match */
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

static int
controller_disconnect(clicon_handle h,
                      cxobj        *xn)
{
    char                *name;
    clixon_client_handle ch;
    
    if ((name = xml_find_body(xn, "name")) != NULL &&
        (ch = clixon_client2_find(h, name)) != NULL)
        device_close_connection(ch, NULL); /* Regular disconnect, no reason */
    return 0;
}

/*! Commit generic part of controller yang
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
controller_commit_device(clicon_handle h,
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
    if (xpath_vec_flag(target, nsc, "devices/device/enable",
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
controller_commit_generic(clicon_handle h,
                          cvec         *nsc,
                          cxobj        *target)
{
    int     retval = -1;
    char   *body;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;
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
                  
/*! Transaction commit
 */
int
controller_commit(clicon_handle    h,
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

/* Called just before plugin unloaded. 
 * @param[in] h    Clixon handle
 */
static int
controller_exit(clicon_handle h)
{
    clixon_client2_free_all(h);
    return 0;
}

/* Forward declaration */
clixon_plugin_api *clixon_plugin_init(clicon_handle h);

static clixon_plugin_api api = {
    "wifi backend",
    .ca_exit         = controller_exit,
    .ca_statedata    = controller_statedata,
    .ca_trans_commit = controller_commit,
};

clixon_plugin_api *
clixon_plugin_init(clicon_handle h)
{
    /* Register callback for rpc calls 
     */
    if (rpc_callback_register(h, sync_rpc, 
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "sync"
                              ) < 0)
        goto done;
    return &api;
 done:
    return NULL;
}
