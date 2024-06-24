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
#include "controller_cli_callbacks.h"

/*! Start cli with -- -g
 *
 * Expand of all clispec:s for all open devices at startup
 * Gives an initial delay, instead of at first device expand
 */
static int gentree_expand_all = 0;

/* Forward */
static int controller_gentree_all(cligen_handle ch);

/*! Called when application is "started", (almost) all initialization is complete
 *
 * Create a global transaction notification handler and socket
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int
controller_cli_start(clixon_handle h)
{
    int   retval = -1;
    int   s;

    clicon_data_set(h, "session-transport", "cl:cli");
    if (clicon_rpc_create_subscription(h, "controller-transaction", NULL, &s) < 0)
        goto done;
    if (clicon_data_int_set(h, "controller-transaction-notify-socket", s) < 0)
        goto done;
    clixon_debug(CLIXON_DBG_CTRL, "notification socket:%d", s);
    if (gentree_expand_all == 1)
        if (controller_gentree_all(cli_cligen(h)) < 0)
            goto done;
    retval = 0;
 done:
    return retval;
}

/*! Called just before plugin unloaded.
 *
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int
controller_cli_exit(clixon_handle h)
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
            clixon_err(OE_XML, errno, "cbuf_new");
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
        clicon_data_int_del(h, "controller-transaction-notify-socket");
        close(s);
    }
    if ((s = clicon_client_socket_get(h)) > 0){
        close(s);
        clicon_client_socket_set(h, -1);
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (msg)
        free(msg);
    return retval;
}

/*! Given device config, check yspec of mount-point, see if shared exists, or parse new
 *
 * Given device name and config, extract module-state (xyanglib)
 * Check if yspec of mount-point exists,
 * If not, check if there is another equivalent xyanglib and if so reuse that yspec
 * and parse the yang modules and set yspec at mountpoint
 * @param[in]  h         Clixon handle
 * @param[in]  xdev      XML device tree full state
 * @param[in]  xdevsp    List of devices shallow tree on the form <data><devices>..
 * @param[in]  xyanglib  Yang-lib in XML format
 * @param[in]  devname   Device name
 * @param[in]  treename  Autocli treename
 * @param[out] yspec1p   yang spec
 * @retval     1         Ok
 * @retval     0         Ok, skip no match
 * @retval    -1         Error
 * @see yang_schema_yanglib_parse_mount
 */
static int
check_mtpoint_yspec(clixon_handle h,
                    cxobj        *xdevs,
                    char         *devname,
                    char         *treename,
                    yang_stmt   **yspec1p)
{
    int        retval = -1;
    yang_stmt *yspec1 = NULL;
    cxobj     *xdevc;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((xdevc = xpath_first(xdevs, 0, "devices/device[name='%s']/config", devname)) == NULL)
        goto skip;
    if (yang_schema_yanglib_parse_mount(h, xdevc) < 0)
        goto done;
    if (controller_mount_yspec_get(h, devname, &yspec1) < 0)
        goto done;
    if (yspec1p)
        *yspec1p = yspec1;
    retval = 1;
 done:
    return retval;
 skip:
    retval = 0;
    goto done;
}

/*! Check one level of parsetree equivalence
 *
 * @param[in]  pt1
 * @param[in]  pt2
 * @retval     0    If equal
 * @retval    <0    If co1 is less than co2
 * @retval    >0    If co1 is greater than co2
 */
static int
pt_eq1(parse_tree *pt1,
       parse_tree *pt2)
{
    int     ptlen;
    int     eq;
    cg_obj *co1;
    cg_obj *co2;
    int     i;

    ptlen = pt_len_get(pt1);
    if ((eq = ptlen - pt_len_get(pt1)) == 0)
        for (i=0; i<ptlen; i++){
            co1 = pt_vec_i_get(pt1, i);
            co2 = pt_vec_i_get(pt2, i);
            if (co1 == NULL && co2 == NULL)
                continue;
            else if (co1 == NULL){
                eq = -1;
                break;
            }
            else if (co2 == NULL){
                eq = 1;
                break;
            }
            if ((eq = co_eq(co1, co2)) != 0)
                break;
        }
    return eq;
}

/*! Force generation of clispec trees for devices given their YANG
 *
 * Typically called at startup to fully expand mounted yangs and clispecs.
 * Caveat: if backend have not connected to devices, do not create new yspec
 * 1. Check if yang controller extension/unknown mount-point exists (yu)
 * 2. Create xpath to specific mountpoint given by devname
 * 3. Check if yspec associated to that mountpoint exists
 * 4. Get yang specs of mountpoint from controller
 * 5. Parse YANGs locally from the yang specs
 * 6. Generate auto-cligen tree from the specs

 * @param[in]  pattern Device name pattern (typically "*")
 * @retval     0       OK
 * @retval    -1       Error
 * @see controller_gentree_one  for single on-demand clispec tree
 */
static int
controller_gentree_all(cligen_handle ch)
{
    int           retval = -1;
    clixon_handle h;
    cbuf         *cb = NULL;
    cxobj        *xdevs0 = NULL;
    cxobj        *xdevs1 = NULL;
    cxobj        *xdev0;
    char         *pattern = "*";
    char         *devname;
    pt_head      *ph;
    char         *newtree;
    yang_stmt    *yspec1 = NULL;
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    h = cligen_userhandle(ch);
    if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs0) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Loop through all matching devices, check if clispec exists, if not generate
     * But only for first match
     */
    xdev0 = NULL;
    while ((xdev0 = xml_child_each(xml_find(xdevs0, "devices"), xdev0, CX_ELMNT)) != NULL) {
        if ((devname = xml_find_body(xdev0, "name")) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        cbuf_reset(cb);
        cprintf(cb, "mountpoint-%s", devname);
        newtree = cbuf_get(cb);
        if ((ph = cligen_ph_find(ch, newtree)) == NULL){
            /* No such cligen specs, query full modules once */
            if (xdevs1 == NULL){
                if (rpc_get_yanglib_mount_match(h, "*", 0, 1, &xdevs1) < 0)
                    goto done;
            }
            if (xdevs1 == NULL)
                continue;
            if ((ret = check_mtpoint_yspec(h, xdevs1, devname, newtree, &yspec1)) < 0)
                goto done;
            if (ret == 0)
                continue;
            if (yspec1 == NULL) /* Skip if not connected or disabled */
                continue;
            /* Generate auto-cligen tree from the specs */
            if (yang2cli_yspec(h, yspec1, newtree) < 0)
                goto done;
            /* Sanity (ph needed further down) */
            if ((ph = cligen_ph_find(ch, newtree)) == NULL){
                clixon_err(OE_YANG, 0, "autocli should have been generated but is not?");
                goto done;
            }
        }
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xdevs0)
        xml_free(xdevs0);
    if (xdevs1)
        xml_free(xdevs1);
    return retval;
}

/*! Generate clispec tree for devices matching pattern
 *
 * Query module-state of devices, create cligen ph tree from YANG
 *
 * Match devname against all existing devices via get-config (using depth?)
 * Find a "newtree" device name (or NULL)
 * construct a treename from that: mountpoint-<newtree>
 * If it does not exist, call a yang2cli_yspec from that
 * @param[in]  ch      CLIgen handle
 * @param[in]  pattern Device name pattern
 * @param[out] namep   New (malloced) name
 * @retval     0       OK, New malloced name in namep
 * @retval    -1       Error
 * @see controller_gentree_all  Similar but force all clispecs at start
 */
static int
controller_gentree_one(cligen_handle ch,
                       char         *pattern,
                       char        **namep)
{
    int           retval = -1;
    clixon_handle h;
    cbuf         *cb = NULL;
    cxobj        *xdevs0 = NULL;
    cxobj        *xdevs1 = NULL;
    cxobj        *xdev0;
    char         *newtree;
    yang_stmt    *yspec1 = NULL;
    char         *devname;
    pt_head      *ph;
    char         *firsttree = NULL;
    int           failed = 0;
    int           allequal = 1; /* if 0, at least 2 trees and at least two of the trees are !eq */
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    h = cligen_userhandle(ch);
    /* Get all device names */
    if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs0) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Loop through all matching devices, check if clispec exists, if not generate
     * But only for first match
     */
    xdev0 = NULL;
    while ((xdev0 = xml_child_each(xml_find(xdevs0, "devices"), xdev0, CX_ELMNT)) != NULL) {
        if ((devname = xml_find_body(xdev0, "name")) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        cbuf_reset(cb);
        cprintf(cb, "mountpoint-%s", devname);
        newtree = cbuf_get(cb);
        if (firsttree == NULL) {
            if ((firsttree= strdup(newtree)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
        }
        if ((ph = cligen_ph_find(ch, newtree)) == NULL){
            /* No such cligen specs, query full modules once
               (* is optimization so you dont need to call again) */
            if (xdevs1 == NULL){
                if (rpc_get_yanglib_mount_match(h, "*", 0, 1, &xdevs1) < 0)
                    goto done;
            }
            if (xdevs1 == NULL)
                continue;
            if ((ret = check_mtpoint_yspec(h, xdevs1, devname, newtree, &yspec1)) < 0)
                goto done;
            if (ret == 0){
                failed++;
                continue;
            }
            /* Generate auto-cligen tree from the specs */
            if (yang2cli_yspec(h, yspec1, newtree) < 0)
                goto done;
            /* Sanity (ph needed further down) */
            if ((ph = cligen_ph_find(ch, newtree)) == NULL){
                clixon_err(OE_YANG, 0, "autocli should have been generated but is not?");
                goto done;
            }
        }
        /* Check if all trees are equal, if not allequal to 0 */
        if (allequal && strcmp(firsttree, newtree) != 0){
            parse_tree *pt0 = cligen_ph_parsetree_get(cligen_ph_find(ch, firsttree));
            parse_tree *pt1 = cligen_ph_parsetree_get(ph);
            if (pt_eq1(pt0, pt1) != 0)
                allequal = 0;
        }
    }
    if (firsttree && allequal && failed == 0){
        *namep = firsttree;
        firsttree = NULL;
    }
    else { /* create dummy tree */
        if (cligen_ph_find(ch, "mointpoint") == NULL){
            parse_tree     *pt0;
            if ((ph = cligen_ph_add(ch, "mountpoint")) == NULL)
                goto done;
            if ((pt0 = pt_new()) == NULL){
                clixon_err(OE_UNIX, errno, "pt_new");
                goto done;
            }
            if (cligen_ph_parsetree_set(ph, pt0) < 0){
                clixon_err(OE_UNIX, 0, "cligen_ph_parsetree_set");
                goto done;
            }
        }
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xdevs0)
        xml_free(xdevs0);
    if (xdevs1)
        xml_free(xdevs1);
    if (firsttree)
        free(firsttree);
    return retval;
}

/*! CLIgen wrap function for making treeref lookup: generate clispec tree from YANG
 *
 * This adds an indirection based on name and context
 * If a yang and specific tree is created, the name of that tree is returned in namep,
 * That tree is called something like mountpoint-<device-name>
 * otherwise the generic name "mountpoint" is used.
 * @param[in]  ch    CLIgen handle
 * @param[in]  name  Base tree name
 * @param[in]  cvt   Tokenized string: vector of tokens
 * @param[in]  arg   Argument given when registering wrap function (maybe not needed?)
 * @param[out] namep New (malloced) name
 * @retval     1     New malloced name in namep
 * @retval     0     No wrapper, use existing
 * @retval    -1     Error
 * @see yang2cli_container  where @mountpoint is added as a generic treeref causing this call
 */
static int
controller_treeref_wrap(cligen_handle ch,
                        char         *name,
                        cvec         *cvt,
                        void         *arg,
                        char        **namep)
{
    int           retval = -1;
    cg_var       *cv;
    cg_var       *cvdev = NULL;
    char         *pattern;
    clixon_handle h;
    cvec         *cvv_edit;

    if (namep == NULL){
        clixon_err(OE_UNIX, EINVAL, "Missing namep");
        goto done;
    }
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
    /* Pattern can be globbed, its the <name> argument to devices */
    if ((pattern = cv_string_get(cvdev)) == NULL)
        goto ok;
    if (controller_gentree_one(ch, pattern, namep) < 0)
        goto done;
    if (*namep == NULL)
        goto ok;
    retval = 1;
 done:
    return retval;
 ok:
    retval = 0;
    goto done;
}

/*! YANG schema mount, query backend of yangs
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
 * @param[in]  xmt     XML mount-point in XML tree
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
controller_cli_yang_mount(clixon_handle   h,
                          cxobj          *xmt,
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
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if (xml_nsctx_node(xmt, &nsc) < 0)
        goto done;
    if (xml2xpath(xmt, nsc, 1, 1, &xpath) < 0)
        goto done;
    /* xmt can be rooted somewhere else than "/devices" , such as /rpc-reply */
    if ((str = strstr(xpath, "/devices/device")) == NULL)
        goto ok;
    cprintf(cb, "%s", str);
    recursion++;
    /* First xpath is on mount-point (to get config) */
    /* XXX: why not use /yanglib:yang-library directly ?*/
    if (clicon_rpc_get2(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", 0, &xt) < 0){
        recursion--;
        goto done;
    }
    recursion--;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "clicon_rpc_get");
        goto done;
    }
    /* Second xpath is specific on module-set */
    if (xml_nsctx_add(nsc, "yanglib", "urn:ietf:params:xml:ns:yang:ietf-yang-library") < 0)
        goto done;
    cprintf(cb, "/yanglib:yang-library/yanglib:module-set[yanglib:name='mount']");
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

/*! CLIgen history callback function, each added command makes a callback
 *
 * Could be used for logging all CLI commands for example, or just mirroring the history file
 * Note this is not exactly the same as the history command file, since it filters equal
 * commands
 * @param[in]  h     CLIgen handle
 * @param[in]  cmd   CLI command. Do not modify or free
 * @param[in]  arg   Argument given when registering
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
cli_history_cb(cligen_handle ch,
               char         *cmd,
               void         *arg)
{
    clixon_handle h = arg;

    return clixon_log(h, LOG_INFO, "command(%s): %s", clicon_username_get(h), cmd);
}

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init,
    controller_cli_start,
    controller_cli_exit,
    .ca_yang_mount = controller_cli_yang_mount,
    .ca_version    = controller_version,
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    .ca_yang_patch = controller_yang_patch_junos,
#endif
};

/*! CLI plugin initialization
 *
 * @param[in]  h    Clixon handle
 * @retval     NULL Error
 * @retval     api  Pointer to API struct
 * @retval     0    OK
 * @retval    -1    Error
 */
clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    struct timeval tv;
    int            argc; /* command-line options (after --) */
    char         **argv;
    int            c;

    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec);
    /* Get user command-line options (after --) */
    if (clicon_argv_get(h, &argc, &argv) < 0)
        goto done;
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, "g")) != -1)
        switch (c) {
        case 'g':
            gentree_expand_all = 1;
            break;
        }
    /* Register treeref wrap function */
    if (clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        cligen_tree_resolve_wrapper_set(cli_cligen(h), controller_treeref_wrap, NULL);
    }
    /* Log CLI commands */
    cligen_hist_fn_set(cli_cligen(h), cli_history_cb, h);
    return &api;
 done:
    return NULL;
}
