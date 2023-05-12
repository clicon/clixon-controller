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


/*! There is not auto cligen tree "treename", create it
 *
 * 1. Check if yang controller extension/unknown mount-pint exists (yu)
 * 2. Create xpath to specific mountpoint given by devname
 * 3. Check if yspec associated to that mountpoint exists
 * 4. Get yang specs of mountpoint from controller
 * 5. Parse YANGs locally from the yang specs
 * 6. Generate auto-cligen tree from the specs 
 * @param[in]  h         Clicon handle
 * @param[in]  xdev      XML device tree 
 * @param[in]  devname   Device name
 * @param[in]  treename  Autocli treename
 * @param[out] yspec1p   yang spec
 * @retval     0         Ok
 * @retval    -1         Error
 */
static int
create_autocli_mount_tree(clicon_handle h,
                          cxobj        *xdev,
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
    char      *devname;
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
    devname = xml_find_body(xdev, "name");
    cprintf(cb, "/ctrl:devices/ctrl:device[ctrl:name='%s']/ctrl:config", devname);
    xpath = cbuf_get(cb);
    /* 3. Check if yspec associated to that mountpoint exists */
    if (yang_mount_get(yu, xpath, &yspec1) < 0)
        goto done;
    if (yspec1 == NULL){
        if ((yspec1 = yspec_new()) == NULL)
            goto done;
        /* 4. Get yang specs of mountpoint from controller */
        yanglib = xpath_first(xdev, 0, "config/yang-library");
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
    char         *pattern;
    char         *newtree;
    char         *firsttree = NULL;
    cbuf         *cb = NULL;
    yang_stmt    *yspec1 = NULL;
    clicon_handle h;
    cvec         *cvv_edit;
    cxobj        *xdevs = NULL;
    cxobj        *xdev;
    pt_head      *ph;

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
    if ((pattern = cv_string_get(cvdev)) == NULL)
        goto ok;
    /* Pattern match all devices (mountpoints) into xdevs
     * Match devname against all existing devices via get-config (using depth?)
     * Find a "newtree" device name (or NULL)
     * construct a treename from that: mountpoint-<newtree>
     * If it does not exist, call a yang2cli_yspec from that
     * create_autocli_mount_tree() should probably be rewritten
     */
    if (rpc_get_yanglib_mount_match(h, pattern, 1, &xdevs) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Loop through all matching devices, check if clispec exists, if not generate
     * But only for first match
     */
    xdev = NULL;
    while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
        if ((devname = xml_find_body(xdev, "name")) == NULL)
            continue;
        cbuf_reset(cb);
        cprintf(cb, "mountpoint-%s", devname);
        newtree = cbuf_get(cb);
        if (firsttree == NULL &&
            (firsttree= strdup(newtree)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        if (cligen_ph_find(ch, newtree) != NULL)
            continue;
        /* No such cligen specs, generate them */
        if (create_autocli_mount_tree(h, xdev, newtree, &yspec1) < 0)
            goto done;
        if (yspec1 == NULL){
            clicon_err(OE_YANG, 0, "No yang spec");
            goto done;
        }
        if (strcmp(firsttree, newtree) == 0){
            /* XXX: Only first match (one could generate all?) */
            /* Generate auto-cligen tree from the specs */
            if (yang2cli_yspec(h, yspec1, newtree) < 0)
                goto done;
            /* Sanity */
            if (cligen_ph_find(ch, newtree) == NULL){
                clicon_err(OE_YANG, 0, "autocli should have been generated but is not?");
                goto done;
            }
        }
    }
    if (namep){
        if (firsttree){
            *namep = firsttree;
            firsttree = NULL;
        }
        else{ /* create dummy tree */
            if (cligen_ph_find(ch, "mointpoint") == NULL){
                parse_tree     *pt0;
                if ((ph = cligen_ph_add(ch, "mountpoint")) == NULL)
                    goto done;
                if ((pt0 = pt_new()) == NULL){
                    clicon_err(OE_UNIX, errno, "pt_new");
                    goto done;
                }
                if (cligen_ph_parsetree_set(ph, pt0) < 0){
                    clicon_err(OE_UNIX, 0, "cligen_ph_parsetree_set");
                    goto done;
                }                
            }
        }
    }
    retval = 1;
 done:
    if (firsttree)
        free(firsttree);
    if (xdevs)
        xml_free(xdevs);
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

