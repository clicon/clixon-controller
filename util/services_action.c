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
  * Simple service action for tests and debug.
  * Read all test services, and add table parameter for each param
  * Proper action scripts are in pyapi
 */

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <syslog.h>
#include <fnmatch.h>
#include <errno.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>

#define CONTROLLER_NAMESPACE "http://clicon.org/controller"

/* Command line options to be passed to getopt(3) */
#define SERVICE_ACTION_OPTS "hD:f:l:s:e"

/*! Read services definition, write and mark a table/param for each param in the service
 * 
 * @param[in] h        Clixon handle
 * @param[in] tidstr   Transaction id
 * @param[in] servstr  String of XML containing list of processed services
 * @retval    0        OK
 * @retval   -1        Error
 */
static int
send_transaction_actions_done(clicon_handle h,
                              char         *tidstr,
                              char         *servstr)
{
    int                retval = -1;
    cbuf              *cb = NULL;
    struct clicon_msg *msg = NULL;
    cxobj             *xt = NULL;

   /* Write and mark a table/param for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR); 
    cprintf(cb, ">");
    cprintf(cb, "<transaction-actions-done xmlns=\"%s\">", 
            CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "%s", servstr);
    cprintf(cb, "</transaction-actions-done>");
    cprintf(cb, "</rpc>");
    if ((msg = clicon_msg_encode(0, "%s", cbuf_get(cb))) == NULL)
        goto done;
    if (clicon_rpc_msg(h, msg, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clicon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    retval = 0;
 done:
    if (msg)
        free(msg);
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
}

static int
send_transaction_error(clicon_handle h,
                       char         *tidstr)
{
    int                retval = -1;
    cbuf              *cb = NULL;
    struct clicon_msg *msg = NULL;
    cxobj             *xt = NULL;

   /* Write and mark a table/param for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR); 
    cprintf(cb, ">");
    cprintf(cb, "<transaction-error xmlns=\"%s\">", 
            CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "<origin>service action</origin>");
    cprintf(cb, "<reason>simulated error</reason>");
    cprintf(cb, "</transaction-error>");
    cprintf(cb, "</rpc>");
    if ((msg = clicon_msg_encode(0, "%s", cbuf_get(cb))) == NULL)
        goto done;
    if (clicon_rpc_msg(h, msg, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clicon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    retval = 0;
 done:
    if (msg)
        free(msg);
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Read services definition, send an edit-config table/param for each param in the service
 * 
 * @param[in] h       Clixon handle
 * @param[in] s       Socket
 * @param[in] devname Device name
 * @param[in] xsc     XML service tree
 * @param[in] db      Target datastore
 * @retval    0       OK
 * @retval   -1       Error
 */
static int
do_service(clicon_handle h,
           int           s,
           char         *devname,
           cxobj        *xsc,
           char         *db,
           char         *service_name)
{
    int   retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;
    char  *p;

    if (strcmp(db, "actions") != 0){
        clicon_err(OE_CFG, 0, "Unexpected datastore: %s (expected actions)", db);
        goto done;
    }
    /* Write and mark a table/param for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<config>");
    cprintf(cb, "<devices xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<device>");
    cprintf(cb, "<name>%s</name>", devname);
    cprintf(cb, "<config>");
    cprintf(cb, "<table xmlns=\"%s\" nc:operation=\"merge\"", "urn:example:clixon");
    cprintf(cb, " xmlns:%s=\"%s\">", CLIXON_LIB_PREFIX, CLIXON_LIB_NS);
    cprintf(cb, ">");
    x = NULL;
    while ((x = xml_child_each(xsc, x,  CX_ELMNT)) != NULL){
        if (strcmp(xml_name(x), "params") != 0)
            continue;
        if ((p = xml_body(x)) == NULL)
            continue;        
        cprintf(cb, "<parameter %s:creator=\"%s\">", CLIXON_LIB_PREFIX, service_name);
        cprintf(cb, "<name>%s</name>", p);
        cprintf(cb, "</parameter>");
    }
    cprintf(cb, "</table>");
    cprintf(cb, "</config>");
    cprintf(cb, "</device>");
    cprintf(cb, "</devices>");
    cprintf(cb, "</config>");
    /* (Read service and) produce device output and mark with service name */
    if (clicon_rpc_edit_config(h,
                               "actions xmlns=\"http://clicon.org/controller\"",
                               OP_NONE, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Read services definition from backend
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database name
 * @param[out] xtp  Top of services tree
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
read_services(clicon_handle      h,
              char             *db,
              cxobj            **xtp)
{
    int                retval = -1;
    cxobj             *xt = NULL;
    cxobj             *xd;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR); /* XXX: use incrementing sequence */
    cprintf(cb, ">");
    cprintf(cb, "<get-config><source><%s/></source>", db);
    cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"ctrl:services\" xmlns:ctrl=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
            CONTROLLER_NAMESPACE);
    cprintf(cb, "/>");
    cprintf(cb, "</get-config></rpc>");
    if ((msg = clicon_msg_encode(0, "%s", cbuf_get(cb))) == NULL)
        goto done;
    if (clicon_rpc_msg(h, msg, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clicon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    if ((xd = xpath_first(xt, NULL, "rpc-reply/data/services")) != NULL){
        xml_rm(xd);
        *xtp = xd;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (msg)
        free(msg);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Read devices definition from backend, to depth devices/device/name
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database name
 * @param[out] xtp  Top of devices tree
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
read_devices(clicon_handle h,
             char         *db,
             cxobj       **xtp)
{
    int                retval = -1;
    cxobj             *xt = NULL;
    cxobj             *xd;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR); /* XXX: use incrementing sequence */
    cprintf(cb, ">"); 
    cprintf(cb, "<get-config nc:depth=\"4\">"); /* depth to include name, etc */
    cprintf(cb, "<source><%s/></source>", db);
    cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"ctrl:devices\" xmlns:ctrl=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
            CONTROLLER_NAMESPACE);
    cprintf(cb, "/>");
    cprintf(cb, "</get-config></rpc>");
    if ((msg = clicon_msg_encode(0, "%s", cbuf_get(cb))) == NULL)
        goto done;
    if (clicon_rpc_msg(h, msg, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clicon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    if ((xd = xpath_first(xt, NULL, "rpc-reply/data/devices")) != NULL){
        xml_rm(xd);
        *xtp = xd;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (msg)
        free(msg);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Service commit notification handling, actions on test* services on all devices
 *
 * @param[in]  h            Clixon handle
 * @param[in]  s            Socket
 * @param[in]  notification XML of notification
 * @param[in]  pattern
 * @param[in]  send_error   Send error instead of edit-config/done
 * @retval     0            OK
 * @retval    -1            Error
 */
static int
service_action_handler(clicon_handle      h,
                       int                s,
                       struct clicon_msg *notification,
                       char              *pattern,
                       int                send_error)
{
    int     retval = -1;
    cxobj  *xt = NULL;
    cxobj  *xn;
    cxobj  *xs;
    cxobj  *xd;
    cxobj  *xservices = NULL;
    cxobj  *xdevs = NULL;
    cxobj  *xsc;
    char   *tidstr;
    char   *service_name;
    char   *devname;
    int     ret;
    cbuf   *cbs = NULL;
    char   *sourcedb = NULL;
    char   *targetdb = NULL;
    int     nr=0;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((ret = clicon_msg_decode(notification, NULL, NULL, &xt, NULL)) < 0) 
        goto done;
    if (ret == 0){ /* will not happen since no yspec ^*/
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    if ((xn = xpath_first(xt, 0, "notification/services-commit")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if ((sourcedb = xml_find_body(xn, "source")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no source");
        goto done;
    }
    if ((targetdb = xml_find_body(xn, "target")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no source");
        goto done;
    }    
    if (send_error){
        if (send_transaction_error(h, tidstr) < 0)
            goto done;
        goto ok;
    }
    /* Read services and devices definition */
    if (read_services(h, sourcedb, &xservices) < 0)
        goto done;
    if (read_devices(h, sourcedb, &xdevs) < 0)
        goto done;
    if ((cbs = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    xs = NULL;
    while ((xs = xml_child_each(xn, xs,  CX_ELMNT)) != NULL){
        if (strcmp(xml_name(xs), "service") != 0)
            continue;
        if ((service_name = xml_body(xs)) == NULL)
            continue;
        nr++;
    }
    if (nr==0){ /* All services */
        xs = NULL;
        while ((xs = xml_child_each(xservices, xs,  CX_ELMNT)) != NULL){
            service_name = xml_name(xs);
            if (pattern != NULL && fnmatch(pattern, service_name, 0) != 0)
                continue;
            cprintf(cbs, "<service>%s</service>", service_name);
            /* Loop over all devices */
            xd = NULL;
            while ((xd = xml_child_each(xdevs, xd,  CX_ELMNT)) != NULL){
                devname = xml_find_body(xd, "name");
                if (do_service(h, s, devname, xs, targetdb, service_name) < 0)
                    goto done;
            }
        }
    }
    else {
        xs = NULL;
        while ((xs = xml_child_each(xn, xs,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(xs), "service") != 0)
                continue;
            if ((service_name = xml_body(xs)) == NULL)
                continue;
            if (pattern != NULL && fnmatch(pattern, service_name, 0) != 0)
                continue;
            if ((xsc = xpath_first(xservices, NULL, "%s", service_name)) != NULL){
                cprintf(cbs, "<service>%s</service>", service_name);
                /* Loop over all devices */
                xd = NULL;
                while ((xd = xml_child_each(xdevs, xd,  CX_ELMNT)) != NULL){
                    devname = xml_find_body(xd, "name");
                    if (do_service(h, s, devname, xsc, targetdb, service_name) < 0)
                        goto done;
                }
            }
        }
    }
    if (send_transaction_actions_done(h, tidstr, cbuf_get(cbs)) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    if (cbs)
        cbuf_free(cbs);
    if (xservices)
        xml_free(xservices);
    if (xdevs)
        xml_free(xdevs);
    if (xt)
        xml_free(xt);
    return retval;    
}

/*! Clean and close all state of backend (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
service_action_terminate(clicon_handle h)
{
    clicon_debug(1, "%s", __FUNCTION__);
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
service_action_sig_child(int arg)
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
            "\t-f <file> \tConfig-file (mandatory)\n"
            "\t-l <s|e|o|n|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut, (n)one or (f)ile (syslog is default)\n"
            "\t-s <pattern> \tGlob pattern of services served, (default *)\n"
            "\t-e  \tSend an error instead of done\n",
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
    clixon_client_handle ch = NULL; /* clixon client handle */
    int                  s = -1;
    struct clicon_msg   *notification = NULL;
    int                  eof = 0;
    char                *service_pattern = "*";
    int                  send_error = 0;

    clicon_log_init(__PROGRAM__, LOG_INFO, logdst);
    if ((h = clicon_handle_init()) == NULL)
        goto done;;
    /*
     * Command-line options for help, debug, and config-file
     */
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, SERVICE_ACTION_OPTS)) != -1)
        switch (c) {
        case 'h':
            usage(h, argv[0]);
            break;
        case 'D' : /* debug */
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(h, argv[0]);
            break;
        case 'f': /* config file */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
            break;
        case 'l': /* Log destination: s|e|o */
            if ((logdst = clicon_log_opt(optarg[0])) < 0)
                usage(h, argv[0]);
            if (logdst == CLICON_LOG_FILE &&
                strlen(optarg)>1 &&
                clicon_log_file(optarg+1) < 0)
                goto done;
            break;
        case 's': /* service pattern */
            if (!strlen(optarg))
                usage(h, argv[0]);
            service_pattern = optarg;
            break;
        case 'e': /* error */
            send_error++;
            break;
        }
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clicon_debug_init(dbg, NULL);
    /* This is in case restconf daemon forked using process-control API */
    if (set_signal(SIGCHLD, service_action_sig_child, NULL) < 0){
        clicon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
        goto done;
    if (clicon_rpc_create_subscription(h, "services-commit", NULL, &s) < 0)
        goto done;
    clicon_debug(CLIXON_DBG_DEFAULT, "%s notification socket:%d", __FUNCTION__, s);
    while (clicon_msg_rcv(s, 1, &notification, &eof) == 0){
        if (eof)
            break;
        if (service_action_handler(h, s, notification, service_pattern, send_error) < 0)
            goto done;
        if (notification){
            free(notification);
            notification = NULL;
        }
    }
    retval = 0;
  done:
    if (ch)
        clixon_client_disconnect(ch);
    if (h)
        service_action_terminate(h); /* Cannot use h after this */
    if (notification)
        free(notification);
    printf("done\n"); /* for test output */     
    return retval;
}
