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

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <syslog.h>
#include <errno.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>

enum controller_state{
    CS_INIT = 0,   /* init */
    CS_HELLO_RCVD, /* hello received */
    CS_HELLO_SENT, /* hello sent */
    CS_REQ_SENT,   /* request sent */
    CS_REPLY_RCVD  /* reply rcvd */
};
typedef enum controller_state  controller_state_t;

/* Command line options to be passed to getopt(3) */
#define CONTROLLER_OPTS "hD:d:l:"

/* clixon-data value to save buffer between invocations.
 * Saving data may be necessary if socket buffer contains partial netconf messages, such as:
 * <foo/> ..wait 1min  ]]>]]>
 */
#define NETCONF_HASH_BUF "netconf_input_cbuf"
#define NETCONF_FRAME_STATE "netconf_input_frame_state"
#define NETCONF_FRAME_SIZE "netconf_input_frame_size"

static int
capabilities_list(clicon_handle h,
                  FILE         *f)
{
    cxobj *xcaps = NULL;
    cxobj *x = NULL;

    clicon_ptr_get(h, "controller-capabilities", (void**)&xcaps);
    while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
        fprintf(f, "%s\n", xml_body(x));
    }
    return 0;
}

static int
capabilities_find(clicon_handle h,
                  const char   *name)
{
    cxobj *xcaps = NULL;
    cxobj *x = NULL;

    clicon_ptr_get(h, "controller-capabilities", (void**)&xcaps);
    while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
        if (strcmp(name, xml_body(x)) == 0)
            break;
    }
    return x?1:0;
}

static int
netconf_rpc_reply_message(clicon_handle h,
                         cxobj        *xrpc,
                         yang_stmt    *yspec,
                         int          *eof)
{
    int    retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    xml_print(stdout, xrpc);
    clicon_option_int_set(h, "controller-state", CS_REPLY_RCVD);
    retval = 0;
    // done:
    return retval;
}

static int
netconf_hello_msg(clicon_handle h,
                  cxobj        *xn,
                  int          *eof)
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
    clicon_ptr_set(h, "controller-capabilities", xcaps);
    /* Change state to hello received */
    if (clicon_option_int(h, "controller-state") ==  CS_INIT)
        clicon_option_int_set(h, "controller-state", CS_HELLO_RCVD);
   retval = 0;
 done:
    return retval;
}

static int
netconf_input_packet(clicon_handle h,
                     cxobj        *xreq,
                     yang_stmt    *yspec,
                     int          *eof)
{
    int     retval = -1;
    char   *rpcname;
    char   *rpcprefix;
    char   *namespace = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    rpcname = xml_name(xreq);
    rpcprefix = xml_prefix(xreq);
    if (xml2ns(xreq, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "rpc-reply") == 0){
        /* Only accept resolved NETCONF base namespace */
        if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
            *eof = 1;
            clicon_err(OE_XML, EFAULT, "No appropriate namespace associated with namespace:%s",
                       namespace);
            goto done;
        }
        if (netconf_rpc_reply_message(h, xreq, yspec, eof) < 0)
            goto done;
    }
    else if (strcmp(rpcname, "hello") == 0){
        /* Only accept resolved NETCONF base namespace -> terminate*/
        if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
            *eof = 1;
            clicon_err(OE_XML, EFAULT, "No appropriate namespace associated with namespace:%s",
                       namespace);
            goto done;
        }
        if (netconf_hello_msg(h, xreq, eof) < 0)
            goto done;
    }
    else{ /* Shouldnt happen should be caught by yang bind check in netconf_input_frame */
        *eof = 1;
        clicon_err(OE_NETCONF, 0, "Unrecognized netconf operation %s", rpcname);
        goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Process incoming frame, ie a char message framed by ]]>]]>
 * Parse string to xml, check only one netconf message within a frame
 * @param[in]   h    Clixon handle
 * @param[in]   cb   Packet buffer
 * @param[out]  eof  Set to 1 if pending close socket
 * @retval      0    OK
 * @retval     -1    Fatal error
 */
static int
netconf_input_frame(clicon_handle h, 
                    cbuf         *cb,
                    int          *eof)
{
    int                  retval = -1;
    char                *str = NULL;
    yang_stmt           *yspec;
    cxobj               *xtop = NULL; /* Request (in) */
    cxobj               *xerr = NULL;
    int                  ret;

    clicon_debug(1, "%s", __FUNCTION__);
    //    clicon_debug(2, "%s: \"%s\"", __FUNCTION__, cbuf_get(cb));
    if ((str = strdup(cbuf_get(cb))) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }    
    yspec = clicon_dbspec_yang(h);
    if (strlen(str) == 0){
        ;     /* Special case: empty: ignore */
    }
    else {
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
        else if (netconf_input_packet(h, xml_child_i_type(xtop, 0, CX_ELMNT), yspec, eof) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (str)
        free(str);
    return retval;
}

/*! Get netconf message: detect end-of-msg XXX could be moved to clixon_netconf_lib.c
 *
 * @param[in]     h           Clixon handle.
 * @param[in]     s           Socket where input arrived. read from this.
 * @param[in,out] frame_state 
 * @param[in,out] cb          Buffer
 * @param[out]    found       If frame found in cb
 * @retval        1           OK, frame found
 * @retval        0           OK, no frame yet
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
netconf_input_msg(clicon_handle h,
                  int           s,
                  int          *frame_state,
                  size_t       *frame_size,
                  cbuf         *cb,
                  int          *eof)
{
    int            retval = -1;
    unsigned char  buf[BUFSIZ]; /* from stdio.h, typically 8K */
    //    unsigned char  buf[256];
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
                if (clicon_option_int(h, "netconf-framing") == NETCONF_SSH_CHUNKED){
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
    retval = found;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

static int
netconf_input_process(clicon_handle h,
                      int           s,
                      int          *eof)
{
    int            retval = -1;
    cbuf          *cb = NULL;
    int            ret;
    int            frame_state;
    size_t         frame_size;
    void          *ptr;
    clicon_hash_t *cdat = clicon_data(h); /* Save cbuf between calls if not done */
    size_t         cdatlen = 0;

    if (clicon_option_exists(h, NETCONF_FRAME_STATE) == 0)
        frame_state = 0;
    else
        if ((frame_state = clicon_option_int(h, NETCONF_FRAME_STATE)) < 0)
            goto done;
    if (clicon_option_exists(h, NETCONF_FRAME_SIZE) == 0)
        frame_size = 0;
    else{
        if ((ret = clicon_option_int(h, NETCONF_FRAME_SIZE)) < 0)
            goto done;
        frame_size = (size_t)ret;
    }
    if ((ptr = clicon_hash_value(cdat, NETCONF_HASH_BUF, &cdatlen)) != NULL){
        if (cdatlen != sizeof(cb)){
            clicon_err(OE_XML, errno, "size mismatch %lu %lu",
                       (unsigned long)cdatlen, (unsigned long)sizeof(cb));
            goto done;
        }
        cb = *(cbuf**)ptr;
        clicon_hash_del(cdat, NETCONF_HASH_BUF);
    }
    else{
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }
    } 
    if ((ret = netconf_input_msg(h, s, &frame_state, &frame_size, cb, eof)) < 0)
        goto done;
    if (*eof == 0){
        clicon_option_int_set(h, NETCONF_FRAME_STATE, frame_state);
        clicon_option_int_set(h, NETCONF_FRAME_SIZE, frame_size);
        //        clicon_debug(1, "%s: cbuf: \"%s\"", __FUNCTION__, cbuf_get(cb)+p);
        if (ret == 1){ /* found */
            if (netconf_input_frame(h, cb, eof) < 0)
                goto done;
        }
        else{
            if (clicon_hash_add(cdat, NETCONF_HASH_BUF, &cb, sizeof(cb)) == NULL)
                goto done;
            cb = NULL;
        }
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

static int
netconf_input_cb(int   s,
                 void *arg)
{
    int retval = -1;
    int eof = 0;
    clicon_handle h = (clicon_handle)arg;
    int           version;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (netconf_input_process(h, s, &eof) < 0)
        goto done;
    if (eof){
        clixon_event_unreg_fd(s, netconf_input_cb);
        clixon_exit_set(1);     
    }
    /* state machine */
    switch (clicon_option_int(h, "controller-state")){
    case CS_INIT:
        break;
    case CS_HELLO_RCVD:{ /* XXX could also check capabilities set */
        cbuf *msg = NULL;
        if (clicon_debug_get())
            capabilities_list(h, stderr);
        if (capabilities_find(h, "urn:ietf:params:netconf:base:1.1"))
            version = 1;
        else if (capabilities_find(h, "urn:ietf:params:netconf:base:1.0"))
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
        clicon_option_int_set(h, "controller-state", CS_HELLO_SENT);
        if ((msg = cbuf_new()) == NULL){
            clicon_err(OE_PLUGIN, errno, "cbuf_new");
            goto done;
        }
        cprintf(msg, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
        cprintf(msg, " %s", NETCONF_MESSAGE_ID_ATTR);
        cprintf(msg, "><get-config><source><running/></source>");
        cprintf(msg, "</get-config></rpc>");
        if (netconf_output_encap(version, msg) < 0)
            goto done;
        if (clicon_msg_send1(s, msg) < 0)
            goto done;
        clicon_option_int_set(h, "controller-state", CS_REQ_SENT);
        if (msg)
            cbuf_free(msg);
        break;
    }
    case CS_REPLY_RCVD: /* Assume it is printed */
        clixon_event_unreg_fd(s, netconf_input_cb);
        clixon_exit_set(1);     
        break;
    default:
        break;
    }
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Clean and close all state of backend (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
controller_terminate(clicon_handle h)
{
    cxobj *x = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (clicon_ptr_get(h, "controller-capabilities", (void**)&x) == 0 && x){
        xml_free(x);
        x = NULL;
    }
    clixon_event_exit();
    clicon_debug(1, "%s done", __FUNCTION__); 
    clixon_err_exit();
    clicon_log_exit();
    clicon_handle_exit(h);
    return 0;
}

/*! wait for killed child
 * primary use in case restconf daemon forked using process-control API
 * This may cause EINTR in eg select() in clixon_event_loop() which will be ignored
 */
static void
controller_sig_child(int arg)
{
    clicon_debug(1, "%s", __FUNCTION__);
    clicon_sig_child_set(1);
}

/*! usage
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s <options>*\n"
            "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D <level>\tDebug level\n"
            "\t-d <user>@<ip>\tSSH destination (mandatory)\n"
            "\t-l <s|e|o|n|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut, (n)one or (f)ile (syslog is default)\n",
            argv0
            );
    exit(-1);
}

int
main(int    argc,
     char **argv)
{
    int                  retval = -1;
    int                  c;
    int                  dbg = 0;
    int                  logdst = CLICON_LOG_SYSLOG|CLICON_LOG_STDERR;
    clixon_handle        h = NULL; /* clixon handle */
    char                *dest = NULL;
    clixon_client_handle ch = NULL; /* clixon client handle */
    cxobj               *xdata = NULL;
    int                  s;

    clicon_log_init(__PROGRAM__, LOG_INFO, logdst);
    /*
     * Command-line options for help, debug, and config-file
     */
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, CONTROLLER_OPTS)) != -1)
        switch (c) {
        case 'h':
            usage(h, argv[0]);
            break;
        case 'D' : /* debug */
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(h, argv[0]);
            break;
        case 'l': /* Log destination: s|e|o */
            if ((logdst = clicon_log_opt(optarg[0])) < 0)
                usage(h, argv[0]);
            if (logdst == CLICON_LOG_FILE &&
                strlen(optarg)>1 &&
                clicon_log_file(optarg+1) < 0)
                goto done;
            break;
        case 'd': /* destination server */
            if (!strlen(optarg))
                usage(h, argv[0]);
            dest = optarg;
            break;
        }
    if (dest == NULL){
        fprintf(stderr, "-d <user@ip> is mandatory\n");
        usage(h, argv[0]);
    }
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clicon_debug_init(dbg, NULL);
    /* This is in case restconf daemon forked using process-control API */
    if (set_signal(SIGCHLD, controller_sig_child, NULL) < 0){
        clicon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    /* Provide a clixon config-file, get a clixon handle */
    if ((h = clicon_handle_init()) == NULL)
        goto done;;
    /* Make a connection over netconf or ssh/netconf */
    if ((ch = clixon_client_connect(h, CLIXON_CLIENT_SSH, dest)) == NULL)
       goto done;
    s = clixon_client_socket_get(ch);
    clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
    clicon_option_int_set(h, "controller-state", CS_INIT);
    if (clixon_event_reg_fd(s, netconf_input_cb, h, "netconf socket") < 0)
        goto done;
    if (clixon_event_loop(h) < 0)
        goto done;
    retval = 0;
  done:
    if (xdata)
       xml_free(xdata);
    if (ch)
        clixon_client_disconnect(ch);
    if (h)
        controller_terminate(h); /* Cannot use h after this */
    printf("done\n"); /* for test output */     
    return retval;
}
