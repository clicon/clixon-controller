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

/*! Called when application is "started", (almost) all initialization is complete
 *
 * Create a global transaction notification handler and socket
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
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
    clicon_data_set(h, "session-transport", "cl:cli");
    if (clicon_rpc_create_subscription(h, "controller-transaction", NULL, &s) < 0)
        goto done;
    if (clicon_data_int_set(h, "controller-transaction-notify-socket", s) < 0)
        goto done;
    clicon_debug(CLIXON_DBG_DEFAULT, "%s notification socket:%d", __FUNCTION__, s);
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

/*! Check if there is another equivalent xyanglib and if so reuse that yspec
 *
 * Prereq: schema-list (xyanglib) is completely known. 
 * Look for an existing equivalent schema-list among other devices.
 * If found, re-use that YANG-SPEC.
 * @param[in]  h         Clixon handle
 * @param[in]  dh        Clixon device handle.
 * @param[in]  xyanglib  Yang-lib in XML format
 * @param[out] yspec1    Yang-spec to use, new or shared with previously created
 * @retval     0         OK
 * @retval    -1         Error
 * ßee device_shared_yspec for backend code
 */
static int
device_shared_yspec_xml(clicon_handle h,
                        cxobj        *xdev0,
                        cxobj        *xyanglib0,
                        yang_stmt   **yspec1)
{
    int        retval = -1;
#ifdef SHARED_PROFILE_YSPEC
    yang_stmt *yspec = NULL;
    cxobj     *xdevs;
    cxobj     *xdev;
    cxobj     *xyanglib;
    char      *devname;

    if ((xdevs = xml_parent(xdev0)) == NULL){
        clicon_err(OE_XML, 0, "Device has no parent");
        goto done;
    }
    xdev = NULL;
    while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
        if (xdev == xdev0)
            continue;
        if ((xyanglib = xpath_first(xdev, 0, "config/yang-library")) == NULL)
            continue;
        if (xml_tree_equal(xyanglib0, xyanglib) != 0)
            continue;
        if ((devname = xml_find_body(xdev, "name")) == NULL)
            continue;
        if (controller_mount_yspec_get(h, devname, &yspec) < 0)
            goto done;
        if (yspec != NULL){
            yang_ref_inc(yspec); /* share */
            break;
        }
    }
    if (yspec == NULL &&
        (yspec = yspec_new()) == NULL)
        goto done;
    *yspec1 = yspec;
    retval = 0;
 done:
    return retval;
#else

    if ((*yspec1 = yspec_new()) == NULL)
        goto done;
    retval = 0;
 done:
    return retval;
#endif
}

/*! There is not auto cligen tree "treename", create it
 *
 * 1. Check if yang controller extension/unknown mount-pint exists (yu)
 * 2. Create xpath to specific mountpoint given by devname
 * 3. Check if yspec associated to that mountpoint exists
 * 4. Get yang specs of mountpoint from controller
 * 5. Parse YANGs locally from the yang specs
 * 6. Generate auto-cligen tree from the specs
 * @param[in]  h         Clixon handle
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
    cbuf      *cb = NULL;
    yang_stmt *yspec1 = NULL;
    char      *devname;
    cxobj     *xyanglib = NULL;
    int        ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    devname = xml_find_body(xdev, "name");
    if (controller_mount_yspec_get(h, devname, &yspec1) < 0)
        goto done;
    if (yspec1 == NULL){
        /* 4. Get yang specs of mountpoint from controller */
        xyanglib = xpath_first(xdev, 0, "config/yang-library");
        /* 5. Check if there is another equivalent xyanglib and if so reuse that yspec */
        if (device_shared_yspec_xml(h, xdev, xyanglib, &yspec1) < 0)
            goto done;
        /* 5. Parse YANGs locally from the yang specs */
        if ((ret = yang_lib2yspec(h, xyanglib, yspec1)) < 0)
            goto done;
        if (controller_mount_yspec_set(h, devname, yspec1) < 0)
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
    int           nomatch = 0;

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
        if ((ph = cligen_ph_find(ch, newtree)) == NULL){
            /* No such cligen specs, generate them */
            if (create_autocli_mount_tree(h, xdev, newtree, &yspec1) < 0)
                goto done;
            if (yspec1 == NULL){
                clicon_err(OE_YANG, 0, "No yang spec");
                goto done;
            }
            /* Generate auto-cligen tree from the specs */
            if (yang2cli_yspec(h, yspec1, newtree) < 0)
                goto done;
            /* Sanity */
            if ((ph = cligen_ph_find(ch, newtree)) == NULL){
                clicon_err(OE_YANG, 0, "autocli should have been generated but is not?");
                goto done;
            }
        }
        /* Check if multiple trees are equal */
        if (strcmp(firsttree, newtree) != 0){
            parse_tree *pt0 = cligen_ph_parsetree_get(cligen_ph_find(ch, firsttree));
            parse_tree *pt1 = cligen_ph_parsetree_get(ph);

            if (pt_eq1(pt0, pt1) != 0)
                nomatch++;
        }
    }
    if (namep){
        if (firsttree && nomatch == 0){
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
    if (clicon_rpc_get2(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", 0, &xt) < 0){
        recursion--;
        goto done;
    }
    recursion--;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(h, xerr, "clicon_rpc_get", NULL);
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

static clixon_plugin_api api = {
    "controller",       /* name */
    clixon_plugin_init,
    controller_cli_start,
    controller_cli_exit,
    .ca_yang_mount = controller_cli_yang_mount,
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    .ca_yang_patch = controller_yang_patch_junos,
#endif
};

/*! CLI plugin initialization
 *
 * @param[in]  h    Clixon handle
 * @retval     NULL Error with clicon_err set
 * @retval     api  Pointer to API struct
 * @retval     0    OK
 * @retval    -1    Error
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
