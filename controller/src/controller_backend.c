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

#define CONTROLLER_NAMESPACE "urn:example:clixon-controller"

static int
netconf_input_cb(int   s,
                 void *arg)
{
    int retval = -1;
    
    clicon_debug(1, "%s", __FUNCTION__);
    retval = 0;
    // done:
    //    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

static int
connect_netconf_ssh(clicon_handle h,
                    cxobj        *xn,
                    char         *user,
                    char         *addr)
{
    int                  retval = -1;
    clixon_client_handle ch = NULL; /* clixon client handle */
    cbuf                *cb;
    int                  s;

    if (xn == NULL || addr == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xn or addr is NULL");
        return -1;
    }
    // XXX check if already connected?
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        return -1;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    if (1)
        clicon_debug(1, "%s %s", __FUNCTION__, cbuf_get(cb));
    else {
        if ((ch = clixon_client_connect(h, CLIXON_CLIENT_SSH, cbuf_get(cb))) == NULL)
            goto done;
        s = clixon_client_socket_get(ch);    
        /* XXX Global */
        clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
        //    clicon_option_int_set(h, "controller-state", CS_INIT);
        if (clixon_event_reg_fd(s, netconf_input_cb, h, "netconf socket") < 0)
            goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}
                
/*! Connect to one or several devices.
 */
static int 
connect_rpc(clicon_handle h,            /* Clicon handle */
            cxobj        *xe,           /* Request: <rpc><xn></rpc> */
            cbuf         *cbret,        /* Reply eg <rpc-reply>... */
            void         *arg,          /* client_entry */
            void         *regarg)       /* Argument given at register */
{
    int     retval = -1;
    cxobj  *xret = NULL;
    cxobj  *xn;
    cvec   *nsc = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;
    char   *pattern = NULL;
    char   *name;
    char   *type;
    char   *addr;
    
    clicon_debug(1, "%s", __FUNCTION__);
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    pattern = xml_find_body(xe, "name");
    if (xmldb_get(h, "running", nsc, "nodes", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "nodes/node", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        /* Only handle netconf/ssh */
        if ((type = xml_find_body(xn, "type")) == NULL ||
            strcmp(type, "NETCONF_SSH"))
            continue;
        if ((addr = xml_find_body(xn, "addr")) == NULL)
            continue;
        if (pattern==NULL || fnmatch(pattern, name, 0) == 0){
            cprintf(cbret, "<name xmlns=\"%s\">%s</name>",  CONTROLLER_NAMESPACE, name);
            if (connect_netconf_ssh(h, xn,
                                    xml_find_body(xn, "user"),
                                    addr) < 0) /* match */
                goto done;
        }
    }
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
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
    int     retval = -1;
    cxobj  *xret = NULL;
    cxobj  *xn;
    cvec   *nsc = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;
    char   *pattern = NULL;
    char   *name;
    char   *type;
    char   *addr;
    
    clicon_debug(1, "%s", __FUNCTION__);
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    pattern = xml_find_body(xe, "name");
    if (xmldb_get(h, "running", nsc, "nodes", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "nodes/node", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        /* Only handle netconf/ssh */
        if ((type = xml_find_body(xn, "type")) == NULL ||
            strcmp(type, "NETCONF_SSH"))
            continue;
        if ((addr = xml_find_body(xn, "addr")) == NULL)
            continue;
        if (pattern==NULL || fnmatch(pattern, name, 0) == 0){
            cprintf(cbret, "<name xmlns=\"%s\">%s</name>", CONTROLLER_NAMESPACE, name);
        }
    }
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

int controller_commit(clicon_handle h, transaction_data td) {
    clicon_debug(1, "controller commit");
    return 0;
}

/* Forward declaration */
clixon_plugin_api *clixon_plugin_init(clicon_handle h);

static clixon_plugin_api api = {
    "wifi backend",
    clixon_plugin_init,
    .ca_trans_commit = controller_commit,
};

clixon_plugin_api *
clixon_plugin_init(clicon_handle h)
{
    /* Register callback for routing rpc calls 
     */
    if (rpc_callback_register(h, connect_rpc, 
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "connect"
                              ) < 0)
        goto done;
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
