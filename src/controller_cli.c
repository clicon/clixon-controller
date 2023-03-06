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

/*! This is the callback used by transaction end notification
 *
 * param[in]  s    UNIX socket from backend  where message should be read
 * param[in]  arg  
 */
static int
transaction_notification_cb(int   s, 
                            void *arg)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    int                eof;
    cxobj             *xt = NULL;
    cxobj             *xn;
    char              *tid;
    char              *status;
    int                ret;
    
    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof) < 0)
        goto done;
    if (eof){
        clicon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
        close(s);
        errno = ESHUTDOWN;
        clixon_event_unreg_fd(s, transaction_notification_cb);
        goto done;
    }
    /* XXX pass yang_spec and use xerr*/
    if ((ret = clicon_msg_decode(reply, NULL, NULL, &xt, NULL)) < 0) 
        goto done;
    if (ret == 0){ /* will not happen since no yspec ^*/
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }    
    if (clicon_debug_xml(1, xt, "Transaction end") < 0)
        goto done;
    if ((xn = xpath_first(xt, 0, "notification/controller-transaction")) != NULL){
        tid = xml_find_body(xn, "tid");
        status = xml_find_body(xn, "status");
        if (tid && status)
            fprintf(stdout, "Transaction %s completed with status: %s\n", tid, status);
    }
    retval = 0;
  done:
    if (xt)
        xml_free(xt);
    if (reply)
        free(reply);
    return retval;
}

/*! Read the config of one or several devices
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : "pull", "push"
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
    char      *name = "*";
    cxobj     *xid;
    char      *idstr;
    uint64_t   tid = 0;
    int        s;

    if (argv == NULL || cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "requires argument: <push>");
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
    idstr = xml_body(xid);
    if (idstr && parse_uint64(idstr, &tid, NULL) <= 0)
        goto done;
    cbuf_reset(cb);
    cprintf(cb, "controller-transaction[tid='%" PRIu64 "']", tid);
    if (clicon_rpc_create_subscription(h, "controller-transaction", cbuf_get(cb), &s) < 0)
        goto done;
    if (cligen_regfd(s, transaction_notification_cb, NULL) < 0)
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
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
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

/*! Compare device two dbs using XML. Write to file and run diff
 * @param[in]   h     Clicon handle
 * @param[in]   cvv  
 * @param[in]   argv  arg: 0 as xml, 1: as text
 */
int
compare_device_dbs(clicon_handle h, 
                   cvec         *cvv, 
                   cvec         *argv)
{
    int              retval = -1;
    cxobj           *xc1 = NULL; /* running xml */
    cxobj           *xc2 = NULL; /* candidate xml */
    cxobj           *xret1 = NULL;
    cxobj           *xret2 = NULL;
    cxobj           *xrpc = NULL;
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
    if (clicon_rpc_get_config(h, NULL, "running", "/", NULL, NULL, &xret1) < 0)
        goto done;
    if ((xerr = xpath_first(xret1, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xc1 = xpath_first(xret1, NULL, "devices/device/root")) == NULL){
        clicon_err(OE_CFG, 0, "No device config in running");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get-device-sync-config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "</get-device-sync-config>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xrpc, NULL) < 0)
        goto done;
    /* Skip top-level */
    if (xml_rootchild(xrpc, 0, &xrpc) < 0)
        goto done;
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret2, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret2, NULL, "rpc-reply/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if ((xc2 = xpath_first(xret2, NULL, "rpc-reply/config/root")) == NULL){
        clicon_err(OE_CFG, 0, "No synced device config");
        goto done;
    }
    if (clixon_compare_xmls(xc2, xc1, format, cligen_output) < 0) /* astext? */
        goto done;
 ok:
    retval = 0;
  done:
    if (cb)
        cbuf_free(cb);
    if (xret1)
        xml_free(xret1);
    if (xret2)
        xml_free(xret2);
    if (xrpc)
        xml_free(xrpc);
    return retval;
}

/*! Show controller device states
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
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

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init, /* init */
};

/*! CLI plugin initialization
 * @param[in]  h    Clixon handle
 * @retval     NULL Error with clicon_err set
 * @retval     api  Pointer to API struct
 */
clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec);

    return &api;
}
