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
 * @retval    0        OK
 * @retval   -1        Error
 */
static int
send_transaction_actions_done(clicon_handle h,
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
    cprintf(cb, "<transaction-actions-done xmlns=\"%s\">", 
            CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
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


/*! Read services definition from backend
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database name
 * @param[out] xtp  Top of services tree
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
read_services(clicon_handle     h,
              char             *db,
              cxobj           **xtp)
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
    cprintf(cb, "<get-config>");
    cprintf(cb, "<source>");
    if (strcmp(db, "actions") == 0) /* XXX: Hardcoded namespace */
        cprintf(cb, "<%s xmlns=\"%s\"/>", db, CONTROLLER_NAMESPACE);
    else
        cprintf(cb, "<%s/>", db);
    cprintf(cb, "</source>");
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
    cprintf(cb, "<source>");
    if (strcmp(db, "actions") == 0) /* XXX: Hardcoded namespace */
        cprintf(cb, "<%s xmlns=\"%s\"/>", db, CONTROLLER_NAMESPACE);
    else
        cprintf(cb, "<%s/>", db);
    cprintf(cb, "</source>");
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

/*! Given service+instance config, send an edit-config table/param for each param in the service
 * 
 * @param[in] h       Clixon handle
 * @param[in] s       Socket
 * @param[in] devname Device name
 * @param[in] xsc     XML service tree
 * @param[in] db      Target datastore
 * @param[in] tag     Creator tag
 * @retval    0       OK
 * @retval   -1       Error
 */
static int
do_service(clicon_handle h,
           int           s,
           char         *devname,
           cxobj        *xsc,
           char         *db,
           char         *tag)
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
    cprintf(cb, " xmlns:%s=\"%s\"", CLIXON_LIB_PREFIX, CLIXON_LIB_NS);
    cprintf(cb, ">");
    x = NULL;
    while ((x = xml_child_each(xsc, x,  CX_ELMNT)) != NULL){
        if (strcmp(xml_name(x), "params") != 0)
            continue;
        if ((p = xml_body(x)) == NULL)
            continue;        
        cprintf(cb, "<parameter %s:creator=\"%s\">", CLIXON_LIB_PREFIX, tag);
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

/*! Loop over all devices
 *
 * @param[in]  h         Clixon handle
 * @param[in]  s         Socket
 * @param[in]  targetdb  Datastore to edit
 * @param[in]  xdevs     Devices XML tree
 * @param[in]  xs        XML tree of one service instance (in config services tree)
 * @param[in]  tag       Service/instance tag
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_loop_devices(clicon_handle h,
                     int           s,
                     char         *targetdb,
                     cxobj        *xdevs,
                     cxobj        *xs,
                     char         *tag)
{
    int     retval = -1;
    cxobj  *xd;
    char   *devname;

    xd = NULL;
    while ((xd = xml_child_each(xdevs, xd,  CX_ELMNT)) != NULL){
        devname = xml_find_body(xd, "name");
        if (do_service(h, s, devname, xs, targetdb, tag) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}    

/*! Iterate through one service+instance
 *
 * @param[in]  h         Clixon handle
 * @param[in]  s         Socket
 * @param[in]  pattern   Glob of services/instance, typically '*'
 * @param[in]  targetdb  Datastore to edit
 * @param[in]  xdevs     Devices XML tree
 * @param[in]  xs        XML tree of one service instance (in config services tree)
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_action_one(clicon_handle h,
                   int           s,
                   char         *pattern,
                   char         *targetdb,
                   cxobj        *xdevs,
                   cxobj        *xs)
{
    int    retval = -1;
    cxobj *xi;
    char  *instance;
    cbuf  *cb = NULL;

    if ((xi = xml_find_type(xs, NULL, NULL, CX_ELMNT)) == NULL ||
        (instance = xml_body(xi)) == NULL)
        goto ok;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s/%s", xml_name(xs), instance);
    if (service_loop_devices(h, s, targetdb, xdevs, xs, cbuf_get(cb)) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Specific service+instance handler, given tag find that service instance and handle it
 *
 * @param[in]  h         Clixon handle
 * @param[in]  s         Socket
 * @param[in]  pattern   Glob of services/instance, typically '*'
 * @param[in]  targetdb  Datastore to edit
 * @param[in]  xservices Services XML tree
 * @param[in]  xdevs     Devices XML tree
 * @param[in]  xsi       XML tree of one service instance (in notification msg tree)
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_action_instance(clicon_handle h,
                        int           s,
                        char         *pattern,
                        char         *targetdb,
                        cxobj        *xservices,
                        cxobj        *xdevs,
                        cxobj        *xsi)
{
    int     retval = -1;
    char   *service_name = NULL;
    char   *instance = NULL;
    char   *tag;
    cxobj  *xs;
    
    if ((tag = xml_body(xsi)) == NULL)
        goto ok;
    if (pattern != NULL && fnmatch(pattern, tag, 0) != 0)
        goto ok;
    if (clixon_strsplit(tag, '/', &service_name, &instance) < 0)
        goto done;
    /* Note: Assumes single key and that key is called "name" 
     * See also controller_actions_diff()
     */
    if ((xs = xpath_first(xservices, NULL, "%s[name='%s']", service_name, instance)) != NULL){
        if (service_loop_devices(h, s, targetdb, xdevs, xs, tag) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (service_name)
        free(service_name);
    if (instance)
        free(instance);
    return retval;
}

/*! Service commit notification handling, actions on test* services on all devices
 *
 * @param[in]  h            Clixon handle
 * @param[in]  s            Socket
 * @param[in]  notification XML of notification
 * @param[in]  pattern      Glob of services/instance, typically '*'
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
    cxobj  *xsi;
    cxobj  *xservices = NULL;
    cxobj  *xdevs = NULL;
    char   *tidstr;
    int     ret;
    char   *sourcedb = NULL;
    char   *targetdb = NULL;

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
    if (xpath_first(xn, 0, "service") == 0){ /* All services: loop through service definitions */
        xs = NULL;
        while ((xs = xml_child_each(xservices, xs,  CX_ELMNT)) != NULL){
            if (service_action_one(h, s, pattern, targetdb, xdevs, xs) < 0)
                goto done;
        }
    }
    else {             /* Loop through specific service+instance field in notification */
        xsi = NULL;
        while ((xsi = xml_child_each(xn, xsi,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(xsi), "service") != 0)
                continue;
            if (service_action_instance(h, s, pattern, targetdb, xservices, xdevs, xsi) < 0)
                goto done;
        }
    }
    if (send_transaction_actions_done(h, tidstr) < 0)
        goto done;
 ok:
    retval = 0;
 done:
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
    clixon_event_exit();
    clicon_debug(1, "%s done", __FUNCTION__); 
    clixon_err_exit();
    clicon_log_exit();
    clicon_handle_exit(h);
    return 0;
}

/*! Quit
 */
static void
service_action_sig_term(int arg)
{
    static int i=0;
    
    clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
               __PROGRAM__, __FUNCTION__, getpid(), arg);
    if (i++ > 0)
        exit(1);
    clixon_exit_set(1); /* checked in clixon_event_loop() */
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
    /* Setup handlers to exit cleanly when killed from parent or user */
    if (set_signal(SIGTERM, service_action_sig_term, NULL) < 0){
        clicon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGINT, service_action_sig_term, NULL) < 0){
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
