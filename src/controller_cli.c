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
static int controller_gentree_pattern(cligen_handle ch, char *pattern, char **namep);

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
        if (controller_gentree_pattern(cli_cligen(h), "*", NULL) < 0)
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
    int      retval = -1;
    int      s;
    cbuf    *cb = NULL;
    char    *username;
    uint32_t session_id = 0;

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
        if (clicon_rpc_msg(h, cb, NULL) < 0)
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
    return retval;
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

/*! Get yanglib from xpath and nsc of mountpoint
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xpath   XPath of mountpoint
 * @param[in]  nsc     Namespace context of xpath
 * @param[out] xylib   New XML tree on the form <yang-library><module-set><module>*, caller frees
 * @retval     1       OK
 * @retval     0       Skip
 * @retval    -1       Error
 */
static int
xpath2yanglib(clixon_handle h,
              char         *xpath,
              cvec         *nsc,
              cxobj       **xylibp)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;
    cxobj *xerr;
    cxobj *xylib = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s", xpath);
    /* xpath to mount-point (to get config) */
    /* XXX: why not use /yanglib:yang-library directly ?
     * A: because you cannot get state-only (across mount-point?)
     */
    if (clixon_rpc_get1(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", YB_NONE, &xt) < 0)
        goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "clicon_rpc_get");
        goto done;
    }
    /* xpath to module-set */
    if (xml_nsctx_add(nsc, "yanglib", "urn:ietf:params:xml:ns:yang:ietf-yang-library") < 0)
        goto done;
    /* Append yang-library cb */
    cprintf(cb, "/yanglib:yang-library");
    if ((xylib = xpath_first(xt, nsc, "%s", cbuf_get(cb))) == NULL)
        goto skip;
    xml_rm(xylib);
    if (controller_yang_library_bind(h, xylib) < 0)
        goto done;
    *xylibp = xylib;
    xylib = NULL;
    retval = 1;
 done:
    if (xylib)
        xml_free(xylib);
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
    return retval;
 skip:
    retval = 0;
    goto done;
}

/*! Given device XML, get yanglib, create xmount and return yspec
 *
 * Given xdev,
 * Get xpath, and yanglib,
 * Create and add xmnt to xdev, yang bind
 * Given yanglib get yspec
 * @param[in]  h        Clixon handle
 * @param[in]  xdev     Device XML on form: <device><name>...
 * @param[out] xyanglib Yang library in XML form
 * @retval     0        OK
 * @retval    -1        Error
 */
static int
controller_xdev2yanglib(clixon_handle h,
                        cxobj        *xdev,
                        cxobj       **xyanglib)
{
    int        retval = -1;
    cvec      *nsc = NULL;
    cbuf      *cb = NULL;
    char      *xpath;
    char      *devname;
    int        ret;

    if ((devname = xml_find_body(xdev,"name")) == NULL)
        goto ok;
    if (xml_nsctx_node(xdev, &nsc) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "/devices/device[name='%s']/config", devname);
    xpath = cbuf_get(cb);
    if ((ret = xpath2yanglib(h, xpath, nsc, xyanglib)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! Generate clispec tree for devices matching pattern
 *
 * Query module-state of devices, create cligen ph tree from YANG
 * If namep given, return first treename created (can be many)
 * Can be called at startup using -- -g to fully expand mounted yangs and clispecs.
 * Caveat: if backend have not connected to devices, do not create new yspec
 * @param[in]  ch      CLIgen handle
 * @param[in]  pattern Device name pattern
 * @param[out] namep   New (malloced) name
 * @retval     0       OK, New malloced name in namep
 * @retval    -1       Error
 */
static int
controller_gentree_pattern(cligen_handle ch,
                           char         *pattern,
                           char        **namep)
{
    int           retval = -1;
    clixon_handle h;
    cbuf         *cb = NULL;
    cxobj        *xdevs0 = NULL;
    cxobj        *xdev;
    char         *devname;
    pt_head      *ph;
    char         *newtree;
    yang_stmt    *yspec1 = NULL;
    char         *firsttree = NULL;
    int           allequal = 1; /* if 0, at least 2 trees and at least two of the trees are !eq */
    parse_tree   *pt0;
    parse_tree   *pt1;
    cxobj        *xyanglib = NULL;
    cxobj        *xmnt;
    yang_stmt    *ydev;
    yang_stmt    *ymod;
    char         *digest = NULL;
    autocli_cache_t cache = AUTOCLI_CACHE_DISABLED;
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL, "%s", pattern);
    h = cligen_userhandle(ch);
    /* Get all device names in pattern */
    if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs0) < 0)
        goto done;
    if (autocli_cache(h, &cache, NULL) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Loop through all matching devices, check if clispec exists, if not generate
     * But only for first match
     */
    xdev = NULL;
    while ((xdev = xml_child_each(xml_find(xdevs0, "devices"), xdev, CX_ELMNT)) != NULL) {
        if ((devname = xml_find_body(xdev, "name")) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        cbuf_reset(cb);
        cprintf(cb, "mountpoint-%s", devname);
        newtree = cbuf_get(cb);
        if (namep && firsttree == NULL) {
            if ((firsttree = strdup(newtree)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
        }
        if ((ph = cligen_ph_find(ch, newtree)) == NULL){
            /* No such cligen parse-tree, create xmnt, get yanglib, get yspec */
            if ((xmnt = xml_new("config", xdev, CX_ELMNT)) == NULL)
                goto done;
            xyanglib = NULL;
            /* Get xyanglib using xdev from backend */
            if (controller_xdev2yanglib(h, xdev, &xyanglib) < 0)
                goto done;
            if (xyanglib == NULL){
                if (firsttree){
                    free(firsttree);
                    firsttree = NULL;
                }
                continue;
            }
            /* Bind xmnt to yang */
            if ((ydev = xml_spec(xdev)) == NULL){
                clixon_err(OE_YANG, 0, "device not bound to yang");
                goto done;
            }
            if ((ymod = ys_module(ydev)) == NULL){
                clixon_err(OE_YANG, 0, "No module");
                goto done;
            }
            if (xml_bind_special(xmnt, ymod, "/devices/device/config") < 0)
                goto done;
            /* Parse yanglib and get yspec */
            if ((ret = yang_schema_yanglib_mount_parse(h, xmnt, xyanglib, 1, &yspec1)) < 0)
                goto done;
            if (ret == 0 || yspec1 == NULL){ /* Skip if disabled */
                if (firsttree){
                    free(firsttree);
                    firsttree = NULL;
                }
                if (xyanglib){
                    xml_free(xyanglib);
                    xyanglib = NULL;
                }
                continue;
            }
            /* Generate auto-cligen tree from the specs */
            switch (cache){
            case AUTOCLI_CACHE_DISABLED: /* Generate locally */
                if (yang2cli_yspec(h, yspec1, newtree) < 0)
                    goto done;
            break;
            case AUTOCLI_CACHE_READ: /* Query backend */
                if (xyanglib_digest(xyanglib, &digest) < 0)
                    goto done;
                if (yang2cli_yanglib(h, digest, xyanglib, newtree) < 0)
                    goto done;
                if (xyanglib){
                    xml_free(xyanglib);
                    xyanglib = NULL;
                }
                if (digest){
                    free(digest);
                    digest = NULL;
                }
                break;
            }
            /* Sanity (ph needed further down) */
            if ((ph = cligen_ph_find(ch, newtree)) == NULL){
                clixon_err(OE_YANG, 0, "autocli should have been generated but is not?");
                goto done;
            }
        }
        if (namep) {
            /* Check if all trees are equal, if not allequal to 0 */
            if (allequal && strcmp(firsttree, newtree) != 0){
                pt0 = cligen_ph_parsetree_get(cligen_ph_find(ch, firsttree));
                pt1 = cligen_ph_parsetree_get(ph);
                if (pt_eq1(pt0, pt1) != 0)
                    allequal = 0;
            }
        }
    }
    if (namep) {
        if (firsttree && allequal){
            if (namep)
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
    }
    if (!allequal) // XXX May occur more than once
        clixon_log(h, LOG_WARNING, "Devices matching %s including %s also match other devices with different YANGs",
                   pattern, firsttree);
    retval = 0;
 done:
    if (digest)
        free(digest);
    if (xyanglib)
        xml_free(xyanglib);
    if (cb)
        cbuf_free(cb);
    if (xdevs0)
        xml_free(xdevs0);
    if (firsttree)
        free(firsttree);
    return retval;
}

/*! CLIgen wrap function for making device treeref lookup: generate clispec tree from YANG
 *
 * @param[in]  ch    CLIgen handle
 * @param[in]  name  Base tree name
 * @param[in]  cvt   Tokenized string: vector of tokens
 * @param[in]  arg   Argument given when registering wrap function (maybe not needed?)
 * @param[out] namep New (malloced) name, if changed
 * @retval     0     OK
 * @retval    -1     Error
 * @note: contains ad-hoc algorithm to find moint-point
 */
static int
controller_yang2cli_mount(cligen_handle ch,
                          const char   *name,
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

    h = cligen_userhandle(ch);
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
        if (cv != NULL)
            cvdev = cvec_next(cvt, cv);
    }
    /* Pattern can be globbed, its the <name> argument to devices */
    if (cvdev != NULL &&
        (pattern = cv_string_get(cvdev)) != NULL){
        if (controller_gentree_pattern(ch, pattern, namep) < 0)
            goto done;
    }
    retval = 0;
 done:
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
controller_yang2cli_wrap(cligen_handle ch,
                         const char   *name,
                         cvec         *cvt,
                         void         *arg,
                         char        **namep)
{
    int   retval = -1;
    char *keyword = NULL;
    int   ret;

    clixon_debug(CLIXON_DBG_CLI, "%s", name);
    if (namep == NULL){
        clixon_err(OE_UNIX, EINVAL, "Missing namep");
        goto done;
    }
    if (strcmp(name, "mountpoint") == 0){
        if ((ret = controller_yang2cli_mount(ch, name, cvt, arg, namep)) < 0)
            goto done;
        if (namep && *namep){
            retval = 1;
            goto done;
        }
    }
    else {
        if (yang2cli_treeref_decode(name, AUTOCLI_CMD_DELIM,
                                    NULL, NULL, NULL, NULL, &keyword, NULL) < 0)
            goto done;
        if (keyword && strcmp(keyword, "grouping") == 0){
            if (yang2cli_grouping_wrap(ch, name, cvt, arg, namep) < 0)
                goto done;
        }
    }
#if 1 /* Backward compatible with cligen 7.6 tree-resolve() */
    if (namep && *namep){
        retval = 1;
    }
    else
#endif
        retval = 0;
 done:
    if (keyword)
        free(keyword);
    return retval;
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
 * @see device_send_get_schema_list/device_recv_schema_list Backend fns for send/rcv
 */
int
controller_cli_yang_mount(clixon_handle   h,
                          cxobj          *xmt,
                          int            *config,
                          validate_level *vl,
                          cxobj         **xyanglib)
{
    int    retval = -1;
    char  *xpath = NULL;
    char  *str;
    cvec  *nsc = NULL;
    int    ret;

    if (xml_nsctx_node(xmt, &nsc) < 0)
        goto done;
    if (xml2xpath(xmt, nsc, 1, 1, &xpath) < 0)
        goto done;
    /* xmt can be rooted somewhere else than "/devices" , such as /rpc-reply */
    if ((str = strstr(xpath, "/devices/device")) == NULL)
        goto ok;
    clixon_debug(CLIXON_DBG_CTRL, "%s", str);
    /* get modset */
    ret = xpath2yanglib(h, str, nsc, xyanglib);
    if (ret < 0)
        goto done;
    if (ret == 0)
        goto ok;
 ok:
    retval = 0;
 done:
    if (xpath)
        free(xpath);
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
               const char   *cmd,
               void         *arg)
{
    clixon_handle h = arg;
    int           flags;

    /* Trick to not echo history to terminal */
    flags = clixon_logflags_get();
    if ((flags & CLIXON_LOG_STDERR) != 0x0)
        clixon_logflags_set(flags & ~CLIXON_LOG_STDERR);
    clixon_log(h, LOG_INFO, "command(%s): %s", clicon_username_get(h), cmd);
    if ((flags & CLIXON_LOG_STDERR) != 0x0)
        clixon_logflags_set(flags);
    return 0;
}

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init,
    controller_cli_start,
    controller_cli_exit,
    .ca_yang_mount = controller_cli_yang_mount,
    .ca_version    = controller_version,
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
        cligen_tree_resolve_wrapper_set(cli_cligen(h), controller_yang2cli_wrap, NULL);
    }
    /* Log CLI commands (note filtering in cli_history_cb to stderr */
    cligen_hist_fn_set(cli_cligen(h), cli_history_cb, h);
    /* Register "ctrl" as a debug key */
    if (clixon_debug_key_add("ctrl", CLIXON_DBG_APP) < 0)
        goto done;
    return &api;
 done:
    return NULL;
}
