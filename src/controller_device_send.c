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
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"

/*! Send a <get> request to a device
 *
 * @param[in]  h   Clixon handle
 * @param[in]  dh  Clixon client handle
 * @param[in]  s   Socket
 * @retval     0   OK
 * @retval    -1   Error
 */
int
device_send_get_config(clixon_handle h,
                       device_handle dh,
                       int           s)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   encap;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, 
            device_handle_msg_id_getinc(dh));
    if (1) { // get-config
        cprintf(cb, "<get-config>");
        cprintf(cb, "<source><running/></source>");
        cprintf(cb, "</get-config>");
    }
    else { // get
        cprintf(cb, "<get>");
        cprintf(cb, "</get>");
    }
    cprintf(cb, "</rpc>");
    encap = device_handle_framing_type_get(dh);
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    s = device_handle_socket_get(dh);
    if (clicon_msg_send1(s, cb) < 0)
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
    int      encap;
    char    *name;
    
    name = device_handle_name_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    seq = device_handle_msg_id_getinc(dh);
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, seq);
    cprintf(cb, "<get-schema xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<identifier>%s</identifier>", identifier);
    cprintf(cb, "<version>%s</version>", version);
    cprintf(cb, "<format>yang</format>");
    cprintf(cb, "</get-schema>");
    cprintf(cb, "</rpc>");
    encap = device_handle_framing_type_get(dh);
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    clicon_debug(1, "%s %s: sent get-schema(%s@%s) seq:%" PRIu64, __FUNCTION__, name, identifier, version, seq);
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send next get-schema requests to a device
 *
 * @param[in]     h   Clixon handle
 * @param[in]     dh  Clixon client handle
 * @param[in]     s   Socket
 * @param[in,out] nr  Last schema index sent
 * @retval        1   Sent a get-schema, nr updated
 * @retval        0   No schema sent, either because all are sent or they are none
 * @retval       -1   Error
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
    yang_stmt *yspec;
    cxobj     **vec = NULL;
    size_t      veclen;
    cvec     *nsc = NULL;
    int        i;

    clicon_debug(CLIXON_DBG_DETAIL, "%s %d", __FUNCTION__, *nr);
    if ((yspec = device_handle_yspec_get(dh)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
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
        /* Chekc if exists as local file */
        if ((ret = yang_file_find_match(h, name, revision, NULL)) < 0)
            goto done;
        if (ret == 1)
            continue;
        if ((ret = device_get_schema_sendit(h, dh, s, name, revision)) < 0)
            goto done;
        device_handle_schema_name_set(dh, name);
        device_handle_schema_rev_set(dh, revision);
        break;
    }
    if (i < veclen)
        retval = 1;
    else
        retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Send ietf-netconf-monitoring schema get request to get list of schemas
 *
 * @param[in]  h       Clixon handle.
 * @param[in]  dh      Clixon client handle.
 * @note this could be part of the generic sync, but juniper seems to need 
 * an explicit to target the schemas (and only that)
 * @see device_state_recv_schema_list  where the reply is received
 */
int
device_send_get_schema_list(clixon_handle h,
                            device_handle dh,
                            int           s)

{
    int   retval = -1;
    cbuf *cb = NULL;
    int   encap;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
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
    encap = device_handle_framing_type_get(dh);
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
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
 * @param[out] cbret   Cligen  buf containing the whole message (not sent)
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
                               cbuf        **cbret)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    int    encap;
    int    i;
    cxobj *xt = NULL;
    cxobj *xn;
    cxobj *xroot;
    cxobj *xa;
    char  *reason = NULL;
    int    ret;
    cvec  *nsc = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (cbret == NULL){
        clicon_err(OE_UNIX, EINVAL, "cbret is NULL");
        goto done;
    }
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
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
            goto done;
        xml_flag_set(xn, XML_FLAG_MARK);
    }
    /* 2. Remove all unmarked nodes, ie unchanged nodes */
    if (xml_tree_prune_flagged_sub(x0, XML_FLAG_MARK, 1, NULL) < 0)
        goto done;
    if (xml_tree_prune_flagged_sub(x1, XML_FLAG_MARK, 1, NULL) < 0)
        goto done;
    /* 3. Merge deleted nodes in x0 with added/changed nods in x1 (diff-tree)*/
    if ((ret = xml_merge(x0, x1, yspec, &reason)) < 0)
        goto done;
    // XXX validate
    /* 4. Create an edit-config message and parse it */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" xmlns:nc=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE,
            NETCONF_BASE_NAMESPACE, 
            device_handle_msg_id_getinc(dh));
    cprintf(cb, "<edit-config>");
    cprintf(cb, "<target><candidate/></target>");
    cprintf(cb, "<default-operation>none</default-operation>");
    cprintf(cb, "</edit-config>");
    cprintf(cb, "</rpc>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xt, NULL)) < 0)
        goto done;
    // XXX validate
    xroot = xpath_first(xt, nsc, "rpc/edit-config");
    if (xml_name_set(x0, "config") < 0)
        goto done;
    /* 5. Add diff-tree to an outgoing netconf edit-config 
     * Local tree xt and external tree x0 temporarily grafted, must be pruned directly after use
     */
    if (xml_addsub(xroot, x0) < 0)
        goto done;
    cbuf_reset(cb);
    if (clixon_xml2cbuf(cb, xt, 0, 0, NULL, -1, 1) < 0){
        xml_rm(x0);
        goto done;
    }
    xml_rm(x0);
    encap = device_handle_framing_type_get(dh);
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    *cbret = cb;
    cb = NULL;
    retval = 0;
 done:
    if (reason)
        free(reason);
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
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
    int   encap;
    int   s;

    clicon_debug(1, "%s %s", __FUNCTION__, msgbody);
    s = device_handle_socket_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, 
            device_handle_msg_id_getinc(dh));
    cprintf(cb, "%s", msgbody);
    cprintf(cb, "</rpc>");
    encap = device_handle_framing_type_get(dh);
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    if (clicon_msg_send1(s, cb) < 0)
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
