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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <signal.h> /* matching strings */

/* clicon */
#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_cli.h>
#include <clixon/cli_generate.h>

/*! Example "downcall", ie initiate an RPC to the backend */
int
cli_connect_rpc(clicon_handle h, 
                cvec         *cvv, 
                cvec         *argv)
{
    int        retval = -1;
    cg_var    *cv;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xerr;

    /* User supplied variable in CLI command */
    if ((cv = cvec_find(cvv, "name")) != NULL){
        if (clixon_xml_parse_va(YB_NONE, NULL, &xtop, NULL,
                                "<rpc xmlns=\"%s\" username=\"%s\" %s>"
                                "<connect xmlns=\"urn:example:clixon-controller\"><name>%s</name></connect></rpc>",
                                NETCONF_BASE_NAMESPACE,
                                clicon_username_get(h),
                                NETCONF_MESSAGE_ID_ATTR,
                                cv_string_get(cv)) < 0)
            goto done;
    }
    else
        if (clixon_xml_parse_va(YB_NONE, NULL, &xtop, NULL,
                                "<rpc xmlns=\"%s\" username=\"%s\" %s>"
                                "<connect xmlns=\"urn:example:clixon-controller\"></connect></rpc>",
                                NETCONF_BASE_NAMESPACE,
                                clicon_username_get(h),
                                NETCONF_MESSAGE_ID_ATTR) < 0)
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
    /* Print result */
    if (clixon_xml2file(stdout, xml_child_i(xret, 0), 0, 0, cligen_output, 0, 1) < 0)
        goto done;
    fprintf(stdout,"\n");

    /* pretty-print:
       clixon_txt2file(stdout, xml_child_i(xret, 0), 0, cligen_output, 0);
    */
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! XXX 
 * copy from cli_connect_rpc
 */
int
cli_sync_rpc(clicon_handle h, 
             cvec         *cvv, 
             cvec         *argv)
{
    return 0;
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
clixon_plugin_init(clicon_handle h)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec);

    return &api;
}
