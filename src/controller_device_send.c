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
  * Routines for sending netconf messages to devices
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_netconf.h"
#include "controller_lib.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"

/*! Send a <lock>/<unlock> target candidate
 *
 * @param[in]  h    Clixon handle
 * @param[in]  dh   Clixon client handle
 * @param[in]  lock 0:Unlock, 1:lock
 * @retval     0    OK
 * @retval    -1    Error
 */
int
device_send_lock(clixon_handle h,
                 device_handle dh,
                 int           lock)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   s;

    if (lock != 0 && lock != 1){
        clixon_err(OE_UNIX, EINVAL, "lock is not 0 or 1");
        goto done;
    }
    s = device_handle_socket_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE,
            device_handle_msg_id_getinc(dh));
#ifdef NETCONF_LOCK_EXTRA_NAMESPACE
    cprintf(cb, "<%slock xmlns=\"%s\">", lock==0?"un":"", NETCONF_BASE_NAMESPACE);
#else
    cprintf(cb, "<%slock>", lock==0?"un":"");
#endif
    cprintf(cb, "<target><candidate/></target>");
    cprintf(cb, "</%slock>", lock==0?"un":"");
    cprintf(cb, "</rpc>");
    if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
        if (clixon_msg_send11(s, device_handle_name_get(dh), cb) < 0)
            goto done;
    }
    else if (clixon_msg_send10(s, device_handle_name_get(dh), cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send a <get>/<get-config> request to a device
 *
 * @param[in]  h     Clixon handle
 * @param[in]  dh    Clixon client handle
 * @param[in]  s     Socket
 * @param[in]  state 0: config, 1: config+state
 * @param[in]  xpath XPath (experimental, unclear semantics)
 * @retval     0     OK
 * @retval    -1     Error
 */
int
device_send_get(clixon_handle h,
                device_handle dh,
                int           s,
                int           state,
                char         *xpath)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE,
            device_handle_msg_id_getinc(dh));
    if (state) {
        cprintf(cb, "<get>");
        if (xpath)
            cprintf(cb, "<filter type=\"xpath\" select=\"%s\"/>", xpath); // XXX xmlns
        cprintf(cb, "</get>");
    }
    else {
        cprintf(cb, "<get-config>");
        cprintf(cb, "<source><running/></source>");
        cprintf(cb, "</get-config>");
    }
    cprintf(cb, "</rpc>");
    s = device_handle_socket_get(dh);
    if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
        if (clixon_msg_send11(s, device_handle_name_get(dh), cb) < 0)
            goto done;
    }
    else if (clixon_msg_send10(s, device_handle_name_get(dh), cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send s single get-schema requests to a device
 *
 * @param[in]  h   Clixon handle
 * @param[in]  dh  Clixon client handle
 * @param[in]  s   Socket
 * @param[in]  xd  XML tree of netconf monitor schema entry
 * @retval     0   OK, sent a get-schema request
 * @retval    -1   Error
 * @see ietf-netconf-monitoring@2010-10-04.yang
 */
static int
device_get_schema_sendit(clixon_handle h,
                         device_handle dh,
                         int           s,
                         char         *identifier,
                         char         *version)
{
    int      retval = -1;
    cbuf    *cb = NULL;
    uint64_t seq;
    char    *name;

    name = device_handle_name_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    seq = device_handle_msg_id_getinc(dh);
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, seq);
    cprintf(cb, "<get-schema xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<identifier>%s</identifier>", identifier);
    cprintf(cb, "<version>%s</version>", version?version:"");
    cprintf(cb, "<format>yang</format>");
    cprintf(cb, "</get-schema>");
    cprintf(cb, "</rpc>");
    if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
        if (clixon_msg_send11(s, device_handle_name_get(dh), cb) < 0)
            goto done;
    }
    else if (clixon_msg_send10(s, device_handle_name_get(dh), cb) < 0)
        goto done;
    clixon_debug(CLIXON_DBG_CTRL, "%s: sent get-schema(%s@%s) seq:%" PRIu64, name, identifier, version, seq);
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Find next schema in list, check if already loaded, or exists locally,
 *
 * If not send request to device
 * @param[in]     h   Clixon handle
 * @param[in]     dh  Clixon client handle
 * @param[in]     s   Socket
 * @param[in,out] nr  Last schema index sent
 * @retval        2   Sent a get-schema, nr updated
 * @retval        1   Error, device closed
 * @retval        0   No schema sent, either because all are sent or they are none, or cberr set
 * @retval       -1   Error
 * @see device_recv_get_schema  Receive a schema request
 * @see device_schemas_mount_parse   Parse the module after if already found or received
 */
int
device_send_get_schema_next(clixon_handle h,
                            device_handle dh,
                            int           s,
                            int          *nr)
{
    int        retval = -1;
    int        ret;
    cxobj     *xylib;
    cxobj     *x;
    char      *name;
    char      *revision;
    yang_stmt *yspec = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    cvec      *nsc = NULL;
    int        i;
    char      *domain;
    char      *location;

    clixon_debug(CLIXON_DBG_CTRL|CLIXON_DBG_DETAIL, "%d", *nr);
    if (controller_mount_yspec_get(h, device_handle_name_get(dh), &yspec) < 0)
        goto done;
    if (yspec == NULL){
        clixon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    if ((domain = device_handle_domain_get(dh)) == NULL){
        clixon_err(OE_YANG, 0, "No YANG domain");
        goto done;
    }
    xylib = device_handle_yang_lib_get(dh);
    x = NULL;
    if (xpath_vec(xylib, nsc, "module-set/module", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        x = vec[i];
        if (i != *nr)
            continue;
        name = xml_find_body(x, "name");
        revision = xml_find_body(x, "revision");
        (*nr)++;
        /* Check if already loaded */
        if (yang_find_module_by_name_revision(yspec, name, revision) != NULL)
            continue;
        /* Check if exists as local file */
        if ((ret = yang_file_find_match(h, name, revision, domain, NULL)) < 0)
            goto done;
        if (ret == 1)
            continue;
        location = xml_find_body(x, "location");
        if (location == NULL || strcmp(location, "NETCONF") != 0){
            device_close_connection(dh, "Module: %s: Unsupported location:%s", name, location);
            retval = 1;
            goto done;
        }
        /* May be some concurrency here if several devices requests same yang simultaneously
         * To avoid that one needs to keep track if another request has been sent.
         */
        if ((ret = device_get_schema_sendit(h, dh, s, name, revision)) < 0)
            goto done;
        device_handle_schema_name_set(dh, name);
        device_handle_schema_rev_set(dh, revision);
        break;
    }
    if (i < veclen)
        retval = 2;
    else
        retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Send ietf-netconf-monitoring schema get request to get list of schemas
 *
 * @param[in]  h      Clixon handle.
 * @param[in]  dh     Clixon client handle.
 * @retval     0      OK
 * @retval    -1      Error
 * @note this could be part of the generic sync, but juniper seems to need
 * an explicit to target the schemas (and only that)
 * @see device_recv_schema_list  where the reply is received
 */
int
device_send_get_schema_list(clixon_handle h,
                            device_handle dh,
                            int           s)
{
    int   retval = -1;
    cbuf *cb = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE,
            device_handle_msg_id_getinc(dh));
    cprintf(cb, "<get>");
    cprintf(cb, "<filter type=\"subtree\">");
    cprintf(cb, "<netconf-state xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<schemas/>");
    cprintf(cb, "</netconf-state>");
    cprintf(cb, "</filter>");
    cprintf(cb, "</get>");
    cprintf(cb, "</rpc>");
    if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
        if (clixon_msg_send11(s, device_handle_name_get(dh), cb) < 0)
            goto done;
    }
    else if (clixon_msg_send10(s, device_handle_name_get(dh), cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! As part of creating edt-config, remove subtree under xn
 *
 * That is, for operation="remove/delete"
 * Do not remove keys if xn is LIST
 * Remove body if xn is LEAF.
 * @param[in]  xn   XML node
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
device_edit_config_remove_subtree(cxobj *xn)
{
    int        retval = -1;
    cvec      *cvk; /* vector of index keys */
    cg_var    *cvi = NULL;
    char      *keyname;
    yang_stmt *yn;
    cxobj     *xsub;

    if ((yn = xml_spec(xn)) != NULL){
        switch (yang_keyword_get(yn)){
        case Y_LIST:
            cvk = yang_cvec_get(xml_spec(xn)); /* Use Y_LIST cache, see ys_populate_list() */
            while ((cvi = cvec_each(cvk, cvi)) != NULL) {
                keyname = cv_string_get(cvi);
                if ((xsub = xml_find_type(xn, NULL, keyname, CX_ELMNT)) != NULL)
                    xml_flag_set(xsub, XML_FLAG_MARK);
            }
            break;
        case Y_LEAF:
            /* If leaf, remove body,
             *   See https://github.com/clicon/clixon-controller/issues/203
             *  Not an element, not removed by prune_flagged below
             */
            if ((xsub = xml_body_get(xn)) != NULL)
                xml_purge(xsub);
            break;
        default:
            break;
        }
    }
    /* Remove all non-key children */
    if (xml_tree_prune_flagged_sub(xn, XML_FLAG_MARK, 1, NULL) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Create edit-config to a device given a diff between two XML trees x0 and x1
 *
 * 1. Add netconf operation attributes to add/del/change nodes in x0 and x1 and mark
 * 2. Remove all unmarked nodes, ie unchanged nodes
 * 3. Merge deleted nodes in x0 with added/changed nods in x1 into x0
 * 4. Create an edit-config message and parse it
 * 5. Add diff-tree to an outgoing netconf edit-config
 * Used for sync push
 * @param[in]  h       Clixon handle.
 * @param[in]  dh      Clixon client handle.
 * @param[in]  x0      Original tree
 * @param[in]  x1      New tree
 * @param[in]  yspec   Yang spec of device mount
 * @param[in]  dvec    Delete xml vector (in x0)
 * @param[in]  dlen;   Delete xml vector length
 * @param[in]  avec    Add xml vector  (in x1)
 * @param[in]  alen    Add xml vector length
 * @param[in]  chvec0  Source changed xml vector
 * @param[in]  chvec1  Target changed xml vector
 * @param[in]  chlen   Changed xml vector length
 * @param[out] cbret1  Buffer containing first edit-config
 * @param[out] cbret2  Buffer containing second edit-config
 * @retval     0       OK
 * @retval    -1       Error
 * XXX Lots of xml and cbuf handling, try to contain some parts in sub-functions
 * XXX validation
 */
int
device_create_edit_config_diff(clixon_handle h,
                               device_handle dh,
                               cxobj        *x0,
                               cxobj        *x1,
                               yang_stmt    *yspec,
                               cxobj       **dvec,
                               int           dlen,
                               cxobj       **avec,
                               int           alen,
                               cxobj       **chvec0,
                               cxobj       **chvec1,
                               int           chlen,
                               cbuf        **cbret1,
                               cbuf        **cbret2)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    int     i;
    cxobj  *xn;
    cxobj  *xa;

    clixon_debug(CLIXON_DBG_CTRL, "");
    /* 1. Add netconf operation attributes to add/del/change nodes in x0 and x1 and mark */
    for (i=0; i<dlen; i++){
        xn = dvec[i];
        if ((xa = xml_new("operation", xn, CX_ATTR)) == NULL)
            goto done;
        if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
            goto done;
        if (xml_value_set(xa, xml_operation2str(OP_REMOVE)) < 0)
            goto done;
        xml_flag_set(xn, XML_FLAG_MARK);
        /* Remove any subtree under xn (except for list keys) */
        if (device_edit_config_remove_subtree(xn) < 0)
            goto done;
    }
    for (i=0; i<alen; i++){
        xn = avec[i];
        if ((xa = xml_new("operation", xn, CX_ATTR)) == NULL)
            goto done;
        if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
            goto done;
        if (xml_value_set(xa, xml_operation2str(OP_MERGE)) < 0)
            goto done;
        xml_flag_set(xn, XML_FLAG_MARK);
    }
    for (i=0; i<chlen; i++){
        xn = chvec1[i];
        if ((xa = xml_new("operation", xn, CX_ATTR)) == NULL)
            goto done;
        if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
            goto done;
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0) // XXX
            goto done;
        xml_flag_set(xn, XML_FLAG_MARK);
    }
    /* 2. Remove all unmarked nodes, ie unchanged nodes */
    if (xml_tree_prune_flagged_sub(x0, XML_FLAG_MARK, 1, NULL) < 0)
        goto done;
    if (xml_tree_prune_flagged_sub(x1, XML_FLAG_MARK, 1, NULL) < 0)
        goto done;
    // XXX validate
    /* 4. Create two edit-config messages and parse them */
    if (dlen) {
        if ((cb = cbuf_new()) == NULL){
            clixon_err(OE_PLUGIN, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "<rpc xmlns=\"%s\" xmlns:nc=\"%s\" message-id=\"%" PRIu64 "\">",
                NETCONF_BASE_NAMESPACE,
                NETCONF_BASE_NAMESPACE,
                device_handle_msg_id_getinc(dh)
                );
        cprintf(cb, "<edit-config>");
        cprintf(cb, "<target><candidate/></target>");
        cprintf(cb, "<default-operation>none</default-operation>");
        cprintf(cb, "<config>");
#ifdef CLIXON_PLUGIN_USERDEF
        /* Rewrite x0 */
        if (clixon_plugin_userdef_all(h, CTRL_NX_SEND, x0, dh) < 0)
            goto done;
#endif
        if (clixon_xml2cbuf(cb, x0, 0, 0, NULL, -1, 1) < 0)
            goto done;
        cprintf(cb, "</config>");
        cprintf(cb, "</edit-config>");
        cprintf(cb, "</rpc>");
        *cbret1 = cb;
        cb = NULL;
    }
    if (alen || chlen) {
        if ((cb = cbuf_new()) == NULL){
            clixon_err(OE_PLUGIN, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "<rpc xmlns=\"%s\" xmlns:nc=\"%s\" message-id=\"%" PRIu64 "\">",
                NETCONF_BASE_NAMESPACE,
                NETCONF_BASE_NAMESPACE,
                device_handle_msg_id_getinc(dh)
                );
        cprintf(cb, "<edit-config>");
        cprintf(cb, "<target><candidate/></target>");
        cprintf(cb, "<default-operation>none</default-operation>");
#ifdef CLIXON_PLUGIN_USERDEF
        /* Rewrite x1 */
        if (clixon_plugin_userdef_all(h, CTRL_NX_SEND, x1, dh) < 0)
            goto done;
#endif
        cprintf(cb, "<config>");
        if (clixon_xml2cbuf(cb, x1, 0, 0, NULL, -1, 1) < 0)
            goto done;
        cprintf(cb, "</config>");
        cprintf(cb, "</edit-config>");
        cprintf(cb, "</rpc>");
        *cbret2 = cb;
        cb = NULL;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send NETCONF RPC to device
 *
 * @param[in]  h       Clixon handle.
 * @param[in]  dh      Clixon client handle.
 * @param[in]  s       Netconf socket
 * @param[in]  msgbody NETCONF RPC message fields (not including header)
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
device_send_rpc(clixon_handle h,
                device_handle dh,
                char         *msgbody)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   s;

    clixon_debug(CLIXON_DBG_CTRL, "%s", msgbody);
    s = device_handle_socket_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE,
            device_handle_msg_id_getinc(dh));
    if (msgbody)
        cprintf(cb, "%s", msgbody);
    cprintf(cb, "</rpc>");
    if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
        if (clixon_msg_send11(s, device_handle_name_get(dh), cb) < 0)
            goto done;
    }
    else if (clixon_msg_send10(s, device_handle_name_get(dh), cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send NETCONF validate to device
 */
int
device_send_validate(clixon_handle h,
                     device_handle dh)
{
    return device_send_rpc(h, dh, "<validate><source><candidate/></source></validate>");
}

/*! Send commit to device
 */
int
device_send_commit(clixon_handle h,
                   device_handle dh)
{
    return device_send_rpc(h, dh, "<commit/>");
}

/*! Send discard-changes to device
 */
int
device_send_discard_changes(clixon_handle h,
                            device_handle dh)
{
    return device_send_rpc(h, dh, "<discard-changes/>");
}

/*! Send generic RPC to device
 *
 * @param[in]  h       Clixon handle
 * @param[in]  dh      Device handle
 * @param[in]  xconfig RPC xml body
 * @retval     0       OK
 * @retval    -1       Error
 * @see device_recv_generic_rpc
 */
int
device_send_generic_rpc(clixon_handle h,
                        device_handle dh,
                        cxobj        *xconfig)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xconfig, 0, 0, NULL, -1, 1) < 0)
        goto done;
    if (device_send_rpc(h, dh, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}
