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
#include "controller_lib.h"

/*! Common transaction notification handling from both async and poll
 *
 * @param[in]   s       Notification socket
 * @param[in]   tidstr0 Transaction id string
 * @param[out]  match   Transaction id match
 * @param[out]  result  Transaction result
 * @param[out]  eof     EOF, socket closed
 * @param[out]  eof     EOF, socket closed
 * @retval      0       OK
 * @retval     -1       Fatal error
 */
static int
transaction_notification_handler(int                 s,
                                 char               *tidstr0,
                                 int                *match,
                                 transaction_result *resultp,
                                 int                *eof)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    cxobj             *xt = NULL;
    cxobj             *xn;
    int                ret;
    char              *tidstr;
    char              *reason = NULL;
    char              *resstr;
    transaction_result result;
    
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
    reason = xml_find_body(xn, "reason");
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if (tidstr0 && strcmp(tidstr0, tidstr) == 0 && match)
        *match = 1;
    if ((resstr = xml_find_body(xn, "result")) == NULL){
        clicon_err(OE_NETCONF, EFAULT, "Notification malformed: no result");
        goto done;
    }
    if ((result = transaction_result_str2int(resstr)) != TR_SUCCESS){
        fprintf(stderr, "Transaction %s failed %s\n", tidstr, reason?reason:"");
        goto ok; // error == ^C
    }
    if (result)
        *resultp = result;
 ok:
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
    transaction_result result = 0;
    
    if (transaction_notification_handler(s, tidstr, &match, &result, &eof) < 0)
        goto done;
    if (eof){ /* XXX: This is never called since eof is return -1, but maybe be used later */
        clixon_event_unreg_fd(s, transaction_notification_cb);
        if (tidstr)
            free(tidstr);
        goto done;
    }
    if (match){
        fprintf(stdout, "Transaction %s completed with result: %s\n", tidstr,
                transaction_result_int2str(result));
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
 *
 * @param[in] h      Clixon handle
 * @param[in] tidstr Transaction id
 * @retval    0      OK
 * @retval   -1      Error
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
    cprintf(cb, "<reason>Aborted by user</reason>");
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
 * param[out] result
 * @retval    0      OK
 * @retval   -1      Error
 * @see transaction_notification_cb
 */
static int
transaction_notification_poll(clicon_handle       h,
                              char               *tidstr,
                              transaction_result *result)
{
    int                retval = -1;
    int                eof = 0;
    int                s;
    int                match = 0;

    clicon_debug(CLIXON_DBG_DEFAULT, "%s tid:%s", __FUNCTION__, tidstr);
    if ((s = clicon_data_int_get(h, "controller-transaction-notify-socket")) < 0){
        clicon_err(OE_EVENTS, 0, "controller-transaction-notify-socket is closed");
        goto done;
    }
    while (!match){
        if (transaction_notification_handler(s, tidstr, &match, result, &eof) < 0){
            if (eof)
                goto done;
            /* Interpret as user stop transaction: abort transaction */
            if (send_transaction_error(h, tidstr) < 0)
                goto done;
            cligen_output(stderr, "Aborted by user\n");
            break;
        }
    }
    if (match){
        switch (*result){
        case TR_ERROR:
            cligen_output(stderr, "Error\n"); // XXX: Not recoverable??
            break;
        case TR_FAILED:
            cligen_output(stderr, "Failed\n");
            break;
        case TR_SUCCESS:
            break;
        }
    }
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DEFAULT, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Read(pull) the config of one or several devices.
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : replace/merge
 * @retval    0      OK
 * @retval   -1      Error
 */
int
cli_rpc_pull(clixon_handle h, 
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
    char      *tidstr;
    uint64_t   tid = 0;
    transaction_result result;

    if (argv == NULL || cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "requires argument: replace/merge");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if (strcmp(op, "replace") != 0 && strcmp(op, "merge") != 0){
        clicon_err(OE_PLUGIN, EINVAL, "pull <type> argument is %s, expected \"validate\" or \"commit\"", op);
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
    cprintf(cb, "<config-pull xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    if (strcmp(op, "merge") == 0)
        cprintf(cb, "<merge>true</merge>");
    cprintf(cb, "</config-pull>");
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
    if (transaction_notification_poll(h, tidstr, &result) < 0)
        goto done;
    if (result == TR_SUCCESS)
        cligen_output(stderr, "OK\n");
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
 * @param[in] argv : source:running/candidate, actions:NONE/CHANGE/FORCE, push:NONE/VALIDATE/COMMIT, 
 * @RETVAL    0      OK
 * @retval   -1      Error
 * @see controller-commit in clixon-controller.yang
 */
int
cli_rpc_controller_commit(clixon_handle h, 
                          cvec         *cvv, 
                          cvec         *argv)
{
    int          retval = -1;
    cbuf        *cb = NULL;
    cg_var      *cv;
    cxobj       *xtop = NULL;
    cxobj       *xrpc;
    cxobj       *xret = NULL;
    cxobj       *xreply;
    cxobj       *xerr;
    char        *push_type;
    char        *name = "*";
    cxobj       *xid;
    char        *tidstr;
    uint64_t     tid = 0;
    char        *actions_type;
    char        *source;
    cxobj       *xdiff;
    char        *diff;
    transaction_result result;

    if (argv == NULL || cvec_len(argv) != 3){
        clicon_err(OE_PLUGIN, EINVAL, "requires arguments: <datastore> <actions-type> <push-type>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <datastore>");
        goto done;
    }
    source = cv_string_get(cv);
    if ((cv = cvec_i(argv, 1)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <actions-type>");
        goto done;
    }
    actions_type = cv_string_get(cv);
    if (actions_type_str2int(actions_type) == -1){
        clicon_err(OE_PLUGIN, EINVAL, "<actions-type> argument is %s, expected NONE/CHANGE/FORCE", actions_type);
        goto done;
    }
    if ((cv = cvec_i(argv, 2)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <push-type>");
        goto done;
    }
    push_type = cv_string_get(cv);
    if (push_type_str2int(push_type) == -1){
        clicon_err(OE_PLUGIN, EINVAL, "<push-type> argument is %s, expected NONE/VALIDATE/COMMIT", push_type);
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
    cprintf(cb, "<controller-commit xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<device>%s</device>", name);
    cprintf(cb, "<push>%s</push>", push_type);
    cprintf(cb, "<actions>%s</actions>", actions_type);
    cprintf(cb, "<source>ds:%s</source>", source); /* Note add datastore prefix */
    cprintf(cb, "</controller-commit>");
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
    if (transaction_notification_poll(h, tidstr, &result) < 0)
        goto done;
    if (result != TR_SUCCESS)
        goto ok;
    /* Interpret actions and no push as diff */
    if (actions_type_str2int(actions_type) != AT_NONE &&
        push_type_str2int(push_type) == PT_NONE){ 
        cbuf_reset(cb);
        cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
        cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
        cprintf(cb, "<xpath>devices</xpath>");
        cprintf(cb, "<dsref1>ds:running</dsref1>");
        cprintf(cb, "<dsref2>actions</dsref2>");
        cprintf(cb, "</datastore-diff>");
        cprintf(cb, "</rpc>");
        if (xtop){
            xml_free(xtop);
            xtop = NULL;
        }
        if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
            goto done;
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
        if ((xdiff = xpath_first(xreply, NULL, "diff")) == NULL){
            clicon_err(OE_CFG, 0, "No returned diff");
            goto done;
        }
        if ((diff = xml_body(xdiff)) != NULL)
            cligen_output(stdout, "%s", diff);
    }
    cligen_output(stderr, "OK\n");
 ok:
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
 * @param[in] argv : 0: close, 1: open, 2: reconnect
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_connection_change(clixon_handle h, 
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
    char      *op;

    if (argv == NULL || cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "requires argument: <operation>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
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
    cprintf(cb, "<connection-change xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<operation>%s</operation>", op);
    cprintf(cb, "</connection-change>");
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

#ifdef NOTUSED
/*! Get device config
 *
 * @param[in]  h           Clixon handle 
 * @param[in]  name        Name of device
 * @param[in]  config_type variant, eg RUNNING/TRANSIENT/SYNCED
 * @param[out] xp          XML tree reply
 * @retval     0           OK
 * @retval    -1           Error
 */
static int
get_device_config(clicon_handle h,
                  char         *name,
                  char         *config_type,
                  cxobj       **xp)
{
    int              retval = -1;
    cbuf            *cb = NULL;
    cxobj           *xrpc = NULL;
    cxobj           *xret = NULL;
    cxobj           *xerr = NULL;

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
    cprintf(cb, "<config-type>%s</config-type>", config_type);
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
    if (xp){
        *xp = xret;
        xret = NULL;
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
#endif

/*! Show controller device states
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_devices(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    int                retval = -1;
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
    int                detail = 0;
    
    if (argv != NULL && cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "optional argument: <detail>");
        goto done;
    }
    if (cvec_len(argv) == 1){
        if ((cv = cvec_i(argv, 0)) == NULL){
            clicon_err(OE_PLUGIN, 0, "Error when accessing argument <detail>");
            goto done;
        }
        detail = strcmp(cv_string_get(cv),"detail")==0;
    }
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
    /* Change top from "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "devices", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        if (detail){
            if (clixon_xml2file(stdout, xn, 0, 1, NULL, cligen_output, 0, 1) < 0)
                goto done;
        }
        else {
            cligen_output(stdout, "%-23s %-10s %-22s %-30s\n", "Name", "State", "Time", "Logmsg");
            cligen_output(stdout, "=======================================================================================\n");
            xc = NULL;
            while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
                char *p;
                char logstr[30];

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
                if ((logmsg = xml_find_body(xc, "logmsg")) != NULL){
                    strncpy(logstr, logmsg, 30);
                    logstr[29] = '\0';
                    cligen_output(stdout, "%s",  logstr);
                }
                cligen_output(stdout, "\n");
            }
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
    return retval;
}

/*! Show controller device states
 * @param[in] h
 * @param[in] cvv  
 * @param[in] argv : "last" or "all"
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_transactions(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int                retval = -1;
    cvec              *nsc = NULL;
    cxobj             *xc;
    cxobj             *xerr;
    cbuf              *cb = NULL;
    cxobj             *xn = NULL; /* XML of senders */
    cg_var            *cv;
    int                all = 0;
    
    if (argv == NULL || cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "requires argument: <operation>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clicon_err(OE_PLUGIN, 0, "Error when accessing argument <all>");
        goto done;
    }
    all = strcmp(cv_string_get(cv), "all")==0;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "co:transactions", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
        goto done;
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get devices", NULL);
        goto done;
    }
    /* Change top from "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "transactions", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        if (all){
            xn = xc;
            xc = NULL;
            while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
                if (clixon_xml2file(stdout, xc, 0, 1, NULL, cligen_output, 0, 1) < 0)
                    goto done;
            }
        }
        else{
            if ((xc = xml_child_i(xn, xml_child_nr(xn) - 1)) != NULL){
                if (clixon_xml2file(stdout, xc, 0, 1, NULL, cligen_output, 0, 1) < 0)
                    goto done;
            }
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
    return retval;
}

/*! Send a pull transient
 *
 * @param[in]   h    Clixon handle
 * @param[out]  tid  Transaction id
 * @retval      0    OK
 * @retval     -1    Error
 */
static int
send_pull_transient(clicon_handle h,
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
    cprintf(cb, "<config-pull xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<transient>true</transient>>");
    cprintf(cb, "</config-pull>");
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

/*! Compare device config types: running with last saved synced or current device (transient)
 *
 * @param[in]   h       Clicon handle
 * @param[in]   cvv     name: device pattern
 * @param[in]   argv    <format>        "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @param[in]   dt1     First device config config 
 * @param[in]   dt2     Second device config config
 * @param[out]  diffp   Allocated diff string
 * @retval      0       OK
 * @retval     -1       Error
 */
int
compare_device_config_type(clicon_handle      h, 
                           cvec              *cvv, 
                           cvec              *argv,
                           device_config_type dt1,
                           device_config_type dt2,
                           char             **diffp)
{
    int              retval = -1;
    enum format_enum format;
    cg_var          *cv;
    char            *pattern = "*";
    char            *tidstr = NULL;
    char            *formatstr;
    cxobj           *xtop = NULL;
    cxobj           *xret = NULL;
    cxobj           *xrpc;
    cxobj           *xreply;
    cxobj           *xerr;
    cxobj           *xdiff;
    cbuf            *cb = NULL;
    char            *device_type = NULL;
    char            *diff;
    transaction_result result;
    
    if (cvec_len(argv) > 1){
        clicon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <format>]", cvec_len(argv));
        goto done;
    }
    cv = cvec_i(argv, 0);
    formatstr = cv_string_get(cv);
    if ((int)(format = format_str2int(formatstr)) < 0){
        clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    /* If remote, start with requesting it asynchrously */
    if (dt1 == DT_TRANSIENT || dt2 == DT_TRANSIENT){
        /* Send pull <transient> */
        if (send_pull_transient(h, pattern, &tidstr) < 0)
            goto done;
        /* Wait to complete transaction try ^C here */
        if (transaction_notification_poll(h, tidstr, &result) < 0)
            goto done;
        if (result == TR_SUCCESS)
            goto ok;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<xpath>config</xpath>");
    device_type = device_config_type_int2str(dt1);
    cprintf(cb, "<devname>%s</devname>", pattern);
    cprintf(cb, "<config-type1>%s</config-type1>", device_type);
    device_type = device_config_type_int2str(dt2);
    cprintf(cb, "<config-type2>%s</config-type2>", device_type);
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (xtop){
        xml_free(xtop);
        xtop = NULL;
    }
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
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
    if ((xdiff = xpath_first(xreply, NULL, "diff")) == NULL){
        clicon_err(OE_CFG, 0, "No returned diff");
        goto done;
    }
    if ((diff = xml_body(xdiff)) != NULL && diffp){
        if ((*diffp = strdup(diff)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
    }
 ok:
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compare device dbs: running with (last) synced db
 *
 * @param[in]   h     Clicon handle
 * @param[in]   cvv  
 * @param[in]   argv  arg: 0 as xml, 1: as text
 * @retval      0     OK
 * @retval     -1     Error
 */
int
compare_device_db_sync(clicon_handle h, 
                       cvec         *cvv, 
                       cvec         *argv)
{
    int   retval = -1;
    char *diff = NULL;

    if (compare_device_config_type(h, cvv, argv, DT_SYNCED, DT_RUNNING, &diff) < 0)
        goto done;
    if (diff)
        cligen_output(stdout, "%s", diff);
    retval = 0;
 done:
    if (diff)
        free(diff);
    return retval;
}

/*! Compare device dbs: running with current device (transient)
 *
 * @param[in] h     Clicon handle
 * @param[in] cvv  : name pattern or NULL
 * @param[in] argv  arg: 0 as xml, 1: as text
 * @retval    0     OK
 * @retval   -1     Error
 */
int
compare_device_db_dev(clicon_handle h, 
                      cvec         *cvv,
                      cvec         *argv)
{
    int   retval = -1;
    char *diff = NULL;

    if (compare_device_config_type(h, cvv, argv, DT_TRANSIENT, DT_RUNNING, &diff) < 0)
        goto done;
    if (diff)
        cligen_output(stdout, "%s", diff);
    retval = 0;
 done:
    if (diff)
        free(diff);
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
check_device_db(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    int   retval = -1;
    char *diff = NULL;
    
    if (compare_device_config_type(h, cvv, argv, DT_RUNNING, DT_TRANSIENT, &diff) < 0)
        goto done;
    if (diff && strlen(diff))
        cligen_output(stdout, "device out-of-sync\n");
    else
        cligen_output(stdout, "OK\n");
    retval = 0;
 done:
    if (diff)
        free(diff);
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
    if (clicon_data_int_set(h, "controller-transaction-notify-socket", s) < 0)
        goto done;
    clicon_debug(CLIXON_DBG_DEFAULT, "%s notification socket:%d", __FUNCTION__, s);
    retval = 0;
 done:
    return retval;
}

/* Called just before plugin unloaded. 
 *
 * @param[in] h    Clixon handle
 */
int
controller_cli_exit(clicon_handle h)
{
    int                retval = -1;
    int                s;
    cbuf              *cb = NULL;
    char              *username;
    struct clicon_msg *msg = NULL;
    uint32_t           session_id = 0;

    if ((s = clicon_data_int_get(h, "controller-transaction-notify-socket")) > 0){
        /* Inline of clicon_rpc_close_session() w other socket */
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }
        clicon_session_id_get(h, &session_id);
        cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
        cprintf(cb, " xmlns:%s=\"%s\"", NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
        if ((username = clicon_username_get(h)) != NULL){
            cprintf(cb, " %s:username=\"%s\"", CLIXON_LIB_PREFIX, username);
            cprintf(cb, " xmlns:%s=\"%s\"", CLIXON_LIB_PREFIX, CLIXON_LIB_NS);
        }
        cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR); /* XXX: use incrementing sequence */
        cprintf(cb, ">");
        cprintf(cb, "<close-session/>");
        cprintf(cb, "</rpc>");
        if ((msg = clicon_msg_encode(session_id, "%s", cbuf_get(cb))) == NULL)
            goto done;
        if (clicon_rpc_msg(h, msg, NULL) < 0)
            goto done;        
        close(s);
        clicon_data_int_del(h, "controller-transaction-notify-socket");
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (msg)
        free(msg);
    return retval;
}

/*! Send get yanglib of mountpount to backend
 *
 * @param[in]  h       Clixon handle
 * @param[in]  devname Device name 
 * @param[out] yanglib XML yang-library module-set, claler needs to deallocate this
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_get_yanglib_mount(clicon_handle h,
                      char         *devname,
                      cxobj       **yanglib)
{
    int retval = -1;

    cbuf  *cb = NULL;
    cxobj *xtop = NULL;
    cxobj *xrpc;
    cxobj *xret = NULL;
    cxobj *xreply;
    cxobj *xerr;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get>");
    cprintf(cb, "<filter type=\"xpath\" select=\"ctrl:devices/ctrl:device[ctrl:name='%s']/ctrl:config/yanglib:yang-library/yanglib:module-set\" xmlns:ctrl=\"%s\" xmlns:yanglib=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">",
            devname,
            CONTROLLER_NAMESPACE);
    cprintf(cb, "</filter>");
    cprintf(cb, "</get>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    clicon_debug_xml(1, xret, "get module-set");
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clicon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_netconf_error(xerr, "Get configuration", NULL);
        goto done;
    }
    if (yanglib){
        *yanglib = xpath_first(xreply, 0, "data/devices/device/config/yang-library");
        xml_rm(*yanglib);
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

/*! There is not auto cligen tree "treename", create it
 *
 * 1. Check if yang controller extension/unknown mount-pint exists (yu)
 * 2. Create xpath to specific mountpoint given by devname
 * 3. Check if yspec associated to that mountpoint exists
 * 4. Get yang specs of mountpoint from controller
 * 5. Parse YANGs locally from the yang specs
 * 6. Generate auto-cligen tree from the specs 
 * @param[in]  h         Clicon handle
 * @param[in]  debname   Device name
 * @param[in]  treename  Autocli treename
 * @param[out] yspec1p   yang spec
 * @retval     0         Ok
 * @retval    -1         Error
 */
static int
create_autocli_mount_tree(clicon_handle h,
                          char         *devname,
                          char         *treename,
                          yang_stmt   **yspec1p)
{
    int        retval = -1;
    yang_stmt *yu;
    cbuf      *cb = NULL;
    char      *xpath;
    yang_stmt *yspec0;
    yang_stmt *yspec1 = NULL;
    yang_stmt *ymod;
    cxobj     *yanglib = NULL;
    int        ret;

    clicon_debug(1, "%s", __FUNCTION__);
    yspec0 = clicon_dbspec_yang(h);
    if ((ymod = yang_find(yspec0, Y_MODULE, "clixon-controller")) == NULL){
        clicon_err(OE_YANG, 0, "module clixon-controller not found");
        goto done;
    }
    /* 1. Check if yang controller extension/unknwon mount-pint exists (yu) */
    if (yang_path_arg(ymod, "/devices/device/config", &yu) < 0)
        goto done;
    if (yu == NULL){
        clicon_err(OE_YANG, 0, "Mountpoint devices/device/config not found");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* 2. Create xpath to specific mountpoint given by devname */
    cprintf(cb, "/ctrl:devices/ctrl:device[ctrl:name='%s']/ctrl:config", devname);
    xpath = cbuf_get(cb);
    /* 3. Check if yspec associated to that mountpoint exists */
    if (yang_mount_get(yu, xpath, &yspec1) < 0)
        goto done;
    if (yspec1 == NULL){
        if ((yspec1 = yspec_new()) == NULL)
            goto done;
        /* 4. Get yang specs of mountpoint from controller */
        if (rpc_get_yanglib_mount(h, devname, &yanglib) < 0)
            goto done;

#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
        /* 5. Parse YANGs locally from the yang specs 
           Added extra JUNOS patch to mod YANGs */
        if ((ret = yang_lib2yspec_junos_patch(h, yanglib, yspec1)) < 0)
            goto done;
#else
        /* 5. Parse YANGs locally from the yang specs */
        if ((ret = yang_lib2yspec(h, yanglib, yspec1)) < 0)
            goto done;
#endif
        if (ret == 0)
            goto done;
        if (yang_mount_set(yu, xpath, yspec1) < 0)
            goto done;
    }
    if (yspec1p)
        *yspec1p = yspec1;
    retval = 0;
 done:
    if (yanglib)
        xml_free(yanglib);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! CLIgen wrap function for making treeref lookup
 *
 * This adds an indirection based on name and context
 * @param[in]  h     CLIgen handle
 * @param[in]  name  Base tree name
 * @param[in]  cvt   Tokenized string: vector of tokens
 * @param[in]  arg   Argument given when registering wrap function (maybe not needed?)
 * @param[out] namep New (malloced) name
 * @retval     1     New malloced name in namep
 * @retval     0     No wrapper, use existing
 * @retval    -1     Error
 */
static int
controller_cligen_treeref_wrap(cligen_handle ch,
                               char         *name,
                               cvec         *cvt,
                               void         *arg,
                               char        **namep)
{
    int           retval = -1;
    cg_var       *cv;
    cg_var       *cvdev = NULL;
    char         *devname;
    char         *treename2;
    cbuf         *cb = NULL;
    yang_stmt    *yspec1 = NULL;
    clicon_handle h;
    cvec         *cvv_edit;

    h = cligen_userhandle(ch);
    if (strcmp(name, "mountpoint") != 0)
        goto ok;
    cvv_edit = clicon_data_cvec_get(h, "cli-edit-cvv");
    /* Ad-hoc: find "name" variable in edit mode cvv, else "device" token */
    if (cvv_edit && (cvdev = cvec_find(cvv_edit, "name")) != NULL)
        ;
    else{
        cv = NULL;
        while ((cv = cvec_each(cvt, cv)) != NULL){
            if (strcmp(cv_string_get(cv), "device") == 0)
                break;
        }
        if (cv == NULL)
            goto ok;
        if ((cvdev = cvec_next(cvt, cv)) == NULL)
            goto ok;
    }
    if ((devname = cv_string_get(cvdev)) == NULL)
        goto ok;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "mountpoint-%s", devname);
    treename2 = cbuf_get(cb);
    /* Does this tree exist? */
    if (cligen_ph_find(ch, treename2) == NULL){
        if (create_autocli_mount_tree(h, devname, treename2, &yspec1) < 0)
            goto done;
        if (yspec1 == NULL){
            clicon_err(OE_YANG, 0, "No yang spec");
            goto done;
        }
        /* Generate auto-cligen tree from the specs */
        if (yang2cli_yspec(h, yspec1, treename2, 0) < 0)
            goto done;
        /* Sanity */
        if (cligen_ph_find(ch, treename2) == NULL){
            clicon_err(OE_YANG, 0, "autocli should have  been generated but is not?");
            goto done;
        }
    }
    if (namep &&
        (*namep = strdup(treename2)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 ok:
    retval = 0;
    goto done;
}

/*! YANG schema mount
 *
 * Given an XML mount-point xt, return XML yang-lib modules-set
 * Return yanglib as XML tree on the RFC8525 form: 
 *   <yang-library>
 *      <module-set>
 *         <module>...</module>
 *         ...
 *      </module-set>
 *   </yang-library>
 * Get the schema-list for this device from the backend
 * @param[in]  h       Clixon handle
 * @param[in]  xt      XML mount-point in XML tree
 * @param[out] config  If '0' all data nodes in the mounted schema are read-only
 * @param[out] vallevel Do or dont do full RFC 7950 validation
 * @param[out] yanglib XML yang-lib module-set tree. Freed by caller.
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC 8528 (schema-mount) and RFC 8525 (yang-lib)
 * @see device_send_get_schema_list/device_state_recv_schema_list Backend fns for send/rcv
 * XXX 1. Recursion in clicon_rpc_get
 * XXX 2. Cache somewhere?
 */
int
controller_cli_yang_mount(clicon_handle   h,
                          cxobj          *xm,
                          int            *config,
                          validate_level *vl, 
                          cxobj         **yanglib)
{
    int    retval = -1;
    cxobj *xt = NULL;
    cxobj *xerr = NULL;
    cxobj *xmodset;
    cvec  *nsc = NULL;
    char  *xpath = NULL;
    cbuf  *cb = NULL;    
    char  *str;
    static int recursion = 0; /* clicon_rpc_get() -> bind back to here */

    if (recursion)
        goto ok;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if (xml_nsctx_node(xm, &nsc) < 0)
        goto done;
    if (xml2xpath(xm, nsc, 1, 1, &xpath) < 0)
        goto done;
    /* xm can be rooted somewhere else than "/devices" , such as /rpc-reply */
    if ((str = strstr(xpath, "/devices/device")) == NULL)
        goto ok;
    if (xml_nsctx_add(nsc, "yanglib", "urn:ietf:params:xml:ns:yang:ietf-yang-library") < 0)
        goto done;
    cprintf(cb, "%s/yanglib:yang-library/yanglib:module-set[yanglib:name='mount']", str);
    recursion++;
    if (clicon_rpc_get(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", &xt) < 0){
        recursion--;
        goto done;
    }
    recursion--;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "clicon_rpc_get", NULL);
        goto done;
    }
    if ((xmodset = xpath_first(xt, nsc, "%s", cbuf_get(cb))) == NULL)
        goto ok;
    cbuf_reset(cb);
    cprintf(cb, "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"/>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, yanglib, NULL) < 0)
        goto done;
    if (xml_addsub(*yanglib, xmodset) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xpath)
        free(xpath);
    if (xt)
        xml_free(xt);
    if (xerr)
        xml_free(xerr);
    if (nsc)
        xml_nsctx_free(nsc);
    return retval;
}

/*! YANG module patch
 *
 * Given a parsed YANG module, give the ability to patch it before import recursion,
 * grouping/uses checks, augments, etc
 * Can be useful if YANG in some way needs modification.
 * Deviations could be used as alternative (probably better)
 * @param[in]  h       Clixon handle
 * @param[in]  ymod    YANG module
 * @retval     0       OK
 * @retval    -1       Error
 */
int
controller_cli_yang_patch(clicon_handle h,
                      yang_stmt    *ymod)
{
    int         retval = -1;
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    char       *modname;
    yang_stmt  *ygr;
    char       *arg = NULL;

    if (ymod == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "ymod is NULL");
        goto done;
    }
    modname = yang_argument_get(ymod);
    if (strncmp(modname, "junos-rpc", strlen("junos-rpc")) == 0){
        if (yang_find(ymod, Y_GROUPING, "command-forwarding") == NULL){
            if ((ygr = ys_new(Y_GROUPING)) == NULL)
                goto done;
            if ((arg = strdup("command-forwarding")) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (yang_argument_set(ygr, arg) < 0)
                goto done;
            if (yn_insert(ymod, ygr) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
#else
    retval = 0;
#endif
    return retval;
}

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init,
    controller_cli_start,
    controller_cli_exit,
    .ca_yang_mount   = controller_cli_yang_mount,
    .ca_yang_patch   = controller_cli_yang_patch,
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
    /* Register treeref wrap function */
    if (clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        cligen_tree_resolve_wrapper_set(cli_cligen(h), controller_cligen_treeref_wrap, NULL);
    }
    return &api;
}
