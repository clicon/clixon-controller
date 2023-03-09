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
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>
#include <fnmatch.h>
#include <signal.h> /* matching strings */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netinet/in.h>

/* clicon */
#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_cli.h>
#include <clixon/cli_generate.h>

/* Controller includes */
#include "controller.h"

/*! Common transaction notification handling from both async and poll
 *
 * @param[in]   s       Notification socket
 * @param[in]   tidstr0 Transaction id string
 * @param[out]  match   Transaction id match
 * @param[out]  status  0: transaction failed, 1: transaction OK (if match)
 * @param[out]  eof     EOF, socket closed
 * @param[out]  eof     EOF, socket closed
 * @retval      0       OK
 * @retval     -1       Fatal error
 */
static int
transaction_notification_handler(int   s,
                                 char *tidstr0,
                                 int  *match,
                                 int  *status,
                                 int  *eof)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    cxobj             *xt = NULL;
    cxobj             *xn;
    int                ret;
    char              *tidstr;
    char              *statusstr;
    
    if (clicon_msg_rcv(s, 1, &reply, eof) < 0)
        goto done;
    if (*eof){
        clicon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
        close(s);
        goto done; /* Fatal, or possibly cli may reconnect */
    }
    if ((ret = clicon_msg_decode(reply, NULL, NULL, &xt, NULL)) < 0) 
        goto done;
    if (ret == 0){ /* will not happen since no yspec ^*/
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    if (clicon_debug_xml(1, xt, "Transaction") < 0)
        goto done;
    if ((xn = xpath_first(xt, 0, "notification/controller-transaction")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if (tidstr0 && strcmp(tidstr0, tidstr) == 0 && match)
        *match = 1;
    if ((statusstr = xml_find_body(xn, "status")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no status");
        goto done;
    }
    if (status && strcmp(statusstr, "true") == 0)
        *status = 1;
    retval = 0;
 done:
    if (reply)
        free(reply);    
    if (xt)
        xml_free(xt);
    return retval;
}

#ifdef NOTUSED
/*! This is the callback used by transaction end notification
 *
 * param[in]  s    UNIX socket from backend  where message should be read
 * param[in]  arg  Registered transaction id string
 * @retval      0  OK
 * @retval     -1  Error
 * @see transaction_notification_poll
 */
static int
transaction_notification_cb(int   s, 
                            void *arg)
{
    int                retval = -1;
    int                eof = 0;
    char              *tidstr = (char*)arg;
    int                match = 0;
    int                status = 0;
    
    if (transaction_notification_handler(s, tidstr, &match, &status, &eof) < 0)
        goto done;
    if (eof){ /* XXX: This is never called since eof is return -1, but maybe be used later */
        clixon_event_unreg_fd(s, transaction_notification_cb);
        if (tidstr)
            free(tidstr);
        goto done;
    }
    if (match){
        fprintf(stdout, "Transaction %s completed with status: %d\n", tidstr, status);
        clixon_event_unreg_fd(s, transaction_notification_cb);            
        if (tidstr)
            free(tidstr);
    }
    retval = 0;
  done:
    return retval;
}
#endif

/*! Send transaction error to backend
 */
static int
send_transaction_error(clicon_handle h,
                       char         *tidstr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xtop = NULL;
    cxobj *xrpc;
    cxobj *xret = NULL;
    cxobj *xreply;
    cxobj *xerr;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<transaction-error xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "<origin>CLI</origin>");
    cprintf(cb, "<reason>Interrupted</reason>");
    cprintf(cb, "</transaction-error>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }

    retval = 0;
 done:
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Poll controller notification socket
 *
 * param[in]  h      Clicon handle
 * param[in]  tidstr Transaction identifier
 * @retval    0      OK
 * @retval   -1      Error
 * @see transaction_notification_cb
 */
static int
transaction_notification_poll(clicon_handle h,
                              char         *tidstr)
{
    int                retval = -1;
    int                eof = 0;
    int                s;
    int                match = 0;
    int                status = 0;

    clicon_debug(CLIXON_DBG_DEFAULT, "%s tid:%s", __FUNCTION__, tidstr);
    if ((s = clicon_option_int(h, "controller-transaction-notify-socket")) < 0){
        clicon_err(OE_EVENTS, 0, "controller-transaction-notify-socket is closed");
        goto done;
    }
    while (!match){
        if (transaction_notification_handler(s, tidstr, &match, &status, &eof) < 0){
            if (eof)
                goto done;
            /* Interpret as user stop transaction: abort transaction */
            if (send_transaction_error(h, tidstr) < 0)
                goto done;
            cligen_output(stderr, "Transaction aborted by user\n");
        }
    }
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DEFAULT, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Read the config of one or several devices
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : "pull", "push" / dryrun, commit
 * @retval    0      OK
 * @retval   -1      Error
 */
int
cli_sync_rpc(clixon_handle h, 
             cvec         *cvv, 
             cvec         *argv)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cg_var    *cv;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xreply;
    cxobj     *xerr;
    char      *op;
    char      *type;
    char      *name = "*";
    cxobj     *xid;
    char      *tidstr;
    uint64_t   tid = 0;

    if (argv == NULL || cvec_len(argv) != 2){
        clicon_err(OE_PLUGIN, EINVAL, "requires argument: <push><type>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if (strcmp(op, "push") != 0 && strcmp(op, "pull") != 0){
        clicon_err(OE_PLUGIN, EINVAL, "<push> argument is %s, expected \"push\" or \"pull\"", op);
        goto done;
    }
    if ((cv = cvec_i(argv, 1)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <type>");
        goto done;
    }
    type = cv_string_get(cv);
    if (strcmp(type, "dryrun") != 0 && strcmp(type, "commit") != 0){
        clicon_err(OE_PLUGIN, EINVAL, "<type> argument is %s, expected \"dryrun\" or \"commit\"", type);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<sync-%s xmlns=\"%s\">", op, CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    if (strcmp(type, "dryrun")==0)
        cprintf(cb, "<dryrun>true</dryrun>");
    cprintf(cb, "</sync-%s>", op);
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clicon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (tidstr && parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (transaction_notification_poll(h, tidstr) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Read the config of one or several devices
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_reconnect(clixon_handle h, 
             cvec         *cvv, 
             cvec         *argv)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cg_var    *cv;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xreply;
    cxobj     *xerr;
    char      *name = "*";

    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<reconnect xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "</reconnect>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Get device config
 *
 * @param[in]  h        Clixon handle 
 * @param[in]  name     Name of device
 * @param[in]  extended Extended name used as postfix: device-<name>-<extended>
 * @retval     0        OK
 * @retval    -1        Error
 */
static int
get_device_config(clicon_handle h,
                  char         *name,
                  char         *extended,
                  cxobj       **xp)
{
    int              retval = -1;
    cbuf            *cb = NULL;
    cxobj           *xrpc = NULL;
    cxobj           *xret = NULL;
    cxobj           *xerr = NULL;
    cxobj           *xc;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get-device-config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    if (extended)
        cprintf(cb, "<extended>%s</extended>", extended);
    cprintf(cb, "</get-device-config>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xrpc, NULL) < 0)
        goto done;
    /* Skip top-level */
    if (xml_rootchild(xrpc, 0, &xrpc) < 0)
        goto done;
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xc = xpath_first(xret, NULL, "rpc-reply/config/root")) == NULL){
        clicon_err(OE_CFG, 0, "No synced device config");
        goto done;
    }
    if (xp){
        xml_rm(xc);
        *xp = xc;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xrpc)
        xml_free(xrpc);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Compare device two dbs using XML. Write to file and run diff
 * @param[in]   h     Clicon handle
 * @param[in]   cvv  
 * @param[in]   argv  arg: 0 as xml, 1: as text
 * @retval      0     OK
 * @retval     -1     Error
 */
int
compare_device_dbs(clicon_handle h, 
                   cvec         *cvv, 
                   cvec         *argv)
{
    int              retval = -1;
    cxobj           *xc1 = NULL; /* running xml */
    cxobj           *xc2 = NULL; /* candidate xml */
    cxobj           *xret = NULL;
    cxobj           *xerr = NULL;
    enum format_enum format;
    cg_var          *cv;
    char            *name;
    cbuf            *cb = NULL;

    if (cvec_len(argv) > 1){
        clicon_err(OE_PLUGIN, EINVAL, "Requires 0 or 1 element. If given: astext flag 0|1");
        goto done;
    }
    if (cvec_len(argv) && cv_int32_get(cvec_i(argv, 0)) == 1)
        format = FORMAT_TEXT;
    else
        format = FORMAT_XML;
    if ((cv = cvec_find(cvv, "name")) == NULL)
        goto ok;
    name = cv_string_get(cv);
    if (clicon_rpc_get_config(h, NULL, "running", "/", NULL, NULL, &xret) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xc1 = xpath_first(xret, NULL, "devices/device/root")) == NULL){
        clicon_err(OE_CFG, 0, "No device config in running");
        goto done;
    }
    if (get_device_config(h, name, NULL, &xc2) < 0)
        goto done;
    if (clixon_compare_xmls(xc2, xc1, format, cligen_output) < 0) /* astext? */
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xc2)
        xml_free(xc2);
    return retval;
}

/*! Show controller device states
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_devices(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cvec              *nsc = NULL;
    cxobj             *xc;
    cxobj             *xerr;
    cbuf              *cb = NULL;
    cxobj             *xn = NULL; /* XML of senders */
    char              *name;
    char              *state;
    char              *timestamp;
    char              *logmsg;
    char              *pattern = NULL;
    cg_var            *cv;
    
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "co:devices", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
        goto done;
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get devices", NULL);
        goto done;
    }
    /* Change top frm "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "devices", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        cligen_output(stdout, "%-23s %-10s %-22s %-30s\n", "Name", "State", "Time", "Logmsg");
        cligen_output(stdout, "========================================================================\n");
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
            char *p;
            name = xml_find_body(xc, "name");
            if (pattern != NULL && fnmatch(pattern, name, 0) != 0)
                continue;
            cligen_output(stdout, "%-24s",  name);
            state = xml_find_body(xc, "conn-state");
            cligen_output(stdout, "%-11s",  state?state:"");
            if ((timestamp = xml_find_body(xc, "conn-state-timestamp")) != NULL){
                /* Remove 6 us digits */
                if ((p = rindex(timestamp, '.')) != NULL)
                    *p = '\0';
            }
            cligen_output(stdout, "%-23s", timestamp?timestamp:"");
            logmsg = xml_find_body(xc, "logmsg");
            cligen_output(stdout, "%-31s",  logmsg?logmsg:"");
            cligen_output(stdout, "\n");
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xn)
        xml_free(xn);
    if (cb)
        cbuf_free(cb);
    if (msg)
        free(msg);
    return retval;
}

/*! Send a sync-pull dryrun
 * @param[in]   h    Clixon handle
 * @param[out]  tid  Transaction id
 * @retval      0    OK
 * @retval     -1    Error
 */
static int
send_pull_dryrun(clicon_handle h,
                 char         *name,
                 char        **tidstrp)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xtop = NULL;
    cxobj     *xret = NULL;
    cxobj     *xrpc;
    cxobj     *xreply;
    cxobj     *xerr;
    cxobj     *xid;
    char      *tidstr;
    uint64_t   tid=0;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<sync-pull xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<dryrun>true</dryrun>>");
    cprintf(cb, "</sync-pull>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clicon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    if ((tidstr = strdup(xml_body(xid))) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    if (parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (tid == 0){
        clicon_err(OE_UNIX, errno, "Invalid tid = 0");
        goto done;
    }
    if (tidstrp && (*tidstrp = strdup(tidstr)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Show compare with device 
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv  arg: 0 as xml, 1: as text
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_sync_compare(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    int              retval = -1;
    char            *name = "*";
    cg_var          *cv;
    char            *tidstr = NULL;
    cxobj           *xc1 = NULL; /* running xml */
    cxobj           *xc2 = NULL; /* candidate xml */
    cxobj           *xret = NULL;
    cxobj           *xerr = NULL;
    enum format_enum format;
    
    if (cvec_len(argv) > 1){
        clicon_err(OE_PLUGIN, EINVAL, "Requires 0 or 1 element. If given: astext flag 0|1");
        goto done;
    }
    if (cvec_len(argv) && cv_int32_get(cvec_i(argv, 0)) == 1)
        format = FORMAT_TEXT;
    else
        format = FORMAT_XML;
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if (clicon_rpc_get_config(h, NULL, "running", "/", NULL, NULL, &xret) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xc1 = xpath_first(xret, NULL, "devices/device/root")) == NULL){
        clicon_err(OE_CFG, 0, "No device config in running");
        goto done;
    }
    if (send_pull_dryrun(h, name, &tidstr) < 0)
        goto done;
    /* XXX try ^C here */
    if (transaction_notification_poll(h, tidstr) < 0)
        goto done;
    if (get_device_config(h, name, "dryrun", &xc2) < 0)
        goto done;
    if (clixon_compare_xmls(xc2, xc1, format, cligen_output) < 0)
        goto done;
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (tidstr)
        free(tidstr);
    return retval;
}

/*! Check if device(s) is in sync
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv 
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_check_sync(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    return cli_sync_compare(h, cvv, argv);
}

int
cli_services_apply(clixon_handle h, 
                   cvec         *cvv, 
                   cvec         *argv)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xreply;
    cxobj     *xerr;
    cxobj     *xid;
    char      *tidstr;
    uint64_t   tid = 0;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<services-apply xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "</services-apply>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clicon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (tidstr && parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (0 && transaction_notification_poll(h, tidstr) < 0) // NOTYET
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/* Called when application is "started", (almost) all initialization is complete 
 *
 * Create a global transaction notification handler and socket
 * @param[in] h    Clixon handle
 */
int
controller_cli_start(clicon_handle h)
{
    int   retval = -1;
    cbuf *cb = NULL;    
    int   s;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (clicon_rpc_create_subscription(h, "controller-transaction", NULL, &s) < 0)
        goto done;
    if (clicon_option_int_set(h, "controller-transaction-notify-socket", s) < 0)
        goto done;
    clicon_debug(CLIXON_DBG_DEFAULT, "%s notification socket:%d", __FUNCTION__, s);
    retval = 0;
 done:
    return retval;
}

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init,
    controller_cli_start,
};

/*! CLI plugin initialization
 * @param[in]  h    Clixon handle
 * @retval     NULL Error with clicon_err set
 * @retval     api  Pointer to API struct
 * @retval      0    OK
 * @retval     -1    Error
 */
clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec);
    return &api;
}
