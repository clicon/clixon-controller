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

/* Local client lib */
#include "clixon_client2.h"

#define CONTROLLER_NAMESPACE "urn:example:clixon-controller"

static int netconf_input_cb(int s, void *arg);
    

/*! Receive hello message
 * XXX maube move semantics to netconf_input_cb
 */
static int
netconf_hello_msg(clixon_client_handle ch,
                  cxobj               *xn)
{
    int     retval = -1;
    cxobj  *xcaps = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((xcaps = xpath_first(xn, NULL, "/hello/capabilities")) == NULL){
        clicon_err(OE_PROTO, ESHUTDOWN, "No capabilities found");
        goto done;
    }
    if (xml_rm(xcaps) < 0)
        goto done;
    if (clixon_client2_capabilities_set(ch, xcaps) < 0)
        goto done;
   retval = 0;
 done:
    return retval;
}

/*! Process incoming frame, ie a char message framed by ]]>]]>
 * Parse string to xml, check only one netconf message within a frame
 * @param[in]   h     Connection handle
 * @param[in]   cb    Packet buffer
 * @param[in]   yspec Yang spec
 * @param[out]  xreq  XML packet
 * @param[out]  eof   Set to 1 if pending close socket
 * @retval      0     OK
 * @retval     -1     Fatal error
 */
static int
netconf_input_frame(clixon_client_handle ch,
                    cbuf                *cb,
                    yang_stmt           *yspec,
                    cxobj              **xrecv)
{
    int                  retval = -1;
    char                *str = NULL;
    cxobj               *xtop = NULL; /* Request (in) */
    cxobj               *xerr = NULL;
    int                  ret;

    clicon_debug(1, "%s", __FUNCTION__);
    if (xrecv == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xrecv is NULL");
        return -1;
    }
    //    clicon_debug(2, "%s: \"%s\"", __FUNCTION__, cbuf_get(cb));
    if ((str = strdup(cbuf_get(cb))) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }    
    if (strlen(str) != 0){
        if ((ret = clixon_xml_parse_string(str, YB_RPC, yspec, &xtop, &xerr)) < 0)
            clicon_log(LOG_WARNING, "%s: read: %s", __FUNCTION__, strerror(errno)); /* Parse error: log */
#if 0 // XXX only after schema mount and get-schema stuff
        else if (ret == 0)
            clicon_log(LOG_WARNING, "%s: YANG error", __FUNCTION__);
#endif
        else if (xml_child_nr_type(xtop, CX_ELMNT) == 0)
            clicon_log(LOG_WARNING, "%s: empty frame", __FUNCTION__);
        else if (xml_child_nr_type(xtop, CX_ELMNT) != 1)
            clicon_log(LOG_WARNING, "%s: multiple message in single frames", __FUNCTION__);
        else
            *xrecv = xtop;
    }
    retval = 0;
 done:
    if (str)
        free(str);
    return retval;
}

/*! Get netconf message: detect end-of-msg XXX could be moved to clixon_netconf_lib.c
 *
 * @param[in]     ch          Clixon client handle.
 * @param[in]     s           Socket where input arrived. read from this.
 * @param[in,out] frame_state 
 * @param[in,out] frame_size 
 * @param[in,out] cb          Buffer
 * @param[out]    eom         If frame found in cb?
 * @param[out]    eof         socket closed / eof?
 * @retval        0           OK
 * @retval       -1           Error
 * This routine continuously reads until no more data on s. There could
 * be risk of starvation, but the netconf client does little else than
 * read data so I do not see a danger of true starvation here.
 * @note data is saved in clicon-handle at NETCONF_HASH_BUF since there is a potential issue if data
 * is not completely present on the s, ie if eg:
 *   <a>foo ..pause.. </a>]]>]]>
 * then only "</a>" would be delivered to netconf_input_frame().
 */
static int
netconf_input_msg(clixon_client_handle ch,
                  int                  s,
                  int                  framing,
                  int                 *frame_state,
                  size_t              *frame_size,
                  cbuf                *cb,
                  int                 *eom,
                  int                 *eof)
{
    int            retval = -1;
    unsigned char  buf[BUFSIZ]; /* from stdio.h, typically 8K */
    int            i;
    ssize_t        len;
    int            ret;
    int            found = 0;

    clicon_debug(1, "%s", __FUNCTION__);
    memset(buf, 0, sizeof(buf));
    while (1){
        clicon_debug(1, "%s read()", __FUNCTION__);
        if ((len = read(s, buf, sizeof(buf))) < 0){
            if (errno == ECONNRESET)
                len = 0; /* emulate EOF */
            else{
                clicon_log(LOG_ERR, "%s: read: %s", __FUNCTION__, strerror(errno));
                goto done;
            }
        } /* read */
        clicon_debug(1, "%s len:%ld", __FUNCTION__, len);
        if (len == 0){  /* EOF */
            clicon_debug(1, "%s len==0, closing", __FUNCTION__);
            *eof = 1;
        }
        else
            for (i=0; i<len; i++){
                if (buf[i] == 0)
                    continue; /* Skip NULL chars (eg from terminals) */
                if (framing == NETCONF_SSH_CHUNKED){
                    /* Track chunked framing defined in RFC6242 */
                    if ((ret = netconf_input_chunked_framing(buf[i], frame_state, frame_size)) < 0)
                        goto done;
                    switch (ret){
                    case 1: /* chunk-data */
                        cprintf(cb, "%c", buf[i]);
                        break;
                    case 2: /* end-of-data */
                        /* Somewhat complex error-handling:
                         * Ignore packet errors, UNLESS an explicit termination request (eof)
                         */
                        found++;
                        break;
                    default:
                        break;
                    }
                }
                else{
                    cprintf(cb, "%c", buf[i]);
                    if (detect_endtag("]]>]]>", buf[i], frame_state)){
                        *frame_state = 0;
                        /* OK, we have an xml string from a client */
                        /* Remove trailer */
                        *(((char*)cbuf_get(cb)) + cbuf_len(cb) - strlen("]]>]]>")) = '\0';
                        found++;
                        break;
                    }
                }
            }
        break;
    } /* while */
    *eom = found;
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Close connection, unregister events, free mem
 */
static int
close_connection(clixon_client_handle ch)
{
    int s;
    
    s = clixon_client2_socket_get(ch);
    clixon_event_unreg_fd(s, netconf_input_cb); /* deregister events */
    clixon_client2_disconnect(ch);              /* close socket, reap sub-processes */
    clixon_client2_free(ch);                    /* free mem */
    return 0;
}

/*! Controller input connected to open state handling
 */
static int
netconf_connected_open(clixon_client_handle ch,
                       int                  s,
                       cxobj               *xmsg,
                       char                *rpcname,
                       conn_state_t         conn_state)
{
    int           retval = -1;
    clicon_handle h;
    char         *rpcprefix;
    char         *namespace = NULL;
    int           version;

    h = clixon_client2_handle_get(ch);
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "hello") != 0){
        clicon_log(LOG_WARNING, "%s: Unexpected msg %s in state %s",
                   __FUNCTION__, rpcname, controller_state_int2str(conn_state));
        close_connection(ch);
        goto ok;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        clicon_log(LOG_WARNING, "No appropriate namespace associated with namespace:%s",
                   namespace);
        close_connection(ch);
        goto ok;
    }
    if (netconf_hello_msg(ch, xmsg) < 0)
        goto done;
    if (clixon_client2_capabilities_find(ch, "urn:ietf:params:netconf:base:1.1"))
        version = 1;
    else if (clixon_client2_capabilities_find(ch, "urn:ietf:params:netconf:base:1.0"))
        version = 0;
    else{
        clicon_err(OE_PROTO, ESHUTDOWN, "Nobase netconf capability found");
        goto done;
    }
    clicon_debug(1, "%s version: %d", __FUNCTION__, version);
    version = 0; /* XXX hardcoded to 0 */
    clicon_option_int_set(h, "netconf-framing", version);
    if (clixon_client_hello(s, version) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Controller input wresp to open state handling
 */
static int
netconf_wresp_open(clixon_client_handle ch,
                   cxobj               *xmsg,
                   yang_stmt           *yspec,
                   char                *rpcname,
                   conn_state_t         conn_state)
{
    int           retval = -1;
    clicon_handle h;
    char         *rpcprefix;
    char         *namespace = NULL;
    cxobj        *xdata;
    cxobj        *x1 = NULL;
    cxobj        *xc;
    cxobj        *xa;
    cbuf         *cb = NULL;
    cbuf         *cbret = NULL;
    char         *name;
    int           ret;

    name = clixon_client2_name_get(ch);
    h = clixon_client2_handle_get(ch);
    xdata = xpath_first(xmsg, NULL, "data");
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "rpc-reply") != 0){
        clicon_log(LOG_WARNING, "%s: Unexpected msg %s in state %s",
                   __FUNCTION__, rpcname, controller_state_int2str(conn_state));
        close_connection(ch);
        goto ok;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        clicon_log(LOG_WARNING, "No appropriate namespace associated with namespace:%s",
                   namespace);
        close_connection(ch);
        goto ok;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<nodes xmlns=\"%s\" xmlns:nc=\"%s\"><node><name>%s</name>",
            CONTROLLER_NAMESPACE,
            NETCONF_BASE_NAMESPACE, /* needed for nc:op below */
            name);
    cprintf(cb, "</node></nodes>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x1, NULL)) < 0)
        goto done;
    if (xml_name_set(x1, "config") < 0)
        goto done;
    if ((xc = xpath_first(x1, NULL, "nodes/node")) != NULL){
        yang_stmt *y;
        if (xml_addsub(xc, xdata) < 0)
            goto done;
        if ((y = yang_find(xml_spec(xc), Y_ANYDATA, "data")) == NULL){
            clicon_err(OE_YANG, 0, "Not finding proper yang child of node(shouldnt happen)");
            goto done;
        }
        xml_spec_set(xdata, y);
        xml_print(stderr, x1); // XXX
        if ((cbret = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        /* Add op=OP_REPLACE to xdata */
        if ((xa = xml_new("operation", xdata, CX_ATTR)) == NULL)
            goto done;
        if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
            goto done;
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
            goto done;
        if (xmldb_put(h, "candidate", OP_NONE, x1, NULL, cbret) < 0)
            goto done;
        cbuf_free(cbret);
    }
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*!
 */
static int
netconf_input_cb(int   s,
                 void *arg)
{
    clixon_client_handle ch = (clixon_client_handle)arg;
    clicon_handle        h;
    int                  retval = -1;
    int                  eom = 0;
    int                  eof = 0;
    int                  frame_state;
    size_t               frame_size;
    cbuf                *cb;
    yang_stmt           *yspec;
    cxobj               *xtop = NULL;
    cxobj               *xmsg;
    conn_state_t         conn_state;
    char                *rpcname;

    clicon_debug(1, "%s", __FUNCTION__);
    h = clixon_client2_handle_get(ch);
    frame_state = clixon_client2_frame_state_get(ch);
    frame_size = clixon_client2_frame_size_get(ch);
    cb = clixon_client2_frame_buf_get(ch);
    /* Read data, if eom set a frame is read
     */
    if (netconf_input_msg(ch, s,
                          clicon_option_int(h, "netconf-framing"),
                          &frame_state, &frame_size,
                          cb, &eom, &eof) < 0)
        goto done;
    clicon_debug(1, "%s eom:%d eof:%d", __FUNCTION__, eom, eof);
    if (eof){         /* Close connection, unregister events, free mem */
        close_connection(ch);
        goto ok;
    }
    clixon_client2_frame_state_set(ch, frame_state);
    clixon_client2_frame_size_set(ch, frame_size);
    if (eom == 0) /* frame not found */
        goto ok;
    yspec = clicon_dbspec_yang(h);
    if (netconf_input_frame(ch, cb, yspec, &xtop) < 0)
        goto done;
    cbuf_reset(cb);
    xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
    rpcname = xml_name(xmsg);
    conn_state = clixon_client2_conn_state_get(ch);
    switch (conn_state){
    case CS_CONNECTED:
        if (netconf_connected_open(ch, s, xmsg, rpcname, conn_state) < 0)
            goto done;
        clixon_client2_conn_state_set(ch, CS_OPEN);
        break;
    case CS_WRESP:
        if (netconf_wresp_open(ch, xmsg, yspec, rpcname, conn_state) < 0)
            goto done;
        clixon_client2_conn_state_set(ch, CS_OPEN);
        break;
    case CS_CLOSED:
    case CS_OPEN:
    default:
        clicon_log(LOG_WARNING, "%s: Unexpected msg %s in state %s",
                   __FUNCTION__, rpcname, controller_state_int2str(conn_state));
        close_connection(ch);
        break;
    }
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xtop)
        xml_free(xtop);
    return retval;
}

static int
connect_netconf_ssh(clicon_handle h,
                    cxobj        *xn,
                    char         *name,
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
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        return -1;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    // XXX check state if already connected?
    if ((ch = clixon_client2_new(h, name)) == NULL)
        goto done;
    if (clixon_client2_connect(ch, CLIXON_CLIENT_SSH, cbuf_get(cb)) < 0)
        goto done;
    clixon_client2_conn_state_set(ch, CS_CONNECTED);
    s = clixon_client2_socket_get(ch);    
    clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
    if (clixon_event_reg_fd(s, netconf_input_cb, ch, "netconf socket") < 0)
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
    char   *statestr;
    clixon_client_handle ch;
    conn_state_t state0;
    
    clicon_debug(1, "%s", __FUNCTION__);
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    pattern = xml_find_body(xe, "name");
    statestr = xml_find_body(xe, "state");
    if (xmldb_get(h, "running", nsc, "nodes", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "nodes/node", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        ch = clixon_client2_find(h, name); /* can be NULL */
        if (strcmp(statestr, "true") == 0){ /* open */
            if (ch != NULL &&
                (state0 = clixon_client2_conn_state_get(ch)) != CS_CLOSED)
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
                                        name,
                                        xml_find_body(xn, "user"),
                                        addr) < 0) /* match */
                    goto done;
            }
        }
        else if (ch != NULL &&
                 (state0 = clixon_client2_conn_state_get(ch)) >= CS_OPEN){
            cprintf(cbret, "<name xmlns=\"%s\">%s</name>",  CONTROLLER_NAMESPACE, name);
            close_connection(ch);
        }
    } /* for */
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
    int                  encap;
    int                  s;
    
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
        if ((ch = clixon_client2_find(h, name)) == NULL)
            continue;
        if ((state = clixon_client2_conn_state_get(ch)) != CS_OPEN)
            continue;
        if (pattern==NULL || fnmatch(pattern, name, 0) == 0){
            cprintf(cbret, "<name xmlns=\"%s\">%s</name>",  CONTROLLER_NAMESPACE, name);
            if ((cb = cbuf_new()) == NULL){
                clicon_err(OE_PLUGIN, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
            cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR);
            cprintf(cb, "><get-config><source><running/></source>");
            cprintf(cb, "</get-config></rpc>");
            encap = clicon_option_int(h, "netconf-framing");
            if (netconf_output_encap(encap, cb) < 0)
                goto done;
            s = clixon_client2_socket_get(ch);
            if (clicon_msg_send1(s, cb) < 0)
                goto done;
            cbuf_free(cb);
            cb = NULL;
            clixon_client2_conn_state_set(ch, CS_WRESP);
        }
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
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xmldb_get(h, "running", nsc, "nodes", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "nodes/node", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((ch = clixon_client2_find(h, name)) == NULL)
            continue;
        cprintf(cb, "<nodes xmlns=\"%s\"><node><name>%s</name>",
                CONTROLLER_NAMESPACE,
                name);
        state = clixon_client2_conn_state_get(ch);
        cprintf(cb, "<conn-state>%s</conn-state>", controller_state_int2str(state));
        cprintf(cb, "</node></nodes>");
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

/*! Transaction commit
 */
int
controller_commit(clicon_handle    h,
                  transaction_data td)
{
    clicon_debug(1, "controller commit");
    return 0;
}

/* Forward declaration */
clixon_plugin_api *clixon_plugin_init(clicon_handle h);

static clixon_plugin_api api = {
    "wifi backend",
    clixon_plugin_init,
    .ca_statedata    = controller_statedata,
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
