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
  * Simple service action for tests and debug based on openconfig interfaces
  * Read all test services, and add interface for each param
  * Proper service scripts are in pyapi
  * Simulated errors using -e <err> and -E <arg>:

  * nr enum  arg
  * 0: NONE
  * 1: SIM
  * 2: DUP
  * 3: TAG   tag
 */

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <syslog.h>
#include <fnmatch.h>
#include <errno.h>
#include <pwd.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>

#include "controller.h"

#define CONTROLLER_NAMESPACE "http://clicon.org/controller"

/* Command line options to be passed to getopt(3) */
#define SERVICE_ACTION_OPTS "hD:f:l:s:e:E:1"

enum {
    SEND_ERROR_NONE=0,
    SEND_ERROR_SIM,   /* Simulate a c-service transaction error */
    SEND_ERROR_DUP,   /* Simulate sending double messages */
    SEND_ERROR_TAG    /* Wrong creator tag.
                         See https://github.com/clicon/clixon-controller/issues/191 */
};

/*! Read services definition, write and mark an interface for each param in the service
 *
 * @param[in] h        Clixon handle
 * @param[in] tidstr   Transaction id
 * @retval    0        OK
 * @retval   -1        Error
 */
static int
send_transaction_actions_done(clixon_handle h,
                              char         *tidstr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;

   /* Write and mark an interface for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " username=\"%s\"", clicon_username_get(h));
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, ">");
    cprintf(cb, "<transaction-actions-done xmlns=\"%s\">",
            CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "</transaction-actions-done>");
    cprintf(cb, "</rpc>");
    if (clicon_rpc_msg(h, cb, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clixon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    retval = 0;
 done:
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send transaction error (simulated)
 *
 * @param[in]  h      Clixon handle
 * @param[in]  tidstr Transaction id
 * @param[in]  reason Reason for error
 * @retval     0      OK
 * @retval    -1      Error
 */
static int
send_transaction_error(clixon_handle h,
                       char         *tidstr,
                       const char   *reason)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "%s", reason);
    /* Write and mark an interface for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " username=\"%s\"", clicon_username_get(h));
    cprintf(cb, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, ">");
    cprintf(cb, "<transaction-error xmlns=\"%s\">",
            CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "<origin>c-service</origin>");
    cprintf(cb, "<reason>%s</reason>", reason);
    cprintf(cb, "</transaction-error>");
    cprintf(cb, "</rpc>");

    if (clicon_rpc_msg(h, cb, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clixon_err(OE_NETCONF, 0, "rpc-error");
        goto done;
    }
    retval = 0;
 done:
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
read_services(clixon_handle h,
              char         *db,
              cxobj       **xtp)
{
    int    retval = -1;
    cxobj *xt = NULL;
    cxobj *xd;
    cbuf  *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " username=\"%s\"", clicon_username_get(h));
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
    if (clicon_rpc_msg(h, cb, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clixon_err(OE_NETCONF, 0, "rpc-error");
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
read_devices(clixon_handle h,
             char         *db,
             cxobj       **xtp)
{
    int    retval = -1;
    cxobj *xt = NULL;
    cxobj *xd;
    cbuf  *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " username=\"%s\"", clicon_username_get(h));
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
    if (clicon_rpc_msg(h, cb, &xt) < 0)
        goto done;
    if (xpath_first(xt,  NULL, "rpc-reply/rpc-error") != NULL){
        clixon_err(OE_NETCONF, 0, "rpc-error");
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
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Given service+instance config, send an edit-config interface for each param in the service
 *
 * @param[in] h         Clixon handle
 * @param[in] s         Socket
 * @param[in] devname   Device name
 * @param[in] xsc       XML service tree
 * @param[in] db        Target datastore
 * @param[in] tag       Creator tag
 * @param[in] send_err  Simulated error
 * @param[in] send_arg  Error argument
 * @retval    0         OK
 * @retval   -1         Error
 */
static int
do_service(clixon_handle h,
           int           s,
           char         *devname,
           cxobj        *xsc,
           char         *db,
           char         *tag,
           char         *tidstr,
           int           send_err,
           char         *send_arg)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *x;
    char      *p;
    static int i = 0;

    if (i==0 && send_err == SEND_ERROR_TAG){
        tag = send_arg;
        clixon_debug(CLIXON_DBG_CTRL, "Inserted wrong tag: %s", tag);
        i++;
    }
    if (strcmp(db, "actions") != 0){
        clixon_err(OE_CFG, 0, "Unexpected datastore: %s (expected actions)", db);
        goto done;
    }
    /* Write and mark a interface for each param in the service */
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<config>");
    cprintf(cb, "<devices xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<device>");
    cprintf(cb, "<name>%s</name>", devname);
    cprintf(cb, "<config>");
    cprintf(cb, "<interfaces xmlns=\"%s\" nc:operation=\"merge\"",
            "http://openconfig.net/yang/interfaces");
    cprintf(cb, " xmlns:%s=\"%s\"", CLIXON_LIB_PREFIX, CLIXON_LIB_NS);
    cprintf(cb, ">");
    x = NULL;
    while ((x = xml_child_each(xsc, x,  CX_ELMNT)) != NULL){
        if (strcmp(xml_name(x), "params") != 0)
            continue;
        if ((p = xml_body(x)) == NULL)
            continue;
        cprintf(cb, "<interface %s:creator=\"%s\">", CLIXON_LIB_PREFIX, tag);
        cprintf(cb, "<name>%s</name>", p);
        cprintf(cb, "<config>");
        cprintf(cb, "<name>%s</name>", p);
        cprintf(cb, "<type xmlns:ianaift=\"%s\">ianaift:ethernetCsmacd</type>", "urn:ietf:params:xml:ns:yang:iana-if-type");
        cprintf(cb, "</config>");
        cprintf(cb, "</interface>");
    }
    if (send_err == SEND_ERROR_DUP){
        x = NULL;
        while ((x = xml_child_each(xsc, x,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(x), "params") != 0)
                continue;
            if ((p = xml_body(x)) == NULL)
                continue;
            cprintf(cb, "<interface %s:creator=\"%s\">", CLIXON_LIB_PREFIX, tag);
            cprintf(cb, "<name>%s</name>", p);
            cprintf(cb, "<config>");
            cprintf(cb, "<name>%s</name>", p);
            cprintf(cb, "<type xmlns:ianaift=\"%s\">ianaift:ethernetCsmacd</type>", "urn:ietf:params:xml:ns:yang:iana-if-type");
            cprintf(cb, "</config>");
            cprintf(cb, "</interface>");
            break;
        }
    }
    cprintf(cb, "</interfaces>");
    cprintf(cb, "</config>");
    cprintf(cb, "</device>");
    cprintf(cb, "</devices>");
    cprintf(cb, "</config>");
    /* (Read service and) produce device output and mark with service name */
    if (clicon_rpc_edit_config(h, "actions xmlns=\"http://clicon.org/controller\"",
                               OP_NONE, cbuf_get(cb)) < 0){
        if (send_err == SEND_ERROR_TAG){
            cbuf_reset(cb);
            cprintf(cb, "Invalid tag: %s", send_arg);
            send_transaction_error(h, tidstr, cbuf_get(cb));
        }
        else
            send_transaction_error(h, tidstr, "Error from controller in edit-config");
        goto done;
    }
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
 * @param[in]  send_err  Simulated error
 * @param[in]  send_arg  Error argument
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_loop_devices(clixon_handle h,
                     int           s,
                     char         *targetdb,
                     cxobj        *xdevs,
                     cxobj        *xs,
                     char         *tag,
                     char         *tidstr,
                     int           send_err,
                     char         *send_arg)

{
    int     retval = -1;
    cxobj  *xd;
    char   *devname;

    xd = NULL;
    while ((xd = xml_child_each(xdevs, xd,  CX_ELMNT)) != NULL){
        if (strcmp(xml_name(xd), "device") != 0)
            continue;
        devname = xml_find_body(xd, "name");
        if (do_service(h, s, devname, xs, targetdb, tag, tidstr, send_err, send_arg) < 0)
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
 * @param[in]  send_err  Simulated error
 * @param[in]  send_arg  Error argument
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_action_one(clixon_handle h,
                   int           s,
                   char         *pattern,
                   char         *targetdb,
                   cxobj        *xdevs,
                   cxobj        *xs,
                   char         *tidstr,
                   int           send_err,
                   char         *send_arg)
{
    int    retval = -1;
    cxobj *xi;
    char  *instance;
    cbuf  *cb = NULL;

    if ((xi = xml_find_type(xs, NULL, NULL, CX_ELMNT)) == NULL ||
        (instance = xml_body(xi)) == NULL)
        goto ok;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    /* XXX See also controller_actions_diff where tags are also created */
    cprintf(cb, "%s[%s='%s']", xml_name(xs), xml_name(xi), instance);
    if (service_loop_devices(h, s, targetdb, xdevs, xs, cbuf_get(cb), tidstr, send_err, send_arg) < 0)
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
 * @param[in]  send_err  Simulated error
 * @param[in]  send_arg  Error argument
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
service_action_instance(clixon_handle h,
                        int           s,
                        char         *pattern,
                        char         *targetdb,
                        cxobj        *xservices,
                        cxobj        *xdevs,
                        cxobj        *xsi,
                        char         *tidstr,
                        int           send_err,
                        char         *send_arg)
{
    int     retval = -1;
    char   *tag;
    cxobj  *xs;

    if ((tag = xml_body(xsi)) == NULL)
        goto ok;
    if (pattern != NULL && fnmatch(pattern, tag, 0) != 0)
        goto ok;
    /* Note: Assumes single key and that key is called "name"
     * See also controller_actions_diff()
     */
    if ((xs = xpath_first(xservices, NULL, "%s", tag)) != NULL){
        if (service_loop_devices(h, s, targetdb, xdevs, xs, tag, tidstr, send_err, send_arg) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Service commit notification handling, actions on test* services on all devices
 *
 * @param[in]  h            Clixon handle
 * @param[in]  s            Socket
 * @param[in]  notification XML of notification
 * @param[in]  pattern      Glob of services/instance, typically '*'
 * @param[in]  send_err     Send error instead of edit-config/done
 * @param[in]  send_arg     Error argument
 * @retval     0            OK
 * @retval    -1            Error
 */
static int
service_action_handler(clixon_handle h,
                       int           s,
                       char         *notification,
                       char         *pattern,
                       int           send_err,
                       char         *send_arg)
{
    int     retval = -1;
    cxobj  *xt = NULL;
    cxobj  *xn;
    cxobj  *xs;
    cxobj  *xsi;
    cxobj  *xservices = NULL;
    cxobj  *xdevs = NULL;
    char   *tidstr;
    char   *sourcedb = NULL;
    char   *targetdb = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if (clixon_xml_parse_string(notification, YB_NONE, NULL, &xt, NULL) < 0)
        goto done;
    if ((xn = xpath_first(xt, 0, "notification/services-commit")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if ((sourcedb = xml_find_body(xn, "source")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no source");
        goto done;
    }
    if ((targetdb = xml_find_body(xn, "target")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no source");
        goto done;
    }
    switch(send_err){
    case SEND_ERROR_NONE:
        break;
    case SEND_ERROR_SIM:
        if (send_transaction_error(h, tidstr, "simulated error") < 0)
            goto done;
        goto ok;
        break;
    default:
        break;
    }
    /* Read services and devices definition */
    if (read_services(h, sourcedb, &xservices) < 0)
        goto done;
    if (read_devices(h, sourcedb, &xdevs) < 0)
        goto done;
    if (xpath_first(xn, 0, "service") == 0){ /* All services: loop through service definitions */
        xs = NULL;
        while ((xs = xml_child_each(xservices, xs,  CX_ELMNT)) != NULL){
            if (service_action_one(h, s, pattern, targetdb, xdevs, xs, tidstr, send_err, send_arg) < 0)
                goto done;
        }
    }
    else {             /* Loop through specific service+instance field in notification */
        xsi = NULL;
        while ((xsi = xml_child_each(xn, xsi,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(xsi), "service") != 0)
                continue;
            if (service_action_instance(h, s, pattern, targetdb, xservices, xdevs, xsi, tidstr, send_err, send_arg) < 0)
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
 *
 * Cannot use h after this
 * @param[in]  h  Clixon handle
 */
static int
service_action_terminate(clixon_handle h)
{
    clixon_event_exit();
    clixon_debug(CLIXON_DBG_CTRL, "done");
    clixon_err_exit();
    clixon_log_exit();
    clixon_handle_exit(h);
    return 0;
}

/*! Quit
 */
static void
service_action_sig_term(int arg)
{
    static int i=0;

    clixon_log(NULL, LOG_NOTICE, "%s: %s: pid: %u Signal %d",
               __PROGRAM__, __func__, getpid(), arg);
    if (i++ > 0)
        exit(1);
    clixon_exit_set(1); /* checked in clixon_event_loop() */
}

/*! Usage
 */
static void
usage(clixon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s <options>*\n"
            "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D <level> \tDebug level (see available levels below)\n"
            "\t-f <file> \tConfig-file (mandatory)\n"
            "\t-l <s|e|o|n|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut, (n)one or (f)ile (syslog is default)\n"
            "\t-s <pattern> \tGlob pattern of services served, (default *)\n"
            "\t-e <nr> \tSend a transaction-error instead of transaction-done(trigger error)\n"
            "\t-E <msg> \tError argument, eg tag\n"
            "\t-1\t\tRun once and then quit (dont wait for events)\n",
            argv0
            );
    fprintf(stderr, "Debug keys: ");
    clixon_debug_key_dump(stderr);
    fprintf(stderr, "\n");
    exit(-1);
}

int
main(int    argc,
     char **argv)
{
    int            retval = -1;
    int            c;
    int            dbg = 0;
    int            logdst = CLIXON_LOG_SYSLOG|CLIXON_LOG_STDERR;
    clixon_handle  h = NULL; /* clixon handle */
    int            s = -1;
    int            eof = 0;
    char          *service_pattern = "*";
    int            send_err = SEND_ERROR_NONE;
    char          *send_arg = NULL;
    int            once = 0;
    struct passwd *pw;
    cbuf          *cb = NULL;

    if ((h = clixon_handle_init()) == NULL)
        goto done;;
    clixon_log_init(h, __PROGRAM__, LOG_INFO, logdst);
    /* Set username to clixon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
        clixon_err(OE_UNIX, errno, "getpwuid");
        goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
        goto done;
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
        case 'D' : { /* debug */
            int d = 0;
            /* Try first symbolic, then numeric match */
            if ((d = clixon_debug_str2key(optarg)) < 0 &&
                sscanf(optarg, "%d", &d) != 1){
                usage(h, argv[0]);
            }
            dbg |= d;
            break;
        }
        case 'f': /* config file */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
            break;
        case 'l': /* Log destination: s|e|o */
            if ((logdst = clixon_log_opt(optarg[0])) < 0)
                usage(h, argv[0]);
            if (logdst == CLIXON_LOG_FILE &&
                strlen(optarg)>1 &&
                clixon_log_file(optarg+1) < 0)
                goto done;
            break;
        case 's': /* service pattern */
            if (!strlen(optarg))
                usage(h, argv[0]);
            service_pattern = optarg;
            break;
        case 'e': /* error */
            send_err=atoi(optarg);
            break;
        case 'E': /* tag */
            send_arg = optarg;
            break;
        case '1' : /* Quit after reading database once - dont wait for events */
            once = 1;
            break;
        }

    clixon_log_init(h, __PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clixon_debug_init(h, dbg);
    yang_init(h);
    /* Setup handlers to exit cleanly when killed from parent or user */
    if (set_signal(SIGTERM, service_action_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGINT, service_action_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (send_err == SEND_ERROR_TAG && send_arg == NULL){
        clixon_err(OE_DAEMON, 0, "-e TAG expects -E arg");
        goto done;
    }

    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
        goto done;
    yang_start(h);

    /* Set RFC6022 session parameters that will be sent in first hello,
     * @see clicon_hello_req
     */
    clicon_data_set(h, "session-transport", "ctrl:services");
    if (clicon_rpc_create_subscription(h, "services-commit", NULL, &s) < 0){
        clixon_log(h, LOG_NOTICE, "services-commit: subscription failed: %s", clixon_err_reason());
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "notification socket:%d", s);

    if (!once)
        while (clixon_msg_rcv11(s, NULL, 0, &cb, &eof) == 0){
            if (eof)
                break;
            if (service_action_handler(h, s, cbuf_get(cb), service_pattern, send_err, send_arg) < 0)
                goto done;
            if (send_err == SEND_ERROR_DUP)
                send_err = SEND_ERROR_NONE; /* just once */
            if (cb){
                cbuf_free(cb);
                cb = NULL;
            }
        }
    retval = 0;
  done:
    if (cb)
        cbuf_free(cb);
    if (h)
        service_action_terminate(h); /* Cannot use h after this */
    return retval;
}
