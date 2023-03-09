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
#include "controller_transaction.h"
#include "controller_device_recv.h"

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
                 conn_state_t  conn_state)
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

/*! Check if there is a location=NETCONF in list
 *
 * @param[in]  dh  Clixon client handle
 * @param[in]  xd  XML tree of netconf monitor schema entry
 * @retval     1   OK, location-netconf
 * @retval     0   No location netconf
 * @see ietf-netconf-monitoring@2010-10-04.yang
 */
static int
schema_check_location_netconf(cxobj *xd)
{
    int      retval = 0;
    cxobj   *x;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    x = NULL;
    while ((x = xml_child_each(xd, x, CX_ELMNT)) != NULL) {
        if (strcmp("location", xml_name(x)) != 0)
            continue;
        if (xml_body(x) && strcmp("NETCONF", xml_body(x)) == 0)
            break;
    }
    if (x == NULL)
        goto skip;
    retval = 1;
 skip:
    return retval;
}

/*! Translate from RFC 6022 schemalist to RFC8525 yang-library
 */
static int
schema_list2yang_library(cxobj  *xschemas,
                         cxobj **xyanglib)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;
    char  *identifier;
    char  *version;
    char  *format;
    char  *namespace;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">");
    cprintf(cb, "<module-set>");
    cprintf(cb, "<name>mount</name>");
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            continue;
        if ((identifier = xml_find_body(x, "identifier")) == NULL ||
            (version = xml_find_body(x, "version")) == NULL ||
            (namespace = xml_find_body(x, "namespace")) == NULL ||
            (format = xml_find_body(x, "format")) == NULL)

            continue;
        if (strcmp(format, "yang") != 0)
            continue;
        if (schema_check_location_netconf(x) == 0)
            continue;
        cprintf(cb, "<module>");
        cprintf(cb, "<name>%s</name>", identifier);
        cprintf(cb, "<revision>%s</revision>", version);
        cprintf(cb, "<namespace>%s</namespace>", namespace);
        cprintf(cb, "</module>");
    }
    cprintf(cb, "</module-set>");
    cprintf(cb, "</yang-library>");
    /* Need yspec to make YB_MODULE */
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, xyanglib, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
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
device_state_recv_hello(clixon_handle h,
                        device_handle dh,
                        int           s,
                        cxobj        *xmsg,
                        char         *rpcname,
                        conn_state_t  conn_state)
{
    int     retval = -1;
    char   *rpcprefix;
    char   *namespace = NULL;
    int     version;
    cvec   *nsc = NULL;
    cxobj  *xcaps;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "hello") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                   rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh,  "No appropriate namespace associated with %s",
                   namespace);
        goto closed;
    }
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    if ((xcaps = xpath_first(xmsg, nsc, "/hello/capabilities")) == NULL){
        clicon_err(OE_PROTO, ESHUTDOWN, "No capabilities found");
        goto done;
    }
    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xcaps) < 0)
        goto done;
    if (device_handle_capabilities_set(dh, xcaps) < 0)
        goto done;
    /* Set NETCONF version */
    if (device_handle_capabilities_find(dh, NETCONF_BASE_CAPABILITY_1_1))
        version = 1;
    else if (device_handle_capabilities_find(dh, NETCONF_BASE_CAPABILITY_1_0))
        version = 0;
    else{
        device_close_connection(dh,  "No base netconf capability found");
        goto closed;
    }
    clicon_debug(1, "%s version: %d", __FUNCTION__, version);
    version = 0; /* XXX hardcoded to 0 */
    clicon_option_int_set(h, "netconf-framing", version);
    /* Send hello */
    if (clixon_client_hello(s, version) < 0)
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

/*! Get a config from device, write to db file without sanity of yang checks
 *
 * @param[in] h          Clixon handle.
 */
static int
device_config_write(clixon_handle h,
                    device_handle dh,
                    char         *name,
                    char         *extended,
                    cxobj        *xdata,
                    cbuf         *cbret)
{
    int    retval = -1;
    cbuf  *cbdb = NULL;
    char  *db;
    int    ret;

    if ((cbdb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }   
    if (extended)
        cprintf(cbdb, "device-%s-%s", name, extended);
    else
        cprintf(cbdb, "device-%s", name);
    db = cbuf_get(cbdb);
    if (xmldb_db_reset(h, db) < 0)
        goto done;
    if ((ret = xmldb_put(h, db, OP_REPLACE, xdata, clicon_username_get(h), cbret)) < 0)
        goto done;
    if (ret == 0){
        if (device_close_connection(dh, "Failed to sync db: %s", cbuf_get(cbret)) < 0)
            goto done;
        goto closed;
    }
    retval = 1;
 done:
    if (cbdb)
        cbuf_free(cbdb);
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
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
int
device_state_recv_config(clixon_handle h,
                         device_handle dh,
                         cxobj        *xmsg,
                         yang_stmt    *yspec0,
                         char         *rpcname,
                         conn_state_t  conn_state)
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
    int                     ret;
    cxobj                  *x;
    cxobj                  *xroot;
    yang_stmt              *yroot;
    cxobj                  *xerr = NULL;
    uint64_t                tid;
    controller_transaction *ct;
    int                     merge = 0;
    int                     dryrun = 0;

    clicon_debug(1, "%s", __FUNCTION__);
    //    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((xdata = xml_find_type(xmsg, NULL, "data", CX_ELMNT)) == NULL){
        device_close_connection(dh, "No data in get reply");
        goto closed;
    }

    /* Move all xmlns declarations to <data> */
    if (xmlns_set_all(xdata, nsc) < 0)
        goto done;
    xml_sort(xdata);
    name = device_handle_name_get(dh);
    if ((cbret = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
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
        clicon_err(OE_YANG, 0, "Device root is not a YANG schema mount-point");
        goto done;
    }
    if ((yspec1 = device_handle_yspec_get(dh)) == NULL){
        device_close_connection(dh, "No YANGs available");
        goto closed;
    }
    /*
     * <root>  clixon-controller:root
     * <data>  ietf-netconf:data (dont bother to bind this node, its just a placeholder)
     * <x>     bind to yspec1
     */
    if ((ret = xml_bind_yang(h, xdata, YB_MODULE, yspec1, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if ((cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cberr, "YANG binding failed at mountpoint:");
        if ((x=xpath_first(xerr, NULL, "//error-message"))!=NULL)
            cprintf(cberr, "%s", xml_body(x));
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
    /* Add op=OP_REPLACE to root mountpoint */
    if ((xa = xml_new("operation", xroot, CX_ATTR)) == NULL)
        goto done;
    if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
        goto done;
    if ((tid = device_handle_tid_get(dh)) != 0 &&
        (ct = controller_transaction_find(h, tid)) != NULL){
        merge = ct->ct_merge;
        dryrun = ct->ct_dryrun;
    }
    if (merge){
        if (xml_value_set(xa, xml_operation2str(OP_MERGE)) < 0)
            goto done;
    }
    else{
        if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
            goto done;
    }
    if (dryrun){
        if ((ret = device_config_write(h, dh, name, "dryrun", xt, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto closed;
        goto ok;
    }
    if ((ret = xmldb_put(h, "candidate", OP_NONE, xt, NULL, cbret)) < 0)
        goto done;
    if (ret && (ret = candidate_commit(h, NULL, "candidate", 0, 0, cbret)) < 0)
        goto done;
    if (ret == 0){ /* discard */
        xmldb_copy(h, "running", "candidate");            
        xmldb_modified_set(h, "candidate", 0); /* reset dirty bit */
        clicon_debug(CLIXON_DBG_DEFAULT, "%s", cbuf_get(cbret));
        if (device_close_connection(dh, 
#if 0
                                    /* XXX cbret is XML and looks ugly in logmsg (at least encode it?)*/
                                    "Failed to commit: %s", cbuf_get(cbret)
#else
                                    "Failed to commit"
#endif
                                    ) < 0)
            goto done;
        goto closed;
    }
    else {
        device_handle_sync_time_set(dh, NULL);
    }
    if ((ret = device_config_write(h, dh, name, NULL, xt, cbret)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
 ok:
    retval = 1;
 done:
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
 */
int
device_state_recv_schema_list(device_handle dh,
                              cxobj        *xmsg,
                              char         *rpcname,
                              conn_state_t  conn_state)
{
    int    retval = -1;
    cxobj *xschemas = NULL;
    cxobj *x;
    cxobj *x1;
    cxobj *xyanglib = NULL;
    int    ret;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    /* Difficult to use xpath here since prefixes are not known */
    if ((x = xml_find_type(xmsg, NULL, "data", CX_ELMNT)) != NULL &&
        (x1 = xml_find_type(x, NULL, "netconf-state", CX_ELMNT)) != NULL &&
        (xschemas = xml_find_type(x1, NULL, "schemas", CX_ELMNT)) != NULL)
        ;
    else{
        device_close_connection(dh, "No schemas returned");
        goto closed;
    }

    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xschemas) < 0)
        goto done;
    /* "Wash" it from other elements: eg. junos may sneak in errors
     * XXX Maybe this can be skipped ? v
     */
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            xml_flag_set(x, XML_FLAG_MARK);
#ifdef CONTROLLER_JUNOS_SKIP_METADATA
        else if (strcmp(xml_find_body(x,"identifier"), "junos-configuration-metadata") == 0)
            xml_flag_set(x, XML_FLAG_MARK);
#endif
    }
    if (xml_tree_prune_flags(xschemas, XML_FLAG_MARK, XML_FLAG_MARK) < 0)
        goto done;
    /* Translate to RFC 8525 */
    if (schema_list2yang_library(xschemas, &xyanglib) < 0)
        goto done;
    if (xml_rootchild(xyanglib, 0, &xyanglib) < 0)
        goto done;
    if (device_handle_yang_lib_set(dh, xyanglib) < 0)
        goto done;
    retval = 1;
 done:
    if (xschemas)
        xml_free(xschemas);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive RFC 6022 get-schema and write to local yang file
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
device_state_recv_get_schema(device_handle dh,
                             cxobj        *xmsg,
                             char         *rpcname,
                             conn_state_t  conn_state)
{
    int         retval = -1;
    char       *ystr;
    char       *ydec = NULL;
    char       *modname;
    char       *revision = NULL;
    cbuf       *cb = cbuf_new();
    FILE       *f = NULL;
    size_t      sz;
    yang_stmt  *yspec = NULL;
    int         ret;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((ystr = xml_find_body(xmsg, "data")) == NULL){
        device_close_connection(dh, "Invalid get-schema, no YANG body");
        goto closed;
    }
    if (xml_chardata_decode(&ydec, "%s", ystr) < 0)
        goto done;
    sz = strlen(ydec);
    revision = device_handle_schema_rev_get(dh);
    modname = device_handle_schema_name_get(dh);
    /* Write to file */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s/%s", YANG_SCHEMA_MOUNT_DIR, modname);
    if (revision)
        cprintf(cb, "@%s", revision);
    cprintf(cb, ".yang");
    clicon_debug(1, "%s: Write yang to %s", __FUNCTION__, cbuf_get(cb));
    if ((f = fopen(cbuf_get(cb), "w")) == NULL){
        clicon_err(OE_UNIX, errno, "fopen(%s)", cbuf_get(cb));
        goto done;
    }
    if (fwrite(ydec, 1, sz, f) != sz){
        clicon_err(OE_UNIX, errno, "fwrite");
        goto done;
    }
    fflush(f);
    retval = 1;
 done:
    if (yspec)
        ys_free(yspec);
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

/*! Controller input wresp to open state handling, read rpc-reply
 *
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
int
device_state_recv_ok(device_handle dh,
                     cxobj        *xmsg,
                     char         *rpcname,
                     conn_state_t  conn_state)
{
    int    retval = -1;
    int    ret;
    cxobj *xerr;
    cxobj *x;

    if ((ret = rpc_reply_sanity(dh, xmsg, rpcname, conn_state)) < 0)
        goto done;
    if (ret == 0)
        goto closed;
    if ((xerr = xpath_first(xmsg, NULL, "rpc-error")) != NULL){
        x = xpath_first(xerr, NULL, "error-message");
        device_close_connection(dh, "Error %s in state %s",
                                x?xml_body(x):"reply",
                                device_state_int2str(conn_state));
        goto closed;
    }
    if (xml_find_type(xmsg, NULL, "ok", CX_ELMNT) == NULL){
        device_close_connection(dh, "No ok in reply in state %s",
                                device_state_int2str(conn_state));
        goto closed;
    }
    retval = 1;
 done:
    return retval;
 closed:
    retval = 0;
    goto done;
}

