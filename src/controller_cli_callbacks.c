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
  * These functions are called from CLISPECs, ie controller_operation.cli or /_configure.cli
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

/* Clixon */
#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_cli.h>
#include <clixon/cli_generate.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_cli_callbacks.h"

/*!
 *
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
cli_apipath(clixon_handle h,
            cvec         *cvv,
            char         *mtpoint,
            char         *api_path_fmt,
            int          *cvvi,
            char        **api_path)
{
    int        retval = -1;
    char      *api_path_fmt01 = NULL;
    yang_stmt *yspec0;

    if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    if (mtpoint){
        /* Get and combined api-path01 */
        if (mtpoint_paths(yspec0, mtpoint, api_path_fmt, &api_path_fmt01) < 0)
            goto done;
        if (api_path_fmt2api_path(api_path_fmt01, cvv, yspec0, api_path, cvvi) < 0)
            goto done;
    }
    else{
        if (api_path_fmt2api_path(api_path_fmt, cvv, yspec0, api_path, cvvi) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (api_path_fmt01)
        free(api_path_fmt01);
    return retval;
}

/*!
 *
 * @param[in]  h            Clixon handle
 * @param[in]  cvv          Vector of cli string and instantiated variable
 * @param[in]  mtpoint      Moint-point
 * @param[in]  api_path_fmt APi-path meta-format
 * @param[out] xpath        XPath (use free() to deallocate)
 * @param[out] nsc          Namespace context of xpath (free w cvec_free)
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
cli_apipath2xpath(clixon_handle h,
                  cvec         *cvv,
                  char         *mtpoint,
                  char         *api_path_fmt,
                  char        **xpath,
                  cvec        **nsc)
{
    int        retval = -1;
    yang_stmt *yspec0;
    char      *api_path = NULL;
    int        cvvi = 0;

    if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
        goto done;
    if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    if (api_path2xpath(api_path, yspec0, xpath, nsc, NULL) < 0)
        goto done;
    if (*xpath == NULL){
        clixon_err(OE_FATAL, 0, "Invalid api-path: %s", api_path);
        goto done;
    }
    retval = 0;
 done:
    if (api_path)
        free(api_path);
    return retval;
}

/*! Send get yanglib of all mountpoints to backend and return matching devices/yang-libs
 *
 * @param[in]  h         Clixon handle
 * @param[in]  pattern   Name glob pattern
 * @param[in]  single    pattern is a single device that can be used in an xpath
 * @param[in]  yanglib   0: only device name, 1: Also include config/yang-librarylib
 * @param[out] xdevsp    XML on the form <data><devices><device><name>x</name>...</data>
 * @retval     0         OK
 * @retval    -1         Error
 * @note due to https://github.com/clicon/clixon/issues/485, there is some complex filtering
 * in the yanglib case marked with "485" below
 */
int
rpc_get_yanglib_mount_match(clixon_handle h,
                            char         *pattern,
                            int           single,
                            int           yanglib,
                            cxobj       **xdevsp)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xdevs = NULL;
    cxobj     *xdev;
    cxobj     *xy;
    char      *devname;
    cxobj     *xret = NULL;
    cxobj     *xerr;
    cxobj     *xp;
    yang_stmt *yspec;
    int        ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get");
    if (yanglib) // 485
        cprintf(cb, " %s:depth=\"%d\" xmlns:%s=\"%s\"",
                CLIXON_LIB_PREFIX,
                8,
                CLIXON_LIB_PREFIX, CLIXON_LIB_NS);
    cprintf(cb, ">");
    cprintf(cb, "<filter type=\"xpath\"");
    cprintf(cb, " select=\"/ctrl:devices/ctrl:device");
    if (single)
        cprintf(cb, "[ctrl:name='%s']", pattern);
    if (yanglib){
        cprintf(cb, "/ctrl:config");
#if 0 // 485
        cprintf(cb, "/yanglib:yang-library");
#endif
    }
    else
        cprintf(cb, "/ctrl:name");
    cprintf(cb, "\"");
    cprintf(cb, " xmlns:ctrl=\"%s\" xmlns:yanglib=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">",
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
    if ((xerr = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xdevs = xpath_first(xret, NULL, "rpc-reply/data/devices")) != NULL){
        if ((yspec = clicon_dbspec_yang(h)) == NULL){
            clixon_err(OE_FATAL, 0, "No DB_SPEC");
            goto done;
        }
        xdev = NULL;
        while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
            if ((devname = xml_find_body(xdev, "name")) == NULL ||
                fnmatch(pattern, devname, 0) == 0){ /* Match */
                if (yanglib &&
                    (xy = xpath_first(xdev, 0, "config/yang-library")) != NULL){
                    xml_flag_set(xml_find(xdev, "name"), XML_FLAG_MARK);
                    xml_flag_set(xy, XML_FLAG_MARK);
                }
                else
                    xml_flag_set(xdev, XML_FLAG_MARK);
            }
        }
        /* 2. Remove all unmarked nodes, ie non-matching nodes */
        if (xml_tree_prune_flagged_sub(xdevs, XML_FLAG_MARK, 1, NULL) < 0)
            goto done;
        /* Populate XML with Yang spec. Binding is done in clicon_rpc_netconf of RPC only
         * where <data> is ANYDATA
         */
        if ((ret = xml_bind_yang0(h, xdevs, YB_MODULE, yspec, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_XML, 0, xerr, "Get devices config");
            goto done;
        }
        /* Double check that there is at least one device */
        if (xdevsp && xpath_first(xdevs, NULL, "device/name") != NULL &&
            (xp = xml_parent(xdevs))){
            xml_rm(xp);
            xml_spec_set(xp, NULL);
            *xdevsp = xp;
        }
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

/*! Specialization of clixon cli_show_auto to handle device globs
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector of function arguments
 * @retval     0    OK
 * @retval    -1    Error
 * @see cli_show_auto  Original function for description, arguments etc
 * @see cli_dbxml_devs Similar controller handling
 */
int
cli_show_auto_devs(clixon_handle h,
                   cvec         *cvv,
                   cvec         *argv)
{
    int              retval = -1;
    char            *dbname;
    enum format_enum format = FORMAT_XML;
    cvec            *nsc = NULL;
    int              pretty = 1;
    char            *prepend = NULL;
    int              state = 0;
    char            *withdefault = NULL; /* RFC 6243 modes */
    char            *extdefault = NULL; /* with extended tagged modes */
    int              argc = 0;
    char            *xpath = NULL;
    char            *api_path_fmt;  /* xml key format */
    char            *str;
    char            *mtpoint = NULL;
    cg_var          *cv;
    char            *pattern;
    cxobj           *xdevs = NULL;
    cxobj           *xdev;
    char            *devname;
    int              devices = 0;
    cbuf            *api_path_fmt_cb = NULL;    /* xml key format */
    int              i;
    int              fromroot = 0;

    if (cvec_len(argv) < 2){
        clixon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected:: <api-path-fmt>* <datastore> [<format> <pretty> <state> <default> <prepend>]", cvec_len(argv));
        goto done;
    }
    if ((api_path_fmt_cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Concatenate all argv strings to a single string
     * Variant of cvec_concat_cb() where api-path-fmt may be interleaved with mtpoint,
     * eg /api-path-fmt2 mtpoint /api-path-fmt1 /api-path-fmt0
     */
    for (i=cvec_len(argv)-1; i>=0; i--){
        cv = cvec_i(argv, i);
        if ((str = cv_string_get(cv)) == NULL)
            continue;
        if (str && strncmp(str, "mtpoint:", strlen("mtpoint:")) == 0){
            mtpoint = str + strlen("mtpoint:");
            devices = strstr(mtpoint, "/ctrl:devices") != NULL;
            argc++;
            continue;
        }
        if (str[0] != '/')
            continue;
        argc++;
        cprintf(api_path_fmt_cb, "%s", str);
    }
    api_path_fmt = cbuf_get(api_path_fmt_cb);
    if (mtpoint == NULL)
        devices = strstr(api_path_fmt, "/clixon-controller:devices") != NULL;
    if (cvec_len(argv) <= argc){
        clixon_err(OE_PLUGIN, EINVAL, "Missing: <datastore>");
        goto done;
    }
    dbname = cv_string_get(cvec_i(argv, argc++));
    if (cvec_len(argv) > argc)
        if (cli_show_option_format(h, argv, argc++, &format) < 0)
            goto done;
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &pretty) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &state) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_withdefault(argv, argc++, &withdefault, &extdefault) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        prepend = cv_string_get(cvec_i(argv, argc++));
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &fromroot) < 0)
            goto done;
    }
    /* ad-hoc if devices device <name> is selected */
    if (devices && (cv = cvec_find(cvv, "name")) != NULL){
        pattern = cv_string_get(cv);
        if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs) < 0)
            goto done;
        if (xdevs == NULL){
            if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
                goto done;
            if (cli_show_common(h, dbname, format, pretty, state,
                                withdefault, extdefault,
                                prepend, xpath, fromroot, nsc, 0) < 0)
                goto done;
        }
        else {
            xdev = NULL;
            while ((xdev = xml_child_each(xml_find(xdevs, "devices"), xdev, CX_ELMNT)) != NULL) {
                if ((devname = xml_find_body(xdev, "name")) == NULL)
                    continue;
                cv_string_set(cv, devname); /* replace name */
                /* aggregate to composite xpath */
                if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
                    goto done;
                /* Meta-info /comment need to follow language, but only for XML here */
                if (format == FORMAT_XML)
                    cligen_output(stdout, "<!-- %s: -->\n", devname);
                else
                    cligen_output(stdout, "%s:", devname);
                if (cli_show_common(h, dbname, format, pretty, state,
                                    withdefault, extdefault,
                                    prepend, xpath, fromroot, nsc, 0) < 0)
                    goto done;
                if (xpath){
                    free(xpath);
                    xpath = NULL;
                }
                if (nsc){
                    cvec_free(nsc);
                    nsc = NULL;
                }
            }
        }
    }
    else {
        if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
            goto done;
        if (cli_show_common(h, dbname, format, pretty, state,
                            withdefault, extdefault,
                            prepend, xpath, fromroot, nsc, 0) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (api_path_fmt_cb)
        cbuf_free(api_path_fmt_cb);
    if (xdevs)
        xml_free(xdevs);
    if (nsc)
        cvec_free(nsc);
    if (xpath)
        free(xpath);
    return retval;
}

/*! Common transaction notification handling from both async and poll
 *
 * @param[in]   h       CLixon handle
 * @param[in]   s       Notification socket
 * @param[in]   tidstr0 Transaction id string
 * @param[out]  match   Transaction id match
 * @param[out]  result  Transaction result
 * @param[out]  eof     EOF, socket closed
 * @retval      0       OK
 * @retval     -1       Fatal error
 */
static int
transaction_notification_handler(clixon_handle       h,
                                 int                 s,
                                 char               *tidstr0,
                                 int                *match,
                                 transaction_result *resultp,
                                 int                *eof)
{
    int                retval = -1;
    cxobj             *xt = NULL;
    cxobj             *xn;
    char              *tidstr;
    char              *reason = NULL;
    char              *resstr;
    transaction_result result;
    void              *wh = NULL;
    cbuf              *cb = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "tid:%s", tidstr0);
    /* Need to set "intr" to enable ^C */
    if (clixon_resource_check(h, &wh, tidstr0, __FUNCTION__) < 0)
        goto done;
    if (clixon_msg_rcv11(s, NULL, 1, &cb, eof) < 0){
        clixon_resource_check(h, &wh, tidstr0, __FUNCTION__);
        goto done;
    }
    if (clixon_resource_check(h, &wh, tidstr0, __FUNCTION__) < 0)
        goto done;
    if (*eof){
        clixon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
        close(s);
        goto done; /* Fatal, or possibly cli may reconnect */
    }
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xt, NULL) < 0)
        goto done;
    clixon_debug_xml(CLIXON_DBG_CTRL, xt, "Transaction");
    if ((xn = xpath_first(xt, 0, "notification/controller-transaction")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    reason = xml_find_body(xn, "reason");
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if (tidstr0 && strcmp(tidstr0, tidstr) == 0 && match)
        *match = 1;
    if ((resstr = xml_find_body(xn, "result")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no result");
        goto done;
    }
    if ((result = transaction_result_str2int(resstr)) != TR_SUCCESS){
        if((clixon_logflags_get() | CLIXON_LOG_STDERR) == 0x0)
	  cligen_output(stderr, "%s: pid: %u Transaction %s failed: %s\n",
		  __FUNCTION__, getpid(), tidstr, reason?reason:"no reason");

      clixon_log(h, LOG_NOTICE, "%s: pid: %u Transaction %s failed: %s",
		 __FUNCTION__, getpid(), tidstr, reason?reason:"no reason");
    }
    if (result)
        *resultp = result;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL, "%d", retval);
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Send transaction error to backend
 *
 * @param[in] h      Clixon handle
 * @param[in] tidstr Transaction id
 * @retval    0      OK
 * @retval   -1      Error
 */
static int
send_transaction_error(clixon_handle h,
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
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
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
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
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
 * param[in]  h      Clixon handle
 * param[in]  tidstr Transaction identifier
 * param[out] result
 * @retval    0      OK
 * @retval   -1      Error
 * @see transaction_notification_cb
 */
static int
transaction_notification_poll(clixon_handle       h,
                              char               *tidstr,
                              transaction_result *result)
{
    int                retval = -1;
    int                eof = 0;
    int                s;
    int                match = 0;

    clixon_debug(CLIXON_DBG_CTRL, "tid:%s", tidstr);
    if (result)
        *result = 0;
    if ((s = clicon_data_int_get(h, "controller-transaction-notify-socket")) < 0){
        clixon_err(OE_EVENTS, 0, "controller-transaction-notify-socket is closed");
        goto done;
    }
    while (!match){
        if (transaction_notification_handler(h, s, tidstr, &match, result, &eof) < 0){
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
        case TR_INIT:
        case TR_SUCCESS:
            break;
        }
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL, "%d", retval);
    return retval;
}

/*! Query backend if transaction exists. call this before polling
 *
 * @param[in]  h      Clixon handle
 * @param[in]  tidstr Transaction id
 * @param[out] exists 1 if exists
 * @retval     0      OK
 * @retval    -1      Error
 */
static int
transaction_exist(clixon_handle h,
                  char         *tidstr,
                  int          *exists)
{
    int    retval = -1;
    cxobj *xn = NULL; /* XML of transactions */
    cxobj *xerr;
    cxobj *xt;
    cvec  *nsc = NULL;

    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "co:transactions", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
        goto done;
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get transactions");
        goto done;
    }
    (*exists) = 0;
    if ((xt = xpath_first(xn, nsc, "transactions/transaction[tid='%s']", tidstr)) != NULL){
        *exists = 1;
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xn)
        xml_free(xn);
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
    int                retval = -1;
    cbuf              *cb = NULL;
    cg_var            *cv;
    cxobj             *xtop = NULL;
    cxobj             *xrpc;
    cxobj             *xret = NULL;
    cxobj             *xreply;
    cxobj             *xerr;
    char              *op;
    char              *name = "*";
    cxobj             *xid;
    char              *tidstr;
    transaction_result result = 0;
    int                exists = 0;

    if (argv == NULL || cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: replace/merge");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if (strcmp(op, "replace") != 0 && strcmp(op, "merge") != 0){
        clixon_err(OE_PLUGIN, EINVAL, "pull <type> argument is %s, expected \"validate\" or \"commit\"", op);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
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
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (transaction_exist(h, tidstr, &exists) < 0)
        goto done;
    if (exists){
        if (transaction_notification_poll(h, tidstr, &result) < 0)
            goto done;
        if (result == TR_SUCCESS)
            cligen_output(stderr, "OK\n");
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

static int
cli_rpc_commit_diff_one(clixon_handle h,
                        char         *name)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xtop = NULL;
    cxobj  *xrpc;
    cxobj  *xreply;
    cxobj  *xerr;
    cxobj  *xdiff;
    cxobj  *xret = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<config-type1>RUNNING</config-type1>");
    cprintf(cb, "<config-type2>ACTIONS</config-type2>");
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL)
            cligen_output(stdout, "%s", xml_body(xdiff));
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Make a controller commit diff variant
 *
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
static int
cli_rpc_commit_diff(clixon_handle h)
{
    int     retval = -1;
    cxobj  *xdevs = NULL;
    cxobj  *xdev;
    cvec   *nsc = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    char   *name;
    int     i;

    /* get all devices */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get_config(h, NULL, "running", "co:devices/co:device/co:name", nsc,
                              "explicit", &xdevs) < 0)
        goto done;
    if (xpath_vec(xdevs, nsc, "devices/device/name", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xdev = vec[i];
        if ((name = xml_body(xdev)) != NULL){
            if (cli_rpc_commit_diff_one(h, name) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (vec)
        free(vec);
    if (xdevs)
        xml_free(xdevs);
    return retval;
}

/*! Get first list key of controller service
 *
 * @param[in]  yspec   YANG specification
 * @param[in]  service Name o service
 * @param[out] keyname Name of (first) key of service
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
get_service_key(yang_stmt *yspec,
                char      *service,
                char     **keyname)
{
    int        retval = -1;
    yang_stmt *yres = NULL;
    cvec      *cvk = NULL;
    cbuf      *cb = NULL;
    cg_var    *cvi;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "/ctrl:services/%s", service);
    if (yang_abs_schema_nodeid(yspec, cbuf_get(cb), &yres) < 0)
        goto done;
    if (yres &&
        (cvk = yang_cvec_get(yres)) != NULL &&
        (cvi = cvec_i(cvk, 0)) != NULL){
        *keyname = cv_string_get(cvi);
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Make a controller commit rpc with its many variants
 *
 * Relies on hardcoded "name" and "instance" variables in cvv
 * @param[in] h     Clixon handle
 * @param[in] cvv   Name pattern
 * @param[in] argv  Source:running/candidate,
                    actions:NONE/CHANGE/FORCE,
                    push:NONE/VALIDATE/COMMIT,
 * @retval    0     OK
 * @retval   -1     Error
 * @see controller-commit in clixon-controller.yang
 */
int
cli_rpc_controller_commit(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int                retval = -1;
    int                argc = 0;
    cbuf              *cb = NULL;
    cg_var            *cv;
    cxobj             *xtop = NULL;
    cxobj             *xrpc;
    cxobj             *xret = NULL;
    cxobj             *xreply;
    cxobj             *xerr;
    char              *push_type;
    char              *name = "*";
    cxobj             *xid;
    char              *tidstr;
    char              *actions_type;
    char              *source;
    transaction_result result = 0;
    char              *str;
    char              *service = NULL;
    char              *instance = NULL;
    yang_stmt         *yspec;
    char              *keyname = NULL;
    int                exists = 0;

    if (argv == NULL || cvec_len(argv) != 3){
        clixon_err(OE_PLUGIN, EINVAL, "requires arguments: <datastore> <actions-type> <push-type>");
        goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <datastore>");
        goto done;
    }
    source = cv_string_get(cv);
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <actions-type>");
        goto done;
    }
    actions_type = cv_string_get(cv);
    if (actions_type_str2int(actions_type) == -1){
        clixon_err(OE_PLUGIN, EINVAL, "<actions-type> argument is %s, expected NONE/CHANGE/FORCE", actions_type);
        goto done;
    }
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push-type>");
        goto done;
    }
    push_type = cv_string_get(cv);
    if (push_type_str2int(push_type) == -1){
        clixon_err(OE_PLUGIN, EINVAL, "<push-type> argument is %s, expected NONE/VALIDATE/COMMIT", push_type);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "service")) != NULL &&
        (str = cv_string_get(cv)) != NULL){
        if (nodeid_split(str, NULL, &service) < 0)
            goto done;
    }
    if ((cv = cvec_find(cvv, "instance")) != NULL)
        instance = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
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
    if (service && instance && actions_type_str2int(actions_type) == AT_FORCE){
        if (get_service_key(yspec, service, &keyname) < 0)
            goto done;
        if (keyname){
            cprintf(cb, "<service-instance>");
            if (xml_chardata_cbuf_append(cb, 0, service) < 0)
                goto done;
            cprintf(cb, "[%s='", keyname);
            if (xml_chardata_cbuf_append(cb, 0, instance) < 0)
                goto done;
            cprintf(cb, "']");
            cprintf(cb, "</service-instance>");
        }
    }
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
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (transaction_exist(h, tidstr, &exists) < 0)
        goto done;
    if (exists){
        if (transaction_notification_poll(h, tidstr, &result) < 0)
            goto done;
        if (result != TR_SUCCESS)
            goto ok;
    }
    /* Interpret actions and no push as diff */
    if (actions_type_str2int(actions_type) != AT_NONE &&
        push_type_str2int(push_type) == PT_NONE){
        if (cli_rpc_commit_diff(h) < 0)
            goto done;
    }
    cligen_output(stderr, "OK\n");
 ok:
    retval = 0;
 done:
    if (service)
        free(service);
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Read the config of one or several devices, assumes a name variable for pattern, or NULL for all
 *
 * @param[in] h
 * @param[in] cvv  : <operation>, <wait>
 * @param[in] argv : 0: close, 1: open, 2: reconnect
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_connection_change(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int                retval = -1;
    cbuf              *cb = NULL;
    cg_var            *cv;
    cxobj             *xtop = NULL;
    cxobj             *xrpc;
    cxobj             *xret = NULL;
    cxobj             *xreply;
    cxobj             *xerr;
    char              *name = "*";
    char              *op;
    char              *wait;
    cxobj             *xid;
    char              *tidstr;
    transaction_result result = 0;
    int                exists = 0;

    if (argv == NULL || cvec_len(argv) != 2){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: <operation> <wait>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if ((cv = cvec_i(argv, 1)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    wait = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
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
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (strcmp(wait, "true") == 0){
        if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
            clixon_err(OE_CFG, 0, "No returned id");
            goto done;
        }
        tidstr = xml_body(xid);
        if (transaction_exist(h, tidstr, &exists) < 0)
            goto done;
        if (exists){
            if (transaction_notification_poll(h, tidstr, &result) < 0)
                goto done;
            if (result != TR_SUCCESS)
                cligen_output(stderr, "OK\n");
        }
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

/*! Show connection state
 *
 * @param[in] h     Clixon handle
 * @param[in] cvv   name pattern
 * @param[in] argv  [detail]
 * @retval    0     OK
 * @retval   -1     Error
 */
int
cli_show_connections(clixon_handle h,
                     cvec         *cvv,
                     cvec         *argv)
{
    int     retval = -1;
    cvec   *nsc = NULL;
    cxobj  *xc;
    cxobj  *xerr;
    cbuf   *cb = NULL;
    cxobj  *xn = NULL; /* XML of senders */
    char   *name;
    char   *state;
    char   *timestamp;
    char   *logmsg;
    char   *pattern = NULL;
    cg_var *cv;
    int     detail = 0;
    int     width;
    int     logw;
    int     i;
    char   *logstr = NULL;
    char   *p;

    if (argv != NULL && cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "optional argument: <detail>");
        goto done;
    }
    if (cvec_len(argv) == 1){
        if ((cv = cvec_i(argv, 0)) == NULL){
            clixon_err(OE_PLUGIN, 0, "Error when accessing argument <detail>");
            goto done;
        }
        detail = strcmp(cv_string_get(cv), "detail")==0;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (detail){
        if (clicon_rpc_get(h, "co:devices", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
            goto done;
    }
    else{
        /* Avoid including moint-point which triggers a lot of extra traffic */
        if (clicon_rpc_get(h,
                           "co:devices/co:device/co:name | co:devices/co:device/co:conn-state | co:devices/co:device/co:conn-state-timestamp | co:devices/co:device/co:logmsg",
                           nsc, CONTENT_ALL, -1, "explicit", &xn) < 0)
            goto done;
    }
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get devices");
        goto done;
    }
    /* Change top from "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "devices", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        /* First run to see if no matches */
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
            if (strcmp(xml_name(xc), "device") != 0)
                continue;
            name = xml_find_body(xc, "name");
            if (pattern != NULL && fnmatch(pattern, name, 0) != 0)
                continue;
            break;
        }
        if (xc == NULL){
            clixon_err(OE_CFG, errno, "No matching devices");
            goto done;
        }
        width = cligen_terminal_width(cli_cligen(h));
        logw = width - 59;
        if (logw < 0)
            logw = 1;
        cligen_output(stdout, "%-23s %-10s %-22s %-*s\n", "Name", "State", "Time", logw, "Logmsg");
        for (i=0; i<width; i++)
            cligen_output(stdout, "=");
        cligen_output(stdout, "\n");
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
            if (logstr){
                free(logstr);
                logstr = NULL;
            }
            if ((logstr = calloc(logw+1, sizeof(char))) == NULL){
                clixon_err(OE_UNIX, errno, "calloc");
                goto done;
            }
            if (strcmp(xml_name(xc), "device") != 0)
                continue;
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
                strncpy(logstr, logmsg, logw);
                if ((p = index(logstr, '\n')) != NULL)
                    *p = '\0';
                cligen_output(stdout, "%s",  logstr);
            }
            cligen_output(stdout, "\n");
        }
    }
    retval = 0;
 done:
    if (logstr)
        free(logstr);
    if (nsc)
        cvec_free(nsc);
    if (xn)
        xml_free(xn);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show controller device states
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 * @see cli_process_control
 */
int
cli_show_services_process(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int            retval = -1;
    char          *name;
    char          *opstr;
    cbuf          *cb = NULL;
    cxobj         *xret = NULL;
    cxobj         *xerr;
    cxobj         *x;
    char          *active = "false";
    char          *status = "unknown";

    name = "Action process";
    opstr = "status";
    if (clixon_process_op_str2int(opstr) == -1){
        clixon_err(OE_UNIX, 0, "No such process op: %s", opstr);
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<process-control xmlns=\"%s\">", CLIXON_LIB_NS);
    cprintf(cb, "<name>%s</name>", name);
    cprintf(cb, "<operation>%s</operation>", opstr);
    cprintf(cb, "</process-control>");
    cprintf(cb, "</rpc>");
    if (clicon_rpc_netconf(h, cbuf_get(cb), &xret, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((x = xpath_first(xret, 0, "rpc-reply/active")) != NULL){
        active = xml_body(x);
    }
    if ((x = xpath_first(xret, 0, "rpc-reply/status")) != NULL){
        status = xml_body(x);
    }
    cligen_output(stdout, "Services status: %s, active: %s\n", status, active);
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show controller device states
 *
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
    cxobj             *xn = NULL; /* XML of transactions */
    cg_var            *cv;
    int                all = 0;

    if (argv == NULL || cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: <operation>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <all>");
        goto done;
    }
    all = strcmp(cv_string_get(cv), "all")==0;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "co:transactions", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
        goto done;
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get transactions");
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

/*! Show controller client sessions
 *
 * @param[in] h
 * @param[in] cvv
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_sessions(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int      retval = -1;
    cvec    *nsc = NULL;
    cxobj   *xret = NULL;
    cxobj   *xdbs = NULL;
    cxobj   *xerr;
    cxobj   *x;
    cbuf    *cb = NULL;
    cxobj   *xsess;
    cxobj  **vec = NULL;
    size_t   veclen;
    char    *b;
    char    *sid;
    int      i;
    cg_var  *cv;
    int      detail = 0;
    uint32_t session_id;
    char    *locked_by = NULL;

    if (argv != NULL && cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "optional argument: <detail>");
        goto done;
    }
    if (cvec_len(argv) == 1){
        if ((cv = cvec_i(argv, 0)) == NULL){
            clixon_err(OE_PLUGIN, 0, "Error when accessing argument <detail>");
            goto done;
        }
        detail = strcmp(cv_string_get(cv), "detail")==0;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("ncm", "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")) == NULL)
        goto done;
    if (clicon_rpc_get(h, "ncm:netconf-state/ncm:sessions", nsc, CONTENT_NONCONFIG, -1, "report-all", &xret) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get sessions");
        goto done;
    }
    if (xpath_vec(xret, NULL, "netconf-state/sessions/session", &vec, &veclen) < 0)
        goto done;
    if (clicon_rpc_get(h, "ncm:netconf-state/ncm:datastores/ncm:datastore[ncm:name='candidate']/ncm:locks", nsc, CONTENT_NONCONFIG, -1, "report-all", &xdbs) < 0)
        goto done;
    if ((xerr = xpath_first(xdbs, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get locks");
        goto done;
    }
    if ((x = xpath_first(xdbs, NULL, "netconf-state/datastores/datastore[name='candidate']/locks/global-lock/locked-by-session")) != NULL){
        locked_by = xml_body(x);
    }
    if (!detail && veclen){
        cligen_output(stdout, "%-8s %-10s %-15s %-10s %-15s\n", "Id", "User", "Type", "Locks", "Time");
        cligen_output(stdout, "===============================================================\n");
    }
    clicon_session_id_get(h, &session_id);
    for (i=0; i<veclen; i++){
        xsess = vec[i];
        if (detail){
            if (clixon_xml2file(stdout, xsess, 0, 1, NULL, cligen_output, 0, 1) < 0)
                goto done;
        }
        else{
            sid = xml_find_body(xsess, "session-id");
            if (sid) {
                if (session_id == atoi(sid))
                    cligen_output(stdout, "*");
                else
                    cligen_output(stdout, " ");
            }
            cligen_output(stdout, "%-8s",  sid?sid:"");
            b = xml_find_body(xsess, "username");
            cligen_output(stdout, "%-11s",  b?b:"");
            b = xml_find_body(xsess, "transport");
            cligen_output(stdout, "%-16s",  b?b:"");
            if (locked_by && strcmp(sid, locked_by) == 0)
                cligen_output(stdout, "%-11s", "candidate");
            else
                cligen_output(stdout, "%-11s", "");
            b = xml_find_body(xsess, "login-time");
            cligen_output(stdout, "%-16s",  b?b:"");

            cligen_output(stdout, "\n");
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xdbs)
        xml_free(xdbs);
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
send_pull_transient(clixon_handle h,
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
    char      *tidstr = NULL;
    uint64_t   tid=0;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
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
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    if ((tidstr = strdup(xml_body(xid))) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    if (parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (tid == 0){
        clixon_err(OE_UNIX, errno, "Invalid tid = 0");
        goto done;
    }
    if (tidstrp && (*tidstrp = strdup(tidstr)) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (tidstr)
        free(tidstr);
    if (cb)
        cbuf_free(cb);
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Compare device config types: running with last saved synced or current device (transient)
 *
 * @param[in]   h       Clixon handle
 * @param[in]   cvv     name: device pattern
 * @param[in]   argv    <format>        "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @param[in]   dt1     First device config config
 * @param[in]   dt2     Second device config config
 * @param[out]  cbdiff  Diff string to show
 * @retval      0       OK
 * @retval     -1       Error
 */
static int
compare_device_config_type(clixon_handle      h,
                           cvec              *cvv,
                           cvec              *argv,
                           device_config_type dt1,
                           device_config_type dt2,
                           cbuf              *cbdiff)
{
    int                retval = -1;
    enum format_enum   format;
    cg_var            *cv;
    char              *pattern = "*";
    char              *tidstr = NULL;
    char              *formatstr;
    cxobj             *xtop = NULL;
    cxobj             *xret = NULL;
    cxobj             *xrpc;
    cxobj             *xreply;
    cxobj             *xerr;
    cxobj             *xdiff;
    cbuf              *cb = NULL;
    char              *device_type = NULL;
    transaction_result result = 0;
    cxobj            **vec = NULL;
    size_t             veclen;
    int                i;
    int                exists = 0;

    if (cvec_len(argv) > 1){
        clixon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <format>]", cvec_len(argv));
        goto done;
    }
    if (cbdiff == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "cbdiff is NULL");
        goto done;
    }
    cv = cvec_i(argv, 0);
    formatstr = cv_string_get(cv);
    if ((int)(format = format_str2int(formatstr)) < 0){
        clixon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
        goto done;
    }
    /* Special default format handling */
    if (format == FORMAT_DEFAULT){
        formatstr = clicon_option_str(h, "CLICON_CLI_OUTPUT_FORMAT");
        if ((int)(format = format_str2int(formatstr)) < 0){
            clixon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
            goto done;
        }
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    /* If remote, start with requesting it asynchrously */
    if (dt1 == DT_TRANSIENT || dt2 == DT_TRANSIENT){
        /* Send pull <transient> */
        if (send_pull_transient(h, pattern, &tidstr) < 0)
            goto done;
        if (transaction_exist(h, tidstr, &exists) < 0)
            goto done;
        if (exists){
            /* Wait to complete transaction try ^C here */
            if (transaction_notification_poll(h, tidstr, &result) < 0)
                goto done;
            if (result != TR_SUCCESS)
                goto done;
        }
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<xpath>config</xpath>");
    cprintf(cb, "<format>%s</format>", formatstr);
    device_type = device_config_type_int2str(dt1);
    cprintf(cb, "<devname>%s</devname>", pattern);
    cprintf(cb, "<config-type1>%s</config-type1>", device_type);
    device_type = device_config_type_int2str(dt2);
    cprintf(cb, "<config-type2>%s</config-type2>", device_type);
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL){
            cprintf(cbdiff, "%s", xml_body(xdiff));
        }
    }
    retval = 0;
 done:
    if (tidstr)
        free(tidstr);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compare datastores uses special diff rpc
 *
 * Use specialized rpc to reduce bandwidth
 * @param[in]   h     Clixon handle
 * @param[in]   cvv
 * @param[in]   argv  <db1> <db2> <format>
 * @retval      0     OK
 * @retval     -1     Error
 * @see compare_dbs  original function
 */
int
compare_dbs_rpc(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    int              retval = -1;
    char            *db1;
    char            *db2;
    enum format_enum format;
    char            *formatstr;
    cxobj           *xtop = NULL;
    cxobj           *xret = NULL;
    cxobj           *xrpc;
    cxobj           *xreply;
    cxobj           *xerr;
    cxobj           *xdiff;
    cbuf            *cb = NULL;
    cxobj          **vec = NULL;
    size_t           veclen;
    int              i;

    if (cvec_len(argv) != 3){
        clixon_err(OE_PLUGIN, EINVAL, "Expected arguments: <db1> <db2> <format>");
        goto done;
    }
    db1 = cv_string_get(cvec_i(argv, 0));
    db2 = cv_string_get(cvec_i(argv, 1));
    formatstr = cv_string_get(cvec_i(argv, 2));
    if ((format = format_str2int(formatstr)) < 0){
        clixon_err(OE_XML, 0, "format not found %s", formatstr);
        goto done;
    }
    /* Special default format handling */
    if (format == FORMAT_DEFAULT){
        formatstr = clicon_option_str(h, "CLICON_CLI_OUTPUT_FORMAT");
        if ((int)(format = format_str2int(formatstr)) < 0){
            clixon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
            goto done;
        }
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<format>%s</format>", formatstr);
    cprintf(cb, "<dsref1>ds:%s</dsref1>", db1);
    cprintf(cb, "<dsref2>ds:%s</dsref2>", db2);
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL)
            cligen_output(stdout, "%s", xml_body(xdiff));
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
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
 * @param[in]   h     Clixon handle
 * @param[in]   cvv
 * @param[in]   argv  arg: 0 as xml, 1: as text
 * @retval      0     OK
 * @retval     -1     Error
 */
int
compare_device_db_sync(clixon_handle h,
                       cvec         *cvv,
                       cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_SYNCED, DT_RUNNING, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "%s", cbuf_get(cbdiff));
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Compare device dbs: running with current device (transient)
 *
 * @param[in] h     Clixon handle
 * @param[in] cvv  : name pattern or NULL
 * @param[in] argv  arg: 0 as xml, 1: as text
 * @retval    0     OK
 * @retval   -1     Error
 * @see check_device_db  only replies with boolean
 */
int
compare_device_db_dev(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_TRANSIENT, DT_RUNNING, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "%s", cbuf_get(cbdiff));
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Check if device(s) is in sync
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
 * @retval    0    OK
 * @retval   -1    Error
 * @see compare_device_db_dev  with detailed diff
 */
int
check_device_db(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_RUNNING, DT_TRANSIENT, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "device out-of-sync\n");
    else
        cligen_output(stdout, "OK\n");
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Sub-routine for device dbxml: api-path to xml and send edit-config
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables
 * @param[in]  op    Operation to perform on datastore
 * @param[in]  nsctx Namespace context for last value added
 * @param[in]  api_path
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
cli_dbxml_devs_sub(clixon_handle       h,
                   cvec               *cvv,
                   enum operation_type op,
                   cvec               *nsctx,
                   int                 cvvi,
                   char               *api_path)
{
    int        retval = -1;
    cxobj     *xtop = NULL;     /* xpath root */
    cxobj     *xbot = NULL;     /* xpath, NULL if datastore */
    cxobj     *xerr = NULL;
    yang_stmt *yspec0;
    yang_stmt *yspec;
    cbuf      *cb = NULL;
    yang_stmt *y = NULL;        /* yang spec of xpath */
    cg_var    *cv;
    int        ret;

    /* Top-level yspec */
    if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    /* Create config top-of-tree */
    if ((xtop = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
        goto done;
    xbot = xtop;
    if (api_path){
        if ((ret = api_path2xml(api_path, yspec0, xtop, YC_DATANODE, 1, &xbot, &y, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_CFG, EINVAL, xerr, "api-path syntax error \"%s\": ", api_path);
            goto done;
        }
    }
    if (xml_add_attr(xbot, "operation", xml_operation2str(op), NETCONF_BASE_PREFIX, NULL) == NULL)
        goto done;
    /* Add body last in case of leaf */
    if (cvec_len(cvv) > 1 &&
        (yang_keyword_get(y) == Y_LEAF)){
        /* Add the body last if there is remaining element that was not used in the
         * earlier api-path transformation.
         * This is to handle differences between:
         * DELETE <foo>bar</foo> and DELETE <foo/>
         * i.e., (1) deletion of a specific leaf entry vs (2) deletion of any entry
         * Discussion: one can claim (1) is "bad" usage but one could see cases where
         * you would want to delete a value if it has a specific value but not otherwise
         */
        if (cvvi != cvec_len(cvv)){
            if (dbxml_body(xbot, cvv) < 0)
                goto done;
        }
        /* Loop over namespace context and add them to this leaf node */
        cv = NULL;
        while ((cv = cvec_each(nsctx, cv)) != NULL){
            char *ns = cv_string_get(cv);
            char *pf = cv_name_get(cv);
            if (ns && pf && xmlns_set(xbot, pf, ns) < 0)
                goto done;
        }
    }
    /* Special handling of identityref:s whose body may be: <namespace prefix>:<id>
     * Ensure the namespace is declared if it exists in YANG
     */
    if (y)
        yspec =  ys_spec(y);
    else
        yspec = yspec0;
    if ((ret = xml_apply0(xbot, CX_ELMNT, identityref_add_ns, yspec)) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xtop, 0, 0, NULL, -1, 0) < 0)
        goto done;
    if (clicon_rpc_edit_config(h, "candidate", OP_NONE, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xtop)
        xml_free(xtop);
    if (xerr)
        xml_free(xerr);
    return retval;
}

/*! Modify xml datastore from a callback using xml key format strings
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables
 * @param[in]  argv  Vector: <apipathfmt> [<mointpt>], eg "/aaa/%s"
 * @param[in]  op    Operation to perform on datastore
 * @param[in]  nsctx Namespace context for last value added
 * @retval     0     OK
 * @retval    -1     Error
 * cvv first contains the complete cli string, and then a set of optional
 * instantiated variables.
 * If the last node is a leaf, the last cvv element is added as a value. This value
 * Example:
 * cvv[0]  = "set interfaces interface eth0 type bgp"
 * cvv[1]  = "eth0"
 * cvv[2]  = "bgp"
 * argv[0] = "/interfaces/interface/%s/type"
 * op: OP_MERGE
 * @see cli_callback_generate where arg is generated
 * @note The last value may require namespace binding present in nsctx. Note that the nsctx
 *   cannot normally be supplied by the clispec functions, such as cli_set, but need to be
 *   generated by a function such as clixon_instance_id_bind() or other programmatically.
 * @see cli_show_auto_devs Similar controller handling
 */
static int
cli_dbxml_devs(clixon_handle       h,
               cvec               *cvv,
               cvec               *argv,
               enum operation_type op,
               cvec               *nsctx)
{
    int        retval = -1;
    char      *api_path_fmt;    /* xml key format */
    char      *api_path = NULL;
    cg_var    *cv;
    int        cvvi = 0;
    char      *mtpoint = NULL;
    char      *pattern;
    cxobj     *xdevs = NULL;
    cxobj     *xdev;
    char      *devname;
    int        devices = 0;
    char      *str;
    cbuf      *api_path_fmt_cb = NULL;    /* xml key format */
    int        i;

    if (cvec_len(argv) < 1){
        clixon_err(OE_PLUGIN, EINVAL, "Requires first element to be xml key format string");
        goto done;
    }
    if ((api_path_fmt_cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Remove all keywords */
    if (cvec_exclude_keys(cvv) < 0)
        goto done;
    /* Concatenate all argv strings to a single string
     * Variant of cvec_concat_cb() where api-path-fmt may be interleaved with mtpoint,
     * eg /api-path-fmt2 mtpoint /api-path-fmt1 /api-path-fmt0
     */
    for (i=cvec_len(argv)-1; i>=0; i--){
        cv = cvec_i(argv, i);
        str = cv_string_get(cv);
        if (str[0] != '/')
            continue;
        cprintf(api_path_fmt_cb, "%s", str);
    }
    api_path_fmt = cbuf_get(api_path_fmt_cb);
    /* See if 2nd arg is mountpoint and if devices cmd tree is selected */
    if (cvec_len(argv) > 1 &&
        (cv = cvec_i(argv, 1)) != NULL &&
        (str = cv_string_get(cv)) != NULL &&
        strncmp(str, "mtpoint:", strlen("mtpoint:")) == 0){
        mtpoint = str + strlen("mtpoint:");
        devices = strstr(mtpoint, "/ctrl:devices") != NULL;
    }
    else{
        devices = strstr(api_path_fmt, "/clixon-controller:devices") != NULL;
    }
    if (devices && (cv = cvec_find(cvv, "name")) != NULL){
        pattern = cv_string_get(cv);
        if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs) < 0)
            goto done;
        if (xdevs == NULL){
            if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
                goto done;
            if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
                goto done;
        }
        else {
            xdev = NULL;
            while ((xdev = xml_child_each(xml_find(xdevs, "devices"), xdev, CX_ELMNT)) != NULL) {
                if ((devname = xml_find_body(xdev, "name")) == NULL)
                    continue;
                cv_string_set(cv, devname); /* replace name */
                if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
                    goto done;
                if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
                    goto done;
                if (api_path){
                    free(api_path);
                    api_path = NULL;
                }
            }
        }
    }
    else{
        if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
            goto done;
        if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (xdevs)
        xml_free(xdevs);
    if (api_path_fmt_cb)
        cbuf_free(api_path_fmt_cb);
    if (api_path)
        free(api_path);
    return retval;
}

/*! CLI callback: set auto db item, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * Format of argv:
 *   <api-path-fmt> Generated
 * @see cli_auto_set  original callback
 */
int
cli_auto_set_devs(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_REPLACE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Merge datastore xml entry, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_auto_merge  original callback
 */
int
cli_auto_merge_devs(clixon_handle h,
                    cvec         *cvv,
                    cvec         *argv)
{
    int retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_MERGE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Delete datastore xml, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_auto_del  original callback
 */
int
cli_auto_del_devs(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_REMOVE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Load configuration from file
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv0  Vector of cli string and instantiated variables
 * @param[in]  argv  Vector. First element xml key format string, eg "/aaa/%s"
 * @retval     0     OK
 * @retval    -1     Error
 * @see cli_auto_merge  original callback
 */
int
cli_auto_load_devs(clixon_handle h,
                   cvec         *cvv0,
                   cvec         *argv)
{
    int                 retval = -1;
    enum operation_type op = OP_MERGE;
    enum format_enum    format = FORMAT_XML;
    cg_var             *cv;
    cvec               *cvv = NULL;
    char               *filename = NULL;
    FILE               *fp = NULL;
    cxobj              *xt = NULL;
    cxobj              *xerr = NULL;
    cbuf               *cb = NULL;
    int                 ret;

    if ((cvv = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv0)) == NULL)
        goto done;
    if ((cv = cvec_find(cvv, "operation")) != NULL) {
        if (xml_operation(cv_string_get(cv), &op) < 0)
            goto done;
    }
    if ((cv = cvec_find(cvv, "format")) != NULL) {
        if ((format = format_str2int(cv_string_get(cv))) < 0)
            goto done;
    }
    if ((cv = cvec_find(cvv, "filename")) != NULL){
        filename = cv_string_get(cv);
        /* Open and parse local file into xml */
        if ((fp = fopen(filename, "r")) == NULL){
            clixon_err(OE_UNIX, errno, "fopen(%s)", filename);
            goto done;
        }
    }
    else
        fp = stdin;
    /* XXX Do without YANG (for the time being) */
    switch (format){
    case FORMAT_XML:
        if ((ret = clixon_xml_parse_file(fp, YB_NONE, NULL, &xt, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_XML, 0, xerr, "Loading: %s", filename?filename:"stdin");
            goto done;
        }
        if (xml_child_nr(xt) == 0){ // XXX DENNIS
            clixon_err(OE_XML, 0, "No XML in file %s", filename?filename:"stdin");
            goto done;
        }
        break;
    case FORMAT_JSON:
        if ((ret = clixon_json_parse_file(fp, 1, YB_NONE, NULL, &xt, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_XML, 0, xerr, "Loading: %s", filename?filename:"stdin");
            goto done;
        }
        if (xml_child_nr(xt) == 0){
            clixon_err(OE_XML, 0, "No XML in file %s", filename?filename:"stdin");
            goto done;
        }
        break;
    default:
        clixon_err(OE_PLUGIN, 0, "format: %s not implemented", format_int2str(format));
        goto done;
        break;
    }
    if (xt == NULL)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xt, 0, 0, NULL, -1, 1) < 0)
        goto done;
    if (clicon_rpc_edit_config(h, "candidate",
                               op,
                               cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
    if (xerr)
        xml_free(xerr);
    if (fp){
        if (fp == stdin)
            clearerr(stdin);
        else
            fclose(fp);
    }
    if (cvv)
        cvec_free(cvv);
    return retval;
}

/*! Show controller and clixon version
 *
 * @see controller_version
 */
int
cli_controller_show_version(clixon_handle h,
                            cvec         *vars,
                            cvec         *argv)
{
    cligen_output(stdout, "CLIgen: \t%s\n", CLIGEN_VERSION);
    cligen_output(stdout, "Clixon: \t%s\n", CLIXON_VERSION);
    cligen_output(stdout, "Controller:\t%s\n", CONTROLLER_VERSION);
    cligen_output(stdout, "Build:\t\t%s\n", CONTROLLER_BUILDSTR);
    return 0;
}

/*! show yang revisions of top-level / mountpoint.
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of command variables
 * @param[in]  argv  Name of cv containing name of top-level/mountpoint
 * @retval     0     OK
 * @retval    -1     Error
 */
int
show_yang_revisions(clixon_handle h,
                    cvec         *cvv,
                    cvec         *argv)
{
    int        retval = -1;
    char      *cvname = NULL;
    char      *name = NULL;
    char      *name1;
    char      *module;
    char      *revision;
    cg_var    *cv;
    cxobj     *xt = NULL;
    cxobj     *xerr;
    cxobj     *xmodset;
    cxobj     *x;
    cvec      *nsc = NULL;
    cbuf      *cb = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;

    if (cvec_len(argv) > 0 &&
        (cvname = cv_string_get(cvec_i(argv, 0))) != NULL) {
        if ((cv = cvec_find(cvv, cvname)) != NULL){
            if ((name = cv_string_get(cv)) == NULL){
                clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                goto done;
            }
        }
    }
    if (name == NULL || (strcmp(name, "top") != 0 && strcmp(name, "config") != 0)) {
        if ((cb = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
            goto done;
        if (xml_nsctx_add(nsc, "yanglib", "urn:ietf:params:xml:ns:yang:ietf-yang-library") < 0)
            goto done;
        /* XXX: cannot access yanglib:yang-library/yanglib:module-set directly */
        if (name)
            cprintf(cb, "/devices/device[name='%s']/config", name);
        else
            cprintf(cb, "/devices/device/config");
        if (clicon_rpc_get(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", &xt) < 0)
            goto done;
        if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
            clixon_err_netconf(h, OE_NETCONF, 0, xerr, "Get configuration");
            goto done;
        }
        cprintf(cb, "/yanglib:yang-library/yanglib:module-set");
        if (xpath_vec(xt, nsc, "%s", &vec, &veclen, cbuf_get(cb)) < 0)
            goto done;
        for (i=0; i<veclen; i++){
            xmodset = vec[i];
            name1 = xml_find_body(xml_parent(xml_parent(xml_parent(xmodset))), "name");
            if (name && strcmp(name, name1))
                continue;
            cligen_output(stdout, "%s:\n", name1);
            x = NULL;
            while ((x = xml_child_each(xmodset, x, CX_ELMNT)) != NULL){
                if (strcmp(xml_name(x), "module") != 0)
                    continue;
                module = xml_find_body(x, "name");
                revision = xml_find_body(x, "revision");
                //            namespace = xml_find_body(x, "namespace");
                if (revision)
                    cligen_output(stdout, "%s@%s\n", module, revision);
                else
                    cligen_output(stdout, "%s\n", module);
            }
            if (name == NULL && i<veclen-1)
                cligen_output(stdout, "\n");
        }
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (cb)
        cbuf_get(cb);
    if (nsc)
        cvec_free(nsc);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! show device capabilities, subset of state / hello
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of command variables
 * @param[in]  argv  Name of cv containing name of top-level/mountpoint
 * @retval     0     OK
 * @retval    -1     Error
 */
int
show_device_capability(clixon_handle h,
                       cvec         *cvv,
                       cvec         *argv)
{
    int     retval = -1;
    char   *cvname = NULL;
    char   *name = NULL;
    char   *name1;
    cg_var *cv;
    cxobj  *xt = NULL;
    cxobj  *xerr;
    cvec   *nsc = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    cxobj  *xcaps;
    int     i;

    if (cvec_len(argv) > 0 &&
        (cvname = cv_string_get(cvec_i(argv, 0))) != NULL) {
        if ((cv = cvec_find(cvv, cvname)) != NULL){
            if ((name = cv_string_get(cv)) == NULL){
                clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                goto done;
            }
        }
    }
    if ((nsc = xml_nsctx_init(NULL, CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "/devices/device/capabilities", nsc, CONTENT_ALL, -1, "explicit", &xt) < 0)
        goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_NETCONF, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xt, nsc, "/devices/device/capabilities", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xcaps = vec[i];
        name1 = xml_find_body(xml_parent(xcaps), "name");
        if (name && strcmp(name, name1))
            continue;
        cligen_output(stdout, "%s:\n", name1);
        if (clixon_xml2file(stdout, xcaps, 0, 1, NULL, cligen_output, 0, 1) < 0)
            goto done;
        if (name == NULL && i<veclen-1)
            cligen_output(stdout, "\n");
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (nsc)
        cvec_free(nsc);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Apply template on devices
 *
 * @param[in] h
 * @param[in] cvv  templ, devs
 * @param[in] argv null
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_apply_device_template(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cg_var *cv;
    cxobj  *xtop = NULL;
    cxobj  *xrpc;
    cxobj  *xret = NULL;
    cxobj  *xreply;
    cxobj  *xerr;
    char   *devs = "*";
    char   *templ;
    char   *var;

    if (argv != NULL){
        clixon_err(OE_PLUGIN, EINVAL, "requires expected NULL");
        goto done;
    }
    if ((cv = cvec_find(cvv, "templ")) == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "template variable required");
        goto done;
    }
    templ = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "devs")) != NULL)
        devs = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<device-template-apply xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", devs);
    cprintf(cb, "<template>%s</template>", templ);
    cprintf(cb, "<variables>");
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL){
        if (strcmp(cv_name_get(cv), "var") == 0){
            var = cv_string_get(cv);
            if ((cv = cvec_next(cvv, cv)) == NULL)
                break;
            if (strcmp(cv_name_get(cv), "val") == 0){
                cprintf(cb, "<variable><name>%s</name><value>%s</value></variable>",
                        var, cv_string_get(cv));
            }
        }
    }
    cprintf(cb, "</variables>");
    cprintf(cb, "</device-template-apply>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
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

/*! Completion callback for device rpc:s
 *
 * @param[in]   h        Clixon handle
 * @param[in]   name     Name of this function
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback:
 *                       name  Name of cv containing device name
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @retval      0        OK
 * @retval     -1        Error
 */
int
expand_device_rpc(void   *h,
                  char   *name,
                  cvec   *cvv,
                  cvec   *argv,
                  cvec   *commands,
                  cvec   *helptexts)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    char      *cvname = NULL;
    cg_var    *cv;
    char      *devname = NULL;
    cxobj     *xdevs = NULL;
    cxobj     *xdevc;
    yang_stmt *yspec1;
    yang_stmt *ymod;
    yang_stmt *yrpc;
    yang_stmt *ydesc;
    int        inext;
    int        inext1;
    int        ret;

    if (argv == NULL || cvec_len(argv) < 1 || cvec_len(argv) > 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires arguments: <name>");
        goto done;
    }
    if (cvec_len(argv) > 0 &&
        (cvname = cv_string_get(cvec_i(argv, 0))) != NULL) {
        if ((cv = cvec_find(cvv, cvname)) != NULL){
            if ((devname = cv_string_get(cv)) == NULL){
                clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                goto done;
            }
        }
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (rpc_get_yanglib_mount_match(h, devname, 1, 1, &xdevs) < 0)
        goto done;
    if (xdevs != NULL &&
        (xdevc = xpath_first(xdevs, 0, "devices/device[name='%s']/config", devname)) != NULL){
        if ((ret = xml_yang_mount_get(h, xdevc, NULL, NULL, &yspec1)) < 0)
            goto done;
        inext = 0;
        while ((ymod = yn_iter(yspec1, &inext)) != NULL) {
            if (yang_keyword_get(ymod) != Y_MODULE &&
                yang_keyword_get(ymod) != Y_SUBMODULE)
                continue;
            inext1 = 0;
            while ((yrpc = yn_iter(ymod, &inext1)) != NULL) {
                if (yang_keyword_get(yrpc) != Y_RPC)
                    continue;
                cbuf_reset(cb);
                cprintf(cb, "%s:%s", yang_argument_get(ymod), yang_argument_get(yrpc));
                cvec_add_string(commands, NULL, cbuf_get(cb));
                if ((ydesc = yang_find(yrpc, Y_DESCRIPTION, NULL)) != NULL)
                    cvec_add_string(helptexts, NULL, yang_argument_get(ydesc));
                else
                    cvec_add_string(helptexts, NULL, "RPC");
            }
        }
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show generic rpc YANG signature
 *
 * @param[in]   h        Clixon handle
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback:
 *                       name     Name of cv containing device name
 *                       rpcname  Name of cv containing module/rpc
 */
int
cli_generic_rpc_show(clixon_handle h,
                     cvec         *cvv,
                     cvec         *argv)
{
    int        retval = -1;
    char      *cvname = NULL;
    char      *devname = NULL;
    char      *rpcname = NULL;
    cg_var    *cv;
    char      *module = NULL;
    char      *rpc = NULL;
    yang_stmt *yspec1;
    yang_stmt *ymod;
    yang_stmt *yrpc;
    cxobj     *xdevs;
    cxobj     *xdevc;
    int        ret;

    if (cvec_len(argv) > 0){
        if ((cvname = cv_string_get(cvec_i(argv, 0))) != NULL) {
            if ((cv = cvec_find(cvv, cvname)) != NULL){
                if ((devname = cv_string_get(cv)) == NULL){
                    clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                    goto done;
                }
            }
        }
        if ((cvname = cv_string_get(cvec_i(argv, 1))) != NULL) {
            if ((cv = cvec_find(cvv, cvname)) != NULL){
                if ((rpcname = cv_string_get(cv)) == NULL){
                    clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                    goto done;
                }
            }
        }
    }
    if (devname && rpcname){
        if (nodeid_split(rpcname, &module, &rpc) < 0)
            goto done;
        if (module && rpc){
            if (rpc_get_yanglib_mount_match(h, devname, 1, 1, &xdevs) < 0)
                goto done;
            if (xdevs != NULL &&
                (xdevc = xpath_first(xdevs, 0, "devices/device[name='%s']/config", devname)) != NULL){
                if ((ret = xml_yang_mount_get(h, xdevc, NULL, NULL, &yspec1)) < 0)
                    goto done;
                if ((ymod = yang_find(yspec1, Y_MODULE, module)) != NULL ||
                    (ymod = yang_find(yspec1, Y_SUBMODULE, module)) != NULL){
                    if ((yrpc = yang_find(ymod, Y_RPC, rpc)) != NULL){
                        yang_print(stdout, yrpc);
                    }
                }
            }
        }
    }
    retval = 0;
 done:
    if (module)
        free(module);
    if (rpc)
        free(rpc);
    return retval;
}

/*! Send generic rpc to controller to relay to device
 *
 * @param[in]  h         Clixon handle
 * @param[in]  devname   Device name
 * @param[in]  rpc       Name of RPC
 * @param[in]  namespace RPC namespace
 * @param[in]  input     RPC input parameters
 */
static int
cli_send_generic_rpc(clixon_handle h,
                     char         *devname,
                     char         *rpc,
                     char         *namespace,
                     char         *input)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xtop = NULL;
    cxobj  *xret = NULL;
    cxobj  *xrpc;
    cxobj  *xreply;
    cxobj  *xerr;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<device-generic-rpc xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", devname);
    cprintf(cb, "<rpc-data>");
    cprintf(cb, "<%s xmlns=\"%s\">", rpc, namespace);
    if (input)
        cprintf(cb, "%s", input);
    cprintf(cb, "</%s>", rpc);
    cprintf(cb, "</rpc-data>");
    cprintf(cb, "</device-generic-rpc>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
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

/*! Call generic rpc on devices via controller
 *
 * @param[in]   h        Clixon handle
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback:
 *                       devname  Name of cv containing device name
 *                       rpcname  Name of cv containing module/rpc
 *                       input  Name of cv containing module/rpc
 */
int
cli_generic_rpc_call(clixon_handle h,
                     cvec         *cvv,
                     cvec         *argv)
{
    int        retval = -1;
    char      *cvname = NULL;
    char      *pattern = NULL;
    char      *devname;
    char      *rpcname = NULL;
    char      *input = NULL;
    cg_var    *cv;
    char      *module = NULL;
    char      *rpc = NULL;
    yang_stmt *yspec1;
    yang_stmt *ymod;
    yang_stmt *yrpc;
    cxobj     *xdevs0 = NULL;
    cxobj     *xdevs1 = NULL;
    cxobj     *xdev0;
    cxobj     *xdevc;
    int        ret;

    if (cvec_len(argv) != 2 && cvec_len(argv) != 3){
        clixon_err(OE_PLUGIN, EINVAL, "requires arguments: <devname> <rpcname> [<inputname>]");
        goto done;
    }
    if (cvec_len(argv) > 0){
        if ((cvname = cv_string_get(cvec_i(argv, 0))) != NULL) {
            if ((cv = cvec_find(cvv, cvname)) != NULL){
                if ((pattern = cv_string_get(cv)) == NULL){
                    clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                    goto done;
                }
            }
        }
        if ((cvname = cv_string_get(cvec_i(argv, 1))) != NULL) {
            if ((cv = cvec_find(cvv, cvname)) != NULL){
                if ((rpcname = cv_string_get(cv)) == NULL){
                    clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                    goto done;
                }
            }
        }
        if (cvec_len(argv) > 2 &&
            (cvname = cv_string_get(cvec_i(argv, 2))) != NULL) {
            if ((cv = cvec_find(cvv, cvname)) != NULL){
                if ((input = cv_string_get(cv)) == NULL){
                    clixon_err(OE_PLUGIN, EINVAL, "cv name is empty");
                    goto done;
                }
            }
        }
    }
    if (pattern && rpcname){
        if (nodeid_split(rpcname, &module, &rpc) < 0)
            goto done;
        if (module && rpc){
            if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs0) < 0)
                goto done;
            /* Loop through all matching devices
             * But only for first match
             */
            xdev0 = NULL;
            while ((xdev0 = xml_child_each(xml_find(xdevs0, "devices"), xdev0, CX_ELMNT)) != NULL) {
                if ((devname = xml_find_body(xdev0, "name")) == NULL)
                    continue;
                if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
                    continue;
                if (rpc_get_yanglib_mount_match(h, devname, 0, 1, &xdevs1) < 0)
                    goto done;
                if (xdevs1 != NULL &&
                    (xdevc = xpath_first(xdevs1, 0, "devices/device[name='%s']/config", devname)) != NULL){
                    if ((ret = xml_yang_mount_get(h, xdevc, NULL, NULL, &yspec1)) < 0)
                        goto done;
                    if ((ymod = yang_find(yspec1, Y_MODULE, module)) != NULL ||
                        (ymod = yang_find(yspec1, Y_SUBMODULE, module)) != NULL){
                        if ((yrpc = yang_find(ymod, Y_RPC, rpc)) != NULL){
                            if (cli_send_generic_rpc(h, pattern, rpc,
                                                     yang_find_mynamespace(yrpc), input) < 0)
                                goto done;
                        }
                    }
                    break;
                }
            }
        }
    }
    retval = 0;
 done:
    if (module)
        free(module);
    if (rpc)
        free(rpc);
    if (xdevs0)
        xml_free(xdevs0);
    if (xdevs1)
        xml_free(xdevs1);
    return retval;
}
