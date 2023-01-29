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
#include "controller_custom.h"
#include "controller.h"

/*! Read the config of one or several devices
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
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
    cxobj     *xerr;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<sync xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if ((cv = cvec_find(cvv, "name")) != NULL)
        cprintf(cb, "<name>%s</name>", cv_string_get(cv));
    cprintf(cb, "</sync></rpc>");
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
#if 0
    /* Print result */
    if (clixon_xml2file(stdout, xml_child_i(xret, 0), 0, 1, cligen_output, 0, 1) < 0)
        goto done;
#endif
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
