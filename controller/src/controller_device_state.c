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
  State machine:

  CS_CLOSED
     ^     \ connect  
     |      v        send get
     |    CS_CONNECTING
     |       |
     |       v
     |    CS_DEVICE_SYNC
     |     /            \
     |    v              v
  CS_OPEN  <------------  CS_SCHEMA(n)

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
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "clixon_client2.h"

/*! Mapping between enum conn_state and yang connection-state
 * @see clixon-controller@2023-01-01.yang for mirror enum and descriptions
 * @see enum conn_state for basic type
 */
static const map_str2int csmap[] = {
    {"CLOSED",       CS_CLOSED},
    {"CONNECTING",   CS_CONNECTING},
    {"DEVICE-SYNC",  CS_DEVICE_SYNC},
    {"SCHEMA_LIST",  CS_SCHEMA_LIST},
    {"SCHEMA_ONE",   CS_SCHEMA_ONE},
    {"OPEN",         CS_OPEN},
    {"WRESP",        CS_WRESP},
    {NULL,           -1}
};

/*! Map connection state from int to string */
char *
controller_state_int2str(conn_state_t state)
{
    return (char*)clicon_int2str(csmap, state);
}

/*! Map connection state from string to int */
conn_state_t
controller_state_str2int(char *str)
{
    return clicon_str2int(csmap, str);
}

/*! Close connection, unregister events and timers
 * @param[in]  ch      Clixon client handle.
 * @param[in]  format  Format string for Log message
 * @retval     0       OK
 * @retval    -1       Error
 */
int
device_close_connection(clixon_client_handle ch,
                        const char *format, ...)
{
    int            retval = -1;

    va_list        ap;
    size_t         len;
    char          *str = NULL;
    int            s;
    
    s = clixon_client2_socket_get(ch);
    clixon_event_unreg_fd(s, device_input_cb); /* deregister events */
    device_state_timeout_unregister(ch);
    clixon_client2_disconnect(ch);              /* close socket, reap sub-processes */
    clixon_client2_conn_state_set(ch, CS_CLOSED);
    if (format == NULL)
        clixon_client2_logmsg_set(ch, NULL);
    else {
        va_start(ap, format); /* dryrun */
        if ((len = vsnprintf(NULL, 0, format, ap)) < 0) /* dryrun, just get len */
            goto done;
        va_end(ap);
        if ((str = malloc(len)) == NULL){
            clicon_err(OE_UNIX, errno, "malloc");
            goto done;
        }
        va_start(ap, format); /* real */
        if (vsnprintf(str, len+1, format, ap) < 0){
            clicon_err(OE_UNIX, errno, "vsnprintf");
            goto done;
        }
        va_end(ap);
        clixon_client2_logmsg_set(ch, str);
    }
    retval = 0;
 done:
    return retval;
}

/*! Handle input data from device, whole or part of a frame ,called by event loop
 * @param[in] s   Socket
 * @param[in] arg Client handle
 */
int
device_input_cb(int   s,
                void *arg)
{
    clixon_client_handle ch = (clixon_client_handle)arg;
    clicon_handle        h;
    int                  retval = -1;
    int                  eom = 0;
    int                  eof = 0;
    int                  frame_state; /* only used for chunked framing not eom */
    size_t               frame_size;
    cbuf                *cb;
    yang_stmt           *yspec;
    cxobj               *xtop = NULL;
    cxobj               *xmsg;
    int                  ret;

    clicon_debug(1, "%s", __FUNCTION__);
    h = clixon_client2_handle_get(ch);
    frame_state = clixon_client2_frame_state_get(ch);
    frame_size = clixon_client2_frame_size_get(ch);
    cb = clixon_client2_frame_buf_get(ch);
    /* Read data, if eom set a frame is read
     */
    if (netconf_input_msg(s,
                          clicon_option_int(h, "netconf-framing"),
                          &frame_state, &frame_size,
                          cb, &eom, &eof) < 0)
        goto done;
    clicon_debug(1, "%s eom:%d eof:%d len:%lu", __FUNCTION__, eom, eof, cbuf_len(cb));
    if (eof){         /* Close connection, unregister events, free mem */
        device_close_connection(ch, "Remote socket endpoint closed");
        goto ok;
    }
    clixon_client2_frame_state_set(ch, frame_state);
    clixon_client2_frame_size_set(ch, frame_size);
    if (eom == 0) /* frame not found */
        goto ok;
    clicon_debug(1, "%s frame: %lu strlen:%lu", __FUNCTION__, cbuf_len(cb), strlen(cbuf_get(cb)));
    cbuf_trunc(cb, cbuf_len(cb));
#if 0 // XXX debug
    if (clicon_debug_get()){
        fprintf(stdout, "%s\n", cbuf_get(cb));
        fflush(stdout);
    }
#endif
    yspec = clicon_dbspec_yang(h);
    if ((ret = netconf_input_frame(cb, yspec, &xtop)) < 0)
        goto done;
    cbuf_reset(cb);
    if (ret==0){
        device_close_connection(ch, "Invalid frame");
        goto ok;
    }
    xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
    if (xmsg && device_state_handler(ch, h, s, xmsg) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Receive hello message
 * XXX maybe move semantics to netconf_input_cb
 */
static int
device_rcv_hello(clixon_client_handle ch,
                  cxobj               *xn,
                  cvec                *nsc)
{
    int     retval = -1;
    cxobj  *xcaps = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((xcaps = xpath_first(xn, nsc, "/hello/capabilities")) == NULL){
        clicon_err(OE_PROTO, ESHUTDOWN, "No capabilities found");
        goto done;
    }
    if (xml_rm(xcaps) < 0)
        goto done;
    if (clicon_debug_get()){
        xml_print(stdout, xcaps);
    }
    if (clixon_client2_capabilities_set(ch, xcaps) < 0)
        goto done;
   retval = 0;
 done:
    return retval;
}

/*! Send a <get> request to a device
 *
 * @param[in]  h   Clicon handle
 * @param[in]  ch  Clixon client handle
 */
int
device_sync(clicon_handle h,
            clixon_client_handle ch)
{
    int           retval = -1;
    cbuf         *cb = NULL;
    int           encap;
    int           s;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE, 
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get>");
    cprintf(cb, "</get>");
    cprintf(cb, "</rpc>");
    encap = clicon_option_int(h, "netconf-framing");
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    s = clixon_client2_socket_get(ch);
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send s single get-schema requests to a device
 *
 * @param[in]  h   Clixon handle
 * @param[in]  ch  Clixon client handle
 * @param[in]  xd  XML tree of netconf monitor schema entry
 * @retval     1   OK, sent a get-schema request
 * @retval     0   Nothing sent
 * @retval    -1   Error
 * @see ietf-netconf-monitoring@2010-10-04.yang
 */
static int
device_send_get_schema_one(clicon_handle        h,
                           clixon_client_handle ch,
                           cxobj               *xd)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    char  *identifier;
    char  *version;
    char  *format;
    cxobj *x;
    int    encap;
    int    s;

    if ((identifier = xml_find_body(xd, "identifier")) == NULL ||
        (version = xml_find_body(xd, "version")) == NULL ||
        (format = xml_find_body(xd, "format")) == NULL){
        clicon_err(OE_XML, EINVAL, "schema id/version/format missing");
        goto done;
    }
    /* Sanity checks, skip if not right */
    if (strcmp(format, "yang") != 0)
        goto skip;
    x = NULL;
    while ((x = xml_child_each(xd, x, CX_ELMNT)) != NULL) {
        if (strcmp("location", xml_name(x)) != 0)
            continue;
        if (xml_body(x) && strcmp("NETCONF", xml_body(x)) == 0)
            break;
    }
    if (x == NULL)
        goto skip;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE, 
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get-schema xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<identifier>%s</identifier>", identifier);
    cprintf(cb, "<version>%s</version>", version);
    cprintf(cb, "<format>%s</format>", format);
    cprintf(cb, "</get-schema>");
    cprintf(cb, "</rpc>");
    encap = clicon_option_int(h, "netconf-framing");
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    s = clixon_client2_socket_get(ch);
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 skip:
    retval = 0;
    goto done;
}
            
/*! Send next get-schema requests to a device
 *
 * @param[in]     h   Clixon handle
 * @param[in]     ch  Clixon client handle
 * @param[in,out] nr  Last schema index sent
 * @retval        1   Sent a get-schema, nr updated
 * @retval        0   All get-schema sent
 * @retval       -1   Error
 */
static int
device_send_get_schema_next(clicon_handle        h,
                            clixon_client_handle ch,
                            int                 *nr)
{
    int        retval = -1;
    cxobj     *xdevs = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    cvec      *nsc = NULL;
    cbuf      *cb = NULL;
    int        i;
    int        ret;

    clicon_debug(1, "%s %d", __FUNCTION__, *nr);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "devices/device[name=\"%s\"]/root/data/netconf-state/schemas/schema", clixon_client2_name_get(ch));
    /* Loop over data/netconf-state/schemas/schema and send get-schema for each */
    if (xmldb_get(h, "running", nsc, cbuf_get(cb), &xdevs) < 0)
        goto done;
    if (xpath_vec(xdevs, nsc, "%s", &vec, &veclen, cbuf_get(cb)) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        if (i != *nr)
            continue;
        if ((ret = device_send_get_schema_one(h, ch, vec[i])) < 0)
            goto done;
        (*nr)++;
        if (ret == 1)
            break;
    }
    if (i<veclen)
        retval = 1; // Sent a get-schema
    else
        retval = 0; // All get-schema sent
 done:
    if (cb)
        cbuf_free(cb);
    if (vec)
        free(vec);
    if (xdevs)
        xml_free(xdevs);
    return retval;
}

#ifdef NOTYET
/*! Send ietf-netconf-monitoring schema get request 
 * @note this could be part of the generic sync, but jumniper needs
 * an explicit step
 */
static int
device_send_get_monitor_schema(clicon_handle        h,
                               clixon_client_handle ch)

{
    int        retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    retval = 0;
    // done:
    return retval;
}
#endif

/*! Controller input: receive hello and send hello
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_connecting(clixon_client_handle ch,
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
    cvec         *nsc = NULL;

    h = clixon_client2_handle_get(ch);
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "hello") != 0){
        device_close_connection(ch, "Unexpected msg %s in state %s",
                   rpcname, controller_state_int2str(conn_state));
        goto closed;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(ch,  "No appropriate namespace associated with %s",
                   namespace);
        goto closed;
    }
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    if (device_rcv_hello(ch, xmsg, nsc) < 0)
        goto done;
    if (clixon_client2_capabilities_find(ch, "urn:ietf:params:netconf:base:1.1"))
        version = 1;
    else if (clixon_client2_capabilities_find(ch, "urn:ietf:params:netconf:base:1.0"))
        version = 0;
    else{
        device_close_connection(ch,  "No base netconf capability found");
        goto closed;
    }
    clicon_debug(1, "%s version: %d", __FUNCTION__, version);
    version = 0; /* XXX hardcoded to 0 */
    clicon_option_int_set(h, "netconf-framing", version);
    if (clixon_client_hello(s, version) < 0)
        goto done;
    retval = 1;
 done:
   if (nsc)
       cvec_free(nsc);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Controller input receive yang schema and parse it
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_get_schema(clixon_client_handle ch,
                        cxobj               *xmsg,
                        char                *rpcname,
                        conn_state_t         conn_state)
{
    int           retval = -1;
    char         *rpcprefix;
    cvec         *nsc = NULL;
    char         *namespace;
    char         *ystr;
    char         *ydec = NULL;
    char         *name;
    yang_stmt    *yspec;
    yang_stmt    *ymod;

    clicon_debug(1, "%s", __FUNCTION__);
    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
        goto closed;
    }
    name = clixon_client2_name_get(ch);
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(ch, "No appropriate namespace associated with:%s",
                   namespace);
        goto closed;
    }
    if ((yspec = clixon_client2_yspec_get(ch)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    if ((ystr = xml_find_body(xmsg, "data")) == NULL){
        device_close_connection(ch, "Invalid get-schema, no YANG body");
        goto closed;
    }
    if (xml_chardata_decode(&ydec, "%s", ystr) < 0)
        goto done;
    if ((ymod = yang_parse_str(ydec, name, yspec)) == NULL)
        goto done;
#ifdef CONTROLLER_DUMP_YANG_FILE
    { /* Dump to file */
        cbuf  *cb = cbuf_new();
        FILE  *f;
        size_t sz;
        
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, CONTROLLER_DUMP_YANG_FILE, name, yang_argument_get(ymod));
        clicon_debug(1, "%s: Dump yang to %s", __FUNCTION__, cbuf_get(cb));
        if ((f = fopen(cbuf_get(cb), "w")) == NULL){
            clicon_err(OE_UNIX, errno, "fopen(%s)", cbuf_get(cb));
            goto done;
        }
        sz = strlen(ydec);
        if (fwrite(ydec, 1, sz, f) != sz){
            clicon_err(OE_UNIX, errno, "fwrite");
            goto done;
        }
        fclose(f);
        cbuf_free(cb);
    }
#endif
    retval = 1;
 done:
    if (ydec)
        free(ydec);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! All schemas read from one device, make yang post-processing
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    0          OK
 * @retval   -1          Error
 */
static int
device_state_all_schemas(clicon_handle        h,
                         clixon_client_handle ch)
{
    int           retval = -1;
    yang_stmt    *yspec;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((yspec = clixon_client2_yspec_get(ch)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    if (yang_parse_post(h, yspec, 0) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Controller input read get rpc-reply commit config data from device
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_device_sync(clixon_client_handle ch,
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
    cvec         *nsc = NULL;
    cg_var       *cv;
    int           ret;

    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
        goto closed;
    }
    name = clixon_client2_name_get(ch);
    h = clixon_client2_handle_get(ch);
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(ch, "No appropriate namespace associated with:%s",
                   namespace);
        goto closed;
    }
    xdata = xpath_first(xmsg, nsc, "data");
    /* Move all namespace declarations to <data> */
    cv = NULL;
    while ((cv = cvec_each(nsc, cv)) != NULL){
        char *pf;
        char *ns;
        if ((pf = cv_name_get(cv)) == NULL)
            continue;
        ns = cv_string_get(cv);
        if (xmlns_set(xdata, pf, ns) < 0)
            goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<devices xmlns=\"%s\" xmlns:nc=\"%s\"><device><name>%s</name>",
            CONTROLLER_NAMESPACE,
            NETCONF_BASE_NAMESPACE, /* needed for nc:op below */
            name);
    cprintf(cb, "<root/>");
    cprintf(cb, "</device></devices>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x1, NULL)) < 0)
        goto done;
    if (xml_name_set(x1, "config") < 0)
        goto done;
    if ((xc = xpath_first(x1, NULL, "devices/device/root")) != NULL){ // XXX not same nsc
        yang_stmt *y;
        if (xml_addsub(xc, xdata) < 0)
            goto done;
        if (0){
        if ((y = yang_find(xml_spec(xc), Y_ANYDATA, "data")) == NULL){
            clicon_err(OE_YANG, 0, "Not finding proper yang child of device(shouldnt happen)");
            goto done;
        }
        xml_spec_set(xdata, y);
        }
#if 0 // XXX debug
        xml_print(stdout, x1);
        fflush(stdout);
#endif
        if ((cbret = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        /* Add op=OP_REPLACE to root mountpoint */
        if ((xa = xml_new("operation", xc, CX_ATTR)) == NULL)
            goto done;
        if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
            goto done;
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
            goto done;
        if (xmldb_put(h, "candidate", OP_NONE, x1, NULL, cbret) < 0)
            goto done;
        if ((ret = candidate_commit(h, NULL, "candidate", cb)) < 0)
            goto done;
        if (ret == 0){ /* discard */
            xmldb_copy(h, "running", "candidate");            
            xmldb_modified_set(h, "candidate", 0); /* reset dirty bit */
        }
        else {
            clixon_client2_sync_time_set(ch, NULL);
        }
    }
    retval = 1;
 done:
    if (cbret)
        cbuf_free(cbret);
    if (cb)
        cbuf_free(cb);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Controller input wresp to open state handling, read rpc-reply
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    0          OK
 * @retval   -1          Error
 * @note This is currently not in use, maybe obsolete?
 */
static int
device_state_wresp2open(clixon_client_handle ch,
                        cxobj               *xmsg,
                        yang_stmt           *yspec,
                        char                *rpcname,
                        conn_state_t         conn_state)
{
    int           retval = -1;

    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
    }
    /* Nothing yet */
    retval = 0;
    // done:
    return retval;
}

/*! Timeout callback of transient states, close connection
 * @param[in] arg    In effect client handle
 */
static int
device_state_timeout(int   s,
                     void *arg)
{
    clixon_client_handle ch = (clixon_client_handle)arg;

    device_close_connection(ch, "Timeout waiting for remote peer");
    return 0;
}

/*! Set timeout of transient device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_register(clixon_client_handle ch)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    uint32_t       d;
    clicon_handle  h;

    gettimeofday(&t, NULL);
    h = clixon_client2_handle_get(ch);
    d = clicon_option_int(h, "controller_device_timeout");
    if (d)
        t1.tv_sec = d;
    else
        t1.tv_sec = 60;
    t1.tv_usec = 0;
    timeradd(&t, &t1, &t);
    if (clixon_event_reg_timeout(t, device_state_timeout, ch,
                                 "Device state timeout") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Cancel timeout of transiet device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_unregister(clixon_client_handle ch)
{
    int retval = -1;

    if (clixon_event_unreg_timeout(device_state_timeout, ch) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
    
static int
device_state_timeout_restart(clixon_client_handle ch)
{
    int retval = -1;

    if (device_state_timeout_unregister(ch) < 0)
        goto done;
    if (device_state_timeout_register(ch) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

static int
junos_schemas(clixon_client_handle ch)
{
    fprintf(stdout, "%s", __FUNCTION__);
    return 0;
}
    
/*! Handle controller device state machine
 *
 * @param[in]  ch      Clixon client handle.
 * @param[in]  xmsg    XML tree of incoming message
 * @retval     0       OK
 * @retval    -1       Error
 */
int
device_state_handler(clixon_client_handle ch,
                     clicon_handle        h,
                     int                  s,
                     cxobj               *xmsg)
{
    int            retval = -1;
    char          *rpcname;
    conn_state_t   conn_state;
    yang_stmt     *yspec0;
    yang_stmt     *yspec1;
    struct timeval tsync;
    int            nr;
    int            ret;

    rpcname = xml_name(xmsg);
    conn_state = clixon_client2_conn_state_get(ch);
    yspec0 = clicon_dbspec_yang(h);
    switch (conn_state){
    case CS_CONNECTING:
        /* receive hello, send hello */
        if ((ret = device_state_connecting(ch, s, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        clixon_client2_sync_time_get(ch, &tsync);
        /* Unconditionally sync */
        if (device_sync(h, ch) < 0)
            goto done;
        clixon_client2_conn_state_set(ch, CS_DEVICE_SYNC);
        device_state_timeout_restart(ch);
        break;
    case CS_DEVICE_SYNC:
        /*! Read get rpc-reply commit config data */
        if ((ret = device_state_device_sync(ch, xmsg, yspec0, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        /* Reset YANGs */
        if ((yspec1 = yspec_new()) == NULL)
            goto done;
        clixon_client2_yspec_set(ch, yspec1);
        if (clixon_client2_capabilities_find(ch, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")){
            nr = 0;
            if ((ret = device_send_get_schema_next(h, ch, &nr)) < 0)
                goto done;
            if (ret == 0){ /* None found */
                device_close_connection(ch, "No YANG schemas announced");
            }
            else{
                clixon_client2_nr_schemas_set(ch, nr);
                clixon_client2_conn_state_set(ch, CS_SCHEMA_ONE);
                device_state_timeout_restart(ch);
            }
        }
        else if (clixon_client2_capabilities_find(ch, "http://xml.juniper.net/netconf/junos/1.0")){
            if ((ret = junos_schemas(ch)) < 0)
                goto done;
            device_close_connection(ch, "Junos work in progress");
        }
        else{
            device_close_connection(ch, "No method to get schemas");
        }
        break;
    case CS_SCHEMA_LIST:
        break;
    case CS_SCHEMA_ONE:
        /* Receive yang schema and parse it */
        if ((ret = device_state_get_schema(ch, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        /* Check if all schemas are received */
        nr = clixon_client2_nr_schemas_get(ch);
        if ((ret = device_send_get_schema_next(h, ch, &nr)) < 0)
            goto done;
        if (ret == 0){ /* None sent, done */
            if (device_state_all_schemas(h, ch) < 0)
                goto done;
            clixon_client2_conn_state_set(ch, CS_OPEN);
            device_state_timeout_unregister(ch);
        }
        else{
            clixon_client2_nr_schemas_set(ch, nr);
            device_state_timeout_restart(ch);
#if 1
            fprintf(stderr, "%s: SCHEMA -> SCHEMA(%d)\n",
                    clixon_client2_name_get(ch), nr);
#else
            clicon_debug(1, "%s: SCHEMA -> SCHEMA(%d)\n",
                         clixon_client2_name_get(ch), nr);
#endif
        }
        break;
    case CS_WRESP: /* XXX currently not used */
        /* Receive get and add to candidate, also commit */
        if (device_state_wresp2open(ch, xmsg, yspec0, rpcname, conn_state) < 0)
            goto done;
        clixon_client2_conn_state_set(ch, CS_OPEN);
        device_state_timeout_unregister(ch);
        break;
    case CS_CLOSED:
    case CS_OPEN:
    default:
        device_close_connection(ch, "Unexpected msg %s in state %s",
                                rpcname, controller_state_int2str(conn_state));
        break;
    }
    retval = 0;
 done:
    return retval;
}
