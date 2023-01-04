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

/* 
 * Variables
 */
/* Timeout of transient states to closed in seconds */
static uint32_t _device_timeout = 60;

/* 
 * Forward declarations
 */
static int device_state_timeout_unregister(clixon_client_handle ch);
static int netconf_input_cb(int s, void *arg);

/*! Receive hello message
 * XXX maybe move semantics to netconf_input_cb
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
 * @retval      1     OK
 * @retval      0     Invalid, parse error, etc
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
        if ((ret = clixon_xml_parse_string(str, YB_RPC, yspec, &xtop, &xerr)) < 0){
            
            goto fail;
        }
#if 0 // XXX only after schema mount and get-schema stuff
        else if (ret == 0){
            clicon_log(LOG_WARNING, "%s: YANG error", __FUNCTION__);
            goto fail;
        }
#endif
        else if (xml_child_nr_type(xtop, CX_ELMNT) == 0){
            clicon_log(LOG_WARNING, "%s: empty frame", __FUNCTION__);
            goto fail;
        }
        else if (xml_child_nr_type(xtop, CX_ELMNT) != 1){
            clicon_log(LOG_WARNING, "%s: multiple message in single frames", __FUNCTION__);
            goto fail;
        }
        else
            *xrecv = xtop;
    }
    retval = 1;
 done:
    if (str)
        free(str);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get netconf message: detect end-of-msg XXX could be moved to clixon_netconf_lib.c
 *
 * @param[in]     ch          Clixon client handle.
 * @param[in]     s           Socket where input arrives. Read from this.
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
#if 1
        break;
#else
        /* This is a way to keep reading, may be better for performance */
        if (found) /* frame found */
            break;
        {
            int poll;
            if ((poll = clixon_event_poll(s)) < 0)
                goto done;
            if (poll == 0){
                clicon_debug(1, "%s poll==0: no data on s", __FUNCTION__);
                break; 
            }
        }
#endif

    } /* while */
    *eom = found;
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Close connection, unregister events and timers
 * @param[in]  ch      Clixon client handle.
 * @param[in]  format  Format string for Log message
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
close_connection(clixon_client_handle ch,
                 const char *format, ...) __attribute__ ((format (printf, 2, 3)));
static int
close_connection(clixon_client_handle ch,
                 const char *format, ...)
{
    int            retval = -1;
    int            s;
    va_list        ap;
    size_t         len;
    char          *str = NULL;
    
    s = clixon_client2_socket_get(ch);
    clixon_event_unreg_fd(s, netconf_input_cb); /* deregister events */
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
#if 0 // XXX
    clixon_client2_free(ch);                    /* free mem */
#endif
    retval = 0;
 done:
    return retval;
}

/*! Send a <get> request to a device
 *
 * @param[in]  h   Clicon handle
 * @param[in]  ch  Clixon client handle
 */
static int
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
    cprintf(cb, "devices/device[name=\"%s\"]/data/netconf-state/schemas/schema", clixon_client2_name_get(ch));
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

/*! Controller input connecting to monitoring state handling
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
device_state_connecting2monitoring(clixon_client_handle ch,
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
        close_connection(ch, "Unexpected msg %s in state %s",
                   rpcname, controller_state_int2str(conn_state));
        goto ok;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        close_connection(ch,  "No appropriate namespace associated with %s",
                   namespace);
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

/*! Controller input connecting to monitoring state handling
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
        close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
        goto ok;
    }
    name = clixon_client2_name_get(ch);
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        close_connection(ch, "No appropriate namespace associated with:%s",
                   namespace);
        goto ok;
    }
    if ((yspec = clixon_client2_yspec_get(ch)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    if ((ystr = xml_find_body(xmsg, "data")) == NULL)
        goto ok;
    if (xml_chardata_decode(&ydec, "%s", ystr) < 0)
        goto done;
    if ((ymod = yang_parse_str(ydec, name, yspec)) == NULL)
        goto done;
#if 0
    { /* Dump to file */
        cbuf  *cb = cbuf_new();
        FILE  *f;
        size_t sz;
        
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "/var/tmp/%s/yang/%s.yang", name, yang_argument_get(ymod));
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
 ok:
    retval = 0;
 done:
    if (ydec)
        free(ydec);
    return retval;
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

/*! Controller input sync to open state handling, read rpc-reply
 *
 * @param[in] ch         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    0          OK
 * @retval   -1          Error
 */
static int
device_state_sync2open(clixon_client_handle ch,
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
        close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
        goto ok;
    }
    name = clixon_client2_name_get(ch);
    h = clixon_client2_handle_get(ch);
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        close_connection(ch, "No appropriate namespace associated with:%s",
                   namespace);
        goto ok;
    }
    xdata = xpath_first(xmsg, NULL, "data");
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
    cprintf(cb, "</device></devices>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x1, NULL)) < 0)
        goto done;
    if (xml_name_set(x1, "config") < 0)
        goto done;
    if ((xc = xpath_first(x1, NULL, "devices/device")) != NULL){
        yang_stmt *y;
        if (xml_addsub(xc, xdata) < 0)
            goto done;
        if ((y = yang_find(xml_spec(xc), Y_ANYDATA, "data")) == NULL){
            clicon_err(OE_YANG, 0, "Not finding proper yang child of device(shouldnt happen)");
            goto done;
        }
        xml_spec_set(xdata, y);
#if 0 // XXX debug
        xml_print(stdout, x1);
        fflush(stdout);
#endif
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
 ok:
    retval = 0;
 done:
    if (cbret)
        cbuf_free(cbret);
    if (cb)
        cbuf_free(cb);
    return retval;
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
        close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
    }
    /* Nothing yet */
    retval = 0;
    // done:
    return retval;
}

/*! Timeout of transient states
 */
static int
device_state_timeout(int   s,
                     void *arg)
{
    clixon_client_handle ch = (clixon_client_handle)arg;

    close_connection(ch, "Timeout waiting for remote peer");
    return 0;
}

static int
device_state_timeout_register(clixon_client_handle ch)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;

    gettimeofday(&t, NULL);
    t1.tv_sec = _device_timeout;
    t1.tv_usec = 0;
    timeradd(&t, &t1, &t);
    if (clixon_event_reg_timeout(t, device_state_timeout, ch,
                                 "Device state timeout") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

static int
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

/*! Handle input data, whole or part of a frame
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
    int                  frame_state; /* only used for chunked framing not eom */
    size_t               frame_size;
    cbuf                *cb;
    yang_stmt           *yspec;
    yang_stmt           *yspec1;
    cxobj               *xtop = NULL;
    cxobj               *xmsg;
    conn_state_t         conn_state;
    char                *rpcname;
    int                  ret;
    struct timeval       tsync;
    int                  nr;

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
    clicon_debug(1, "%s eom:%d eof:%d len:%lu", __FUNCTION__, eom, eof, cbuf_len(cb));
    if (eof){         /* Close connection, unregister events, free mem */
        close_connection(ch, "Remote socket endpoint closed");
        goto ok;
    }
    clixon_client2_frame_state_set(ch, frame_state);
    clixon_client2_frame_size_set(ch, frame_size);
    if (eom == 0) /* frame not found */
        goto ok;
    clicon_debug(1, "%s frame: %lu strlen:%lu", __FUNCTION__, cbuf_len(cb), strlen(cbuf_get(cb)));
    cbuf_trunc(cb, cbuf_len(cb));
#if 1 // XXX debug
    if (clicon_debug_get()){
        fprintf(stdout, "%s\n", cbuf_get(cb));
        fflush(stdout);
    }
#endif
    yspec = clicon_dbspec_yang(h);
    if ((ret = netconf_input_frame(ch, cb, yspec, &xtop)) < 0)
        goto done;
    cbuf_reset(cb);
    if (ret==0){
        close_connection(ch, "Invalid frame");
        goto ok;
    }
    xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
    rpcname = xml_name(xmsg);
    conn_state = clixon_client2_conn_state_get(ch);
    switch (conn_state){
    case CS_CONNECTING:
        if (device_state_connecting2monitoring(ch, s, xmsg, rpcname, conn_state) < 0)
            goto done;
        clixon_client2_sync_time_get(ch, &tsync);
        /* Check if synced, if not, sync now */
#if NOTYET
         (clixon_client2_capabilities_find(ch, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")
#endif
          if (tsync.tv_sec == 0){
            if (device_sync(h, ch) < 0)
                goto done;
            clixon_client2_conn_state_set(ch, CS_DEVICE_SYNC);
            device_state_timeout_restart(ch);
        }
        else{
            clixon_client2_conn_state_set(ch, CS_OPEN);
            device_state_timeout_unregister(ch);
        }
        break;
    case CS_DEVICE_SYNC:
          if (device_state_sync2open(ch, xmsg, yspec, rpcname, conn_state) < 0)
            goto done;
          if (clixon_client2_capabilities_find(ch, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")  &&
              clixon_client2_yspec_get(ch) == NULL){
              if ((yspec1 = yspec_new()) == NULL)
                  goto done;
              clixon_client2_yspec_set(ch, yspec1);
              nr = 0;
              if ((ret = device_send_get_schema_next(h, ch, &nr)) < 0)
                  goto done;
              if (ret == 0){ /* None found */
                  clixon_client2_conn_state_set(ch, CS_OPEN);
                  device_state_timeout_unregister(ch);
              }
              else{
                  clixon_client2_nr_schemas_set(ch, nr);
                  clixon_client2_conn_state_set(ch, CS_SCHEMA);
                  device_state_timeout_restart(ch);
              }
          }
          else{
              clixon_client2_conn_state_set(ch, CS_OPEN);
              device_state_timeout_unregister(ch);
          }
        break;
    case CS_SCHEMA:
        if (device_state_get_schema(ch, xmsg, rpcname, conn_state) < 0)
            goto done;
          nr = clixon_client2_nr_schemas_get(ch);
          if ((ret = device_send_get_schema_next(h, ch, &nr)) < 0)
              goto done;
          if (ret == 0){ /* None sent, done */
              if (device_state_all_schemas(h, ch) < 0)
                  goto done;
              clixon_client2_conn_state_set(ch, CS_OPEN);
              device_state_timeout_unregister(ch);
              break;
          }
          clixon_client2_nr_schemas_set(ch, nr);
          device_state_timeout_restart(ch);
#if 1
          fprintf(stderr, "%s: SCHEMA -> SCHEMA(%d)\n",
                       clixon_client2_name_get(ch), nr);
#else
                    clicon_debug(1, "%s: SCHEMA -> SCHEMA(%d)\n",
                       clixon_client2_name_get(ch), nr);
#endif
          break;
    case CS_WRESP:
        /* Receive get and add to candidate, also commit */
        if (device_state_wresp2open(ch, xmsg, yspec, rpcname, conn_state) < 0)
            goto done;
        clixon_client2_conn_state_set(ch, CS_OPEN);
        device_state_timeout_unregister(ch);
        break;
    case CS_CLOSED:
    case CS_OPEN:
    default:
        close_connection(ch, "Unexpected msg %s in state %s",
                         rpcname, controller_state_int2str(conn_state));
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
        close_connection(ch, NULL); /* Regular disconnect, no reason */
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
    
    if (xpath_vec_flag(target, nsc, "generic/device-timeout",
                       XML_FLAG_ADD | XML_FLAG_CHANGE,
                       &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((body = xml_body(vec[i])) == NULL)
            continue;
        if (parse_uint32(body, &_device_timeout, NULL) < 1){
            clicon_err(OE_UNIX, errno, "error parsing limit:%s", body);
            goto done;
        }
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
