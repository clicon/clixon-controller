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
  * Routines for receiving netconf messages from devices
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
#include <sys/stat.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_transaction.h"
#include "controller_device_recv.h"
#include "controller_transaction.h"

/* Forward declaration */
static int
device_recv_check_errors(clixon_handle h,
                         device_handle dh,
                         cxobj        *xmsg,
                         conn_state    conn_state,
                         cbuf        **cberr);

/*! Check sanity of a rpc-reply
 *
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
rpc_reply_sanity(device_handle dh,
                 cxobj        *xmsg,
                 char         *rpcname,
                 conn_state    conn_state)
{
    int   retval = -1;
    cvec *nsc = NULL;
    char *rpcprefix;
    char *namespace;

    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                                rpcname, device_state_int2str(conn_state));
    }
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh, "No appropriate namespace associated with:%s",
                                namespace);
        goto closed;
    }
    retval = 1;
 done:
    if (nsc)
        cvec_free(nsc);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive hello from device, send hello
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
int
device_recv_hello(clixon_handle h,
                  device_handle dh,
                  int           s,
                  cxobj        *xmsg,
                  char         *rpcname,
                  conn_state    conn_state)
{
    int    retval = -1;
    char  *rpcprefix;
    char  *namespace = NULL;
    cvec  *nsc = NULL;
    cxobj *xcapabilities;

    clixon_debug(CLIXON_DBG_CTRL|CLIXON_DBG_DETAIL, "");
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "hello") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                   rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh, "No appropriate namespace associated with %s",
                   namespace);
        goto closed;
    }
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    // XXX not prefix/namespace independent
    if ((xcapabilities = xpath_first(xmsg, nsc, "/hello/capabilities")) == NULL){
        clixon_err(OE_PROTO, ESHUTDOWN, "No capabilities found");
        goto done;
    }
    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xcapabilities) < 0)
        goto done;
    if (device_handle_capabilities_set(dh, xcapabilities) < 0)
        goto done;
    retval = 1;
 done:
   if (nsc)
       cvec_free(nsc);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive config data from device and add config to mount-point
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state
 * @param[in] force_transient If set, always save config in TRANSIENT db, regardless of dh setting
 * @param[in] force_merge If set, always merge db
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
int
device_recv_config(clixon_handle h,
                   device_handle dh,
                   cxobj        *xmsg,
                   yang_stmt    *yspec0,
                   char         *rpcname,
                   conn_state    conn_state,
                   int           force_transient,
                   int           force_merge)
{
    int                     retval = -1;
    cxobj                  *xdata;
    cxobj                  *xt = NULL;
    cxobj                  *xa;
    cbuf                   *cbret = NULL;
    cbuf                   *cberr = NULL;
    char                   *name;
    cvec                   *nsc = NULL;
    yang_stmt              *yspec1;
    cxobj                  *x;
    cxobj                  *xroot;
    yang_stmt              *yroot;
    cxobj                  *xerr = NULL;
    uint64_t                tid;
    controller_transaction *ct;
    int                     merge = 0;
    int                     transient = 0;
    cxobj                  *xt1 = NULL;
    char                   *db = NULL;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((xdata = xml_find_type(xmsg, NULL, "data", CX_ELMNT)) == NULL){
        device_close_connection(dh, "No data in get reply");
        goto closed;
    }
    if (clixon_plugin_userdef_all(h, CTRL_NX_RECV, xdata, dh) < 0)
        goto done;
    /* Move all xmlns declarations to <data> */
    if (xmlns_set_all(xdata, nsc) < 0)
        goto done;
    xml_sort(xdata);
    name = device_handle_name_get(dh);
    if ((cbret = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Create config tree (xt) and device mount-point (xroot) */
    if (device_state_mount_point_get(name, yspec0, &xt, &xroot) < 0)
        goto done;
    yroot = xml_spec(xroot);
    /* Sanity-check mount-point extension */
    if ((ret = yang_schema_mount_point(yroot)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_YANG, 0, "Device root is not a YANG schema mount-point");
        goto done;
    }
    yspec1 = NULL;
    if (controller_mount_yspec_get(h, name, &yspec1) < 0)
        goto done;
    if (yspec1 == NULL){
        device_close_connection(dh, "No YANGs available");
        goto closed;
    }
    /*
     * <config> clixon-controller:root
     * <data>  ietf-netconf:data (dont bother to bind this node, its just a placeholder)
     * <x>     bind to yspec1
     */
    if ((ret = xml_bind_yang(h, xdata, YB_MODULE, yspec1, 0, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if ((cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cberr, "Device %s in state %s, mismatch between XML and YANG when reading running config from device: ", name, device_state_int2str(conn_state));
        // xpath not prefix/namespace independent
        if (xerr && (x = xpath_first(xerr, NULL, "rpc-error/error-message")) != NULL){
            if (netconf_err2cb(h,
                               xml_find_type(xerr, NULL, "rpc-error", CX_ELMNT),
                               cberr) < 0)
                goto done;
        }
        if (device_close_connection(dh, "%s", cbuf_get(cberr)) < 0)
            goto done;
        goto closed;
    }
    /* Add all xdata children to xroot
     * XXX:
     * 1. idempotent?
     * 2. should state nodes be added (they do now)?
     */
    while ((x = xml_child_i_type(xdata, 0, CX_ELMNT)) != NULL) {
        if (xml_addsub(xroot, x) < 0)
            goto done;
    }
    if (xml_sort_recurse(xroot) < 0)
        goto done;
    /* Add op=OP_REPLACE to root mountpoint (this is stripped by xmldb_put) */
    if ((xa = xml_new("operation", xroot, CX_ATTR)) == NULL)
        goto done;
    if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
        goto done;
    if (xml_sort(xroot) < 0)
        goto done;
    /* Special handling if part of transaction. XXX: currently not activated */
    if ((tid = device_handle_tid_get(dh)) == 0) {
        clixon_debug(CLIXON_DBG_CTRL, "tid is 0, shouldnt happen");
        if (device_close_connection(dh, "Tid is zero") < 0)
            goto done;
        goto closed;
    }
    if ((ct = controller_transaction_find(h, tid)) == NULL){
        clixon_debug(CLIXON_DBG_CTRL, "ct is NULL, shouldnt happen");
        if (device_close_connection(dh, "ct is NULL") < 0)
            goto done;
        goto closed;
    }
    merge = ct->ct_pull_merge;
    transient = ct->ct_pull_transient;
    if (force_transient)
        transient = 1;
    if (force_merge)
        merge = 1;
    if (merge){
        if (xml_value_set(xa, xml_operation2str(OP_MERGE)) < 0)
            goto done;
    }
    else{
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
            goto done;
    }
    if (transient){
        if ((ret = device_config_write(h, name, "TRANSIENT", xt, cbret)) < 0)
            goto done;
        if (ret == 0){
            if (device_close_connection(dh, "%s", cbuf_get(cbret)) < 0)
                goto done;
            goto closed;
        }
        goto ok;
    }
    /* Must make a copy: xmldb_put strips attributes */
    if ((xt1 = xml_dup(xt)) == NULL)
        goto done;
    /* 1. Put device config change to tmp */
    if ((ret = xmldb_put(h, "tmpdev", OP_NONE, xt, NULL, cbret)) < 0)
        goto done;
    if (ret == 0){ /* discard */
        clixon_debug(CLIXON_DBG_CTRL, "%s", cbuf_get(cbret));
        if (device_close_connection(dh, "Failed to commit: %s", cbuf_get(cbret)) < 0)
            goto done;
        goto closed;
    }
    device_handle_sync_time_set(dh, NULL);
    /* 2. Put same to candidate */
    if (xmldb_candidate_find(h, "candidate", ct->ct_client_id, NULL, &db) < 0)
        goto done;
    if (db == NULL){
        clixon_debug(CLIXON_DBG_CTRL, "candidate not found");
        if (device_close_connection(dh, "Failed to commit: candidate not found") < 0)
            goto done;
        goto closed;
    }
    /* This is where existing config is overwritten
     * One could have a warning here, but that would require a diff
     */
    if ((ret = xmldb_put(h, db, OP_NONE, xt1, NULL, cbret)) < 0)
        goto done;
    if (ret && (ret = device_config_write(h, name, "SYNCED", xt, cbret)) < 0)
        goto done;
    if (ret == 0){
        if (device_close_connection(dh, "%s", cbuf_get(cbret)) < 0)
            goto done;
        goto closed;
    }
    device_handle_sync_time_set(dh, NULL);
 ok:
    retval = 1;
 done:
    if (xt1)
        xml_free(xt1);
    if (xt)
        xml_free(xt);
    if (xerr)
        xml_free(xerr);
    if (cberr)
        cbuf_free(cberr);
    if (cbret)
        cbuf_free(cbret);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive netconf-state schema list from device using RFC 6022 state
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 * @see device_send_get_schema_list  where the request is sent
 */
int
device_recv_schema_list(device_handle dh,
                        cxobj        *xmsg,
                        char         *rpcname,
                        conn_state    conn_state)
{
    int           retval = -1;
    cxobj        *xschemas = NULL;
    cxobj        *x;
    cxobj        *x1;
    cxobj        *xyanglib = NULL;
    clixon_handle h;
    cbuf         *cb = NULL;
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    h = device_handle_handle_get(dh);
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((ret = device_recv_check_errors(h, dh, xmsg, conn_state, &cb)) < 0)
        goto done;
    if (ret == 0){
        device_close_connection(dh, "Get netconf-state/schemas failed: %s", cbuf_get(cb));
        goto closed;
    }
    /* Difficult to use xpath here since prefixes are not known */
    if ((x = xml_find_type(xmsg, NULL, "data", CX_ELMNT)) != NULL &&
        (x1 = xml_find_type(x, NULL, "netconf-state", CX_ELMNT)) != NULL &&
        (xschemas = xml_find_type(x1, NULL, "schemas", CX_ELMNT)) != NULL)
        ;
    else {
        device_close_connection(dh, "No schemas returned in state %s, no data/netconf-state/schemas found",
                                device_state_int2str(conn_state));
        goto closed;
    }
    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xschemas) < 0)
        goto done;
    /* "Wash" it from other elements: eg. junos may sneak in errors
     * XXX Maybe this can be skipped ?
     */
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            xml_flag_set(x, XML_FLAG_MARK);
    }
    if (xml_tree_prune_flags(xschemas, XML_FLAG_MARK, XML_FLAG_MARK) < 0)
        goto done;
    /* Translate to RFC 8525 */
    if (schema_list2yang_library(h, xschemas, device_handle_domain_get(dh), &xyanglib) < 0)
        goto done;
    if (xml_rootchild(xyanglib, 0, &xyanglib) < 0)
        goto done;
    /* @see controller_connect where initial yangs may be set
     */
    if (device_handle_yang_lib_append(dh, xyanglib) < 0) /* xyanglib consumed */
        goto done;
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    if (xschemas)
        xml_free(xschemas);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive RFC 6022 get-schema and write to local yang file
 *
 * Local dir is CLICON_YANG_DOMAIN_DIR/domain and is created if it does not exist.
 * Get data payload as YANG and write to file.
 * Decode yang using CDATA or regular XML character decoding
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 * @see device_send_get_schema_next  Check local/ send a schema request
 * @see device_schemas_mount_parse   Parse the module after it is found or requested
 */
int
device_recv_get_schema(device_handle dh,
                       cxobj        *xmsg,
                       char         *rpcname,
                       conn_state    conn_state)
{
    int           retval = -1;
    clixon_handle h;
    char         *ystr;
    char         *ydec = NULL;
    char         *modname;
    char         *revision = NULL;
    cbuf         *cb = NULL;
    FILE         *f = NULL;
    size_t        sz;
    char         *dir;
    char         *file;
    char         *domain;
    struct stat   st0;
    struct stat   st1;
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    h = device_handle_handle_get(dh);
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((ystr = xml_find_body(xmsg, "data")) == NULL){
        device_close_connection(dh, "Invalid get-schema, no YANG body");
        goto closed;
    }
    /* Check if CDATA */
    if (strncmp(ystr, "<![CDATA[", strlen("<![CDATA[")) == 0){
        if (strncmp(&ystr[strlen(ystr)-strlen("]]>")], "]]>", strlen("]]>")) != 0){
            device_close_connection(dh, "CDATA encoding but not CDATA trailer");
            goto closed;
        }
        if ((ydec = strdup(ystr + strlen("<![CDATA["))) == NULL){
            clixon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        ydec[strlen(ydec)-strlen("]]>")] = '\0';
    }
    else if (xml_chardata_decode(&ydec, "%s", ystr) < 0)
        goto done;
    sz = strlen(ydec);
    revision = device_handle_schema_rev_get(dh);
    modname = device_handle_schema_name_get(dh);
    /* Write to file */
    if ((domain = device_handle_domain_get(dh)) == NULL){
        clixon_err(OE_YANG, 0, "No YANG domain");
        goto done;
    }
    if ((dir = clicon_yang_domain_dir(h)) == NULL){
        clixon_err(OE_YANG, 0, "CLICON_YANG_DOMAIN_DIR not set");
        goto done;
    }
    if (stat(dir, &st0) < 0){
        clixon_err(OE_YANG, errno, "%s not found", dir);
        goto done;
    }
    /* Check top dir  */
    if (S_ISDIR(st0.st_mode) == 0){
        clixon_err(OE_YANG, errno, "%s not directory", dir);
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s/%s", dir, domain);
    dir = cbuf_get(cb);
    if (stat(dir, &st1) < 0){
        /* Create domain dir and copy mods from top-dir  */
        if (mkdir(dir, st0.st_mode) < 0){
            clixon_err(OE_UNIX, errno, "mkdir %s ", dir);
            goto done;
        }
        if (chown(dir, st0.st_uid, st0.st_gid) < 0){
            clixon_err(OE_UNIX, errno, "chown %s ", dir);
            goto done;
        }
    }
    cprintf(cb, "/%s", modname);
    if (revision)
        cprintf(cb, "@%s", revision);
    cprintf(cb, ".yang");
    file = cbuf_get(cb);
    clixon_debug(CLIXON_DBG_CTRL, "Write yang to %s", file);
    if ((f = fopen(cbuf_get(cb), "w")) == NULL){
        clixon_err(OE_UNIX, errno, "fopen(%s)", file);
        goto done;
    }
    if (fwrite(ydec, 1, sz, f) != sz){
        clixon_err(OE_UNIX, errno, "fwrite");
        goto done;
    }
    fflush(f);
    retval = 1;
 done:
    if (f)
        fclose(f);
    if (cb)
        cbuf_free(cb);
    if (ydec)
        free(ydec);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Loop through all replies, if error stop, if only warning continue
 *
 * @param[in]  h          Clixon handle.
 * @param[in]  dh         Clixon client handle.
 * @param[out] cberr      Error, free with cbuf_err (retval = 0)
 * @retval     1          OK
 * @retval     0          Failed
 * @retval    -1          Error
 */
static int
device_recv_check_errors(clixon_handle h,
                         device_handle dh,
                         cxobj        *xmsg,
                         conn_state    conn_state,
                         cbuf        **cberr)
{
    int                      retval = -1;
    cxobj                   *xerr;
    cxobj                   *x;
    char                    *b;
    uint64_t                tid;
    cbuf                   *cb = NULL;
    controller_transaction *ct;

    /* Loop through all replies, if error stop, if only warning continue */
    xerr = NULL;
    while ((xerr = xml_child_each(xmsg, xerr, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(xerr), "rpc-error") == 0){
            if ((x = xml_find_type(xerr, NULL, "error-severity", CX_ELMNT)) != NULL &&
                (b = xml_body(x)) != NULL &&
                strcmp(b, "warning") == 0){
                if ((tid = device_handle_tid_get(dh)) != 0 &&
                    (ct = controller_transaction_find(h, tid)) != NULL &&
                    ct->ct_warning == NULL){
                    x = xml_find_type(xerr, NULL, "error-message", CX_ELMNT);
                    if ((cb = cbuf_new()) == NULL){
                        clixon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    cprintf(cb, "Device %s in state %s:",
                            device_handle_name_get(dh),
                            device_state_int2str(conn_state));
                    if (netconf_err2cb(h, xerr, cb) < 0)
                        goto done;
                    if ((ct->ct_warning = strdup(cbuf_get(cb))) == NULL){
                        clixon_err(OE_UNIX, EINVAL, "strdup");
                        goto done;
                    }
                    cbuf_free(cb);
                    cb = NULL;
                }
            }
            else { /* assume error */
                if ((cb = cbuf_new()) == NULL){
                    clixon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                cprintf(cb, "Device %s in state %s:",
                        device_handle_name_get(dh),
                        device_state_int2str(conn_state));
                if (netconf_err2cb(h, xerr, cb) < 0)
                    goto done;
                if (cberr){
                    *cberr = cb;
                    cb = NULL;
                }
                goto failed;
            }
        }
    }
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Controller input wresp to open state handling, read rpc-reply
 *
 * @param[in]  h          Clixon handle.
 * @param[in]  dh         Clixon client handle.
 * @param[in]  xmsg       XML tree of incoming message
 * @param[in]  yspec      Yang top-level spec
 * @param[in]  rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in]  conn_state Device connection state
 * @param[out] cberr      Error, free with cbuf_err (retval = 0)
 * @retval     2          OK
 * @retval     1          Closed
 * @retval     0          Failed: received rpc-error or not <ok> (not closed)
 * @retval    -1          Error
 */
int
device_recv_ok(clixon_handle h,
               device_handle dh,
               cxobj        *xmsg,
               char         *rpcname,
               conn_state    conn_state,
               cbuf        **cberr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xerr;
    int    ret;

    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((ret = device_recv_check_errors(h, dh, xmsg, conn_state, cberr)) < 0)
        goto done;
    if (ret == 0)
        goto failed;
    if (xml_find_type(xmsg, NULL, "ok", CX_ELMNT) == NULL){
        if ((cb = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "Unexpected reply from %s in state %s:",
                device_handle_name_get(dh),
                device_state_int2str(conn_state));
        /* XXX: following fn does not support prefixes properly, so the err msg from XML does not appear as it should */
        if ((xerr = xml_find(xmsg, "rpc-error")) != NULL){
            if (netconf_err2cb(h, xerr, cb) < 0)
                goto done;
        }
        else{
            if (clixon_xml2cbuf(cb, xmsg, 0, 0, NULL, -1, 1) < 0)
                goto done;
        }
        if (cberr){
            *cberr = cb;
            cb = NULL;
        }
        goto failed;
    }
    retval = 2;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 failed:
    retval = 0;
    goto done;
 closed:
    retval = 1;
    goto done;
}

/*! Controller input rpc reply of generic(any) rpc
 *
 * @param[in]  h          Clixon handle.
 * @param[in]  dh         Clixon client handle.
 * @param[in]  ct         Controller transaction
 * @param[in]  xmsg       XML tree of incoming message
 * @param[in]  yspec      Yang top-level spec
 * @param[in]  rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in]  conn_state Device connection state
 * @param[out] cberr      Error, free with cbuf_err (retval = 0)
 * @retval     2          OK
 * @retval     1          Closed
 * @retval     0          Failed: received rpc-error or not <ok> (not closed)
 * @retval    -1          Error
 * @see device_send_generic_rpc
 */
int
device_recv_generic_rpc(clixon_handle           h,
                        device_handle           dh,
                        controller_transaction *ct,
                        cxobj                  *xmsg,
                        char                   *rpcname,
                        conn_state              conn_state,
                        cbuf                  **cberr)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   ret;

    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((ret = device_recv_check_errors(h, dh, xmsg, conn_state, cberr)) < 0)
        goto done;
    if (ret == 0)
        goto failed;
    if ((ret = transaction_devdata_add(h, ct, device_handle_name_get(dh), xmsg, cberr)) < 0)
        goto done;
    if (ret == 0)
        goto failed;
    retval = 2;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 failed:
    retval = 0;
    goto done;
 closed:
    retval = 1;
    goto done;
}
