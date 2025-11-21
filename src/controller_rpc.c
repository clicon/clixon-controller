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
  * Backend rpc callbacks, see clixon-controller.yang for declarations
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
#include <assert.h>
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_rpc.h"

/* Forward */
static int traverse_device_group(clixon_handle h, cxobj *xdevs, cxobj **vec1, size_t vec1len, cxobj **vec2, size_t vec2len, cvec *devvec);

/*! Connect to device via Netconf SSH
 *
 * @param[in]  h             Clixon handle
 * @param[in]  dh            Device handle, either NULL or in closed state
 * @param[in]  user          Username for ssh login
 * @param[in]  addr          Address for ssh to connect to
 * @param[in]  port          Port for ssh to connect to
 * @param[in]  stricthostkey If set ensure strict hostkey checking. Only for ssh
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
connect_netconf_ssh(clixon_handle h,
                    device_handle dh,
                    char         *user,
                    char         *addr,
                    const char   *port,
                    int           stricthostkey)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   s;

    if (addr == NULL || dh == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "xn, addr or dh is NULL");
        goto done;
    }
    if (device_handle_conn_state_get(dh) != CS_CLOSED){
        clixon_err(OE_PLUGIN, EINVAL, "dh is not closed");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    if (device_handle_connect(dh, CLIXON_CLIENT_SSH, cbuf_get(cb), port, stricthostkey) < 0)
        goto done;
    if (device_state_set(dh, CS_CONNECTING) < 0)
        goto done;
    s = device_handle_socket_get(dh);
    device_handle_framing_type_set(dh, NETCONF_SSH_EOM); // XXX
    cbuf_reset(cb); /* reuse cb for event dbg str */
    cprintf(cb, "Netconf ssh %s", addr);
    if (clixon_event_reg_fd(s, device_input_cb, dh, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Connect to device
 *
 * Typically called from commit
 * @param[in]  h      Clixon handle
 * @param[in]  xn     XML of device config
 * @param[in]  ct     Transaction
 * @param[out] reason reason, if retval is 0
 * @retval     1      OK
 * @retval     0      Connection can not be set up, see reason
 * @retval    -1      Error
 */
static int
controller_connect(clixon_handle           h,
                   cxobj                  *xn,
                   controller_transaction *ct,
                   char                  **reason)
{
    int           retval = -1;
    char         *name;
    device_handle dh;
    char         *type;
    char         *addr;
    char         *port = "22";
    char         *user = NULL;
    char         *enablestr;
    char         *yfstr;
    char         *str;
    cxobj        *xb;
    cxobj        *xdevprofile = NULL;
    cxobj        *xmod = NULL;
    cxobj        *xconfig = NULL;
    cxobj        *xyanglib = NULL;
    int           ssh_stricthostkey = 1;
    char         *domain = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((name = xml_find_body(xn, "name")) == NULL)
        goto ok;
    dh = device_handle_find(h, name); /* can be NULL */
    if ((enablestr = xml_find_body(xn, "enabled")) == NULL)
        goto ok;
    if (strcmp(enablestr, "false") == 0){
        if ((dh = device_handle_new(h, name)) == NULL)
            goto done;
        device_handle_logmsg_set(dh, strdup("Configured down"));
        goto ok;
    }
    if (dh != NULL) {
        if (device_handle_conn_state_get(dh) != CS_CLOSED)
            goto ok;
        /* Clear yangs for domain changes, upgrade etc
         * Alt: clear in device_close_connection()
         */
        device_handle_yang_lib_set(dh, NULL);
    }
    if ((xconfig = xml_find(xn, "config")) != NULL){
        if (yang_schema_yspec_rm(h, xconfig) < 0)
            goto done;
    }
    /* Find device-profile object if any */
    if ((xb = xml_find_type(xn, NULL, "device-profile", CX_ELMNT)) != NULL)
        xdevprofile = xpath_first(xn, NULL, "../device-profile[name='%s']", xml_body(xb));
    if ((xb = xml_find_type(xn, NULL, "conn-type", CX_ELMNT)) == NULL)
        goto ok;
    /* If not explicit value (default value set) AND device-profile set, use that */
    if (xml_flag(xb, XML_FLAG_DEFAULT) &&
        xdevprofile)
        xb = xml_find_type(xdevprofile, NULL, "conn-type", CX_ELMNT);
    /* Only handle netconf/ssh */
    if ((type = xml_body(xb)) == NULL ||
        strcmp(type, "NETCONF_SSH")){
        if ((*reason = strdup("Connect failed: conn-type missing or not NETCONF_SSH")) == NULL)
            goto done;
        goto failed;
    }
    if ((addr = xml_find_body(xn, "addr")) == NULL){
        if ((*reason = strdup("Connect failed: addr missing")) == NULL)
            goto done;
        goto failed;
    }
    if ((xb = xml_find_type(xn, NULL, "user", CX_ELMNT)) == NULL &&
        xdevprofile){
        xb = xml_find_type(xdevprofile, NULL, "user", CX_ELMNT);
    }
    if (xb != NULL)
        user = xml_body(xb);
    if ((xb = xml_find_type(xn, NULL, "ssh-stricthostkey", CX_ELMNT)) == NULL ||
        xml_flag(xb, XML_FLAG_DEFAULT)){
        if (xdevprofile)
            xb = xml_find_type(xdevprofile, NULL, "ssh-stricthostkey", CX_ELMNT);
    }
    if (xb && (str = xml_body(xb)) != NULL)
        ssh_stricthostkey = strcmp(str, "true") == 0;
    if ((xb = xml_find_type(xn, NULL, "port", CX_ELMNT)) == NULL ||
        xml_flag(xb, XML_FLAG_DEFAULT)){
        if (xdevprofile)
            xb = xml_find_type(xdevprofile, NULL, "port", CX_ELMNT);
    }
    if (xb && (str = xml_body(xb)) != NULL)
        port = str;
    /* Now dh is either NULL or in closed state and with correct type
     * First create it if still NULL
     */
    if (dh == NULL &&
        (dh = device_handle_new(h, name)) == NULL)
        goto done;
    if ((xb = xml_find_type(xn, NULL, "yang-config", CX_ELMNT)) == NULL)
        goto ok;
    if (xml_flag(xb, XML_FLAG_DEFAULT) &&
        xdevprofile)
        xb = xml_find_type(xdevprofile, NULL, "yang-config", CX_ELMNT);
    if ((yfstr = xml_body(xb)) == NULL){
        if ((*reason = strdup("Connect failed: yang-config missing from device config")) == NULL)
            goto done;
        goto failed;
    }
    device_handle_yang_config_set(dh, yfstr); /* Cache yang config */
    if ((xb = xml_find_type(xn, NULL, "device-domain", CX_ELMNT)) == NULL ||
        xml_flag(xb, XML_FLAG_DEFAULT)){
        if (xdevprofile)
            xb = xml_find_type(xdevprofile, NULL, "device-domain", CX_ELMNT);
    }
    if (xb && (domain = xml_body(xb)) != NULL)
        if (device_handle_domain_set(dh, domain) < 0)
            goto done;
    /* Parse and save local methods into RFC 8525 yang-lib module-set/module */
    if ((xmod = xml_find_type(xn, NULL, "module-set", CX_ELMNT)) == NULL)
        xmod = xml_find_type(xdevprofile, NULL, "module-set", CX_ELMNT);
    if (xmod){
        if (xdev2yang_library(xmod, domain, &xyanglib) < 0)
            goto done;
        if (xyanglib){
            if (xml_rootchild(xyanglib, 0, &xyanglib) < 0)
                goto done;
            /* see device_recv_schema_list */
            if (device_handle_yang_lib_set(dh, xyanglib) < 0)
                goto done;
        }
    }
    if ((xb = xml_find_type(xn, NULL, "private-candidate", CX_ELMNT)) == NULL ||
        xml_flag(xb, XML_FLAG_DEFAULT)){
        if (xdevprofile)
            xb = xml_find_type(xdevprofile, NULL, "private-candidate", CX_ELMNT);
    }
    if (xb && (str = xml_body(xb)) != NULL && strcmp(str, "true") == 0)
        device_handle_flag_set(dh, DH_FLAG_PRIVATE_CANDIDATE);
    if ((xb = xml_find_type(xn, NULL, "netconf-framing", CX_ELMNT)) == NULL ||
        xml_flag(xb, XML_FLAG_DEFAULT)){
        if (xdevprofile)
            xb = xml_find_type(xdevprofile, NULL, "netconf-framing", CX_ELMNT);
    }
    if (xb && (str = xml_body(xb)) != NULL){
        if (strcmp(str, "1.0") == 0)
            device_handle_flag_set(dh, DH_FLAG_NETCONF_BASE10);
        else if (strcmp(str, "1.1") == 0)
            device_handle_flag_set(dh, DH_FLAG_NETCONF_BASE11);
    }
    /* Point of no return: assume errors handled in device_input_cb */
    device_handle_tid_set(dh, ct->ct_id);
    if (connect_netconf_ssh(h, dh, user, addr, port, ssh_stricthostkey) < 0) /* match */
        goto done;
 ok:
    retval = 1;
 done:
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Iterate incoming device pattern
 *
 * @param[in]  h       Clixon handle
 * @param[in]  pattern Pattern match
 * @param[in]  xdevs   Incoming RPC XML of device-group
 * @param[in]  vec     List of configured devices
 * @param[in]  veclen  Length of vec1
 * @param[out] devvec  List of matching devices on exit
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
iterate_device(clixon_handle h,
               char         *pattern,
               cxobj       **vec,
               size_t        veclen,
               cvec         *devvec)
{
    int     retval = -1;
    char   *devname;
    cxobj  *xn;
    cg_var *cv;
    int     i;

    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (xml_flag(xn, XML_FLAG_MARK) != 0x0)
            continue;
        if ((cv = cvec_add(devvec, CGV_VOID)) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_add");
            goto done;
        }
        cv_void_set(cv, xn);
        xml_flag_set(xn, XML_FLAG_MARK);
    }
    retval = 0;
 done:
    return retval;
}

/*! Iterate incoming device-group pattern
 *
 * @param[in]  h       Clixon handle
 * @param[in]  pattern Pattern match
 * @param[in]  vec1    List of configured devices
 * @param[in]  vec1len Length of vec1
 * @param[in]  vec2    List of configured device groups
 * @param[in]  vec2len Length of vec2
 * @param[out] devvec  List of matching devices on exit
 * @retval     0       OK
 * @retval    -1       Error
 * @note Just ignore duplicate recursive groups
 */
static int
iterate_device_group(clixon_handle h,
                     char         *pattern,
                     cxobj       **vec1,
                     size_t        vec1len,
                     cxobj       **vec2,
                     size_t        vec2len,
                     cvec         *devvec)
{
    int    retval = -1;
    char  *devname;
    cxobj *xn;
    int    i;

    for (i=0; i<vec2len; i++){
        xn = vec2[i];
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (xml_flag(xn, XML_FLAG_MARK) != 0x0)
            continue;
        xml_flag_set(xn, XML_FLAG_MARK);
        if (traverse_device_group(h, xn, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Traverse device-group recursively
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xdevs   Incoming RPC XML of device-group
 * @param[in]  vec1    List of configured devices
 * @param[in]  vec1len Length of vec1
 * @param[in]  vec2    List of configured device groups
 * @param[in]  vec2len Length of vec2
 * @param[out] devvec  List of matching devices on exit
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
traverse_device_group(clixon_handle h,
                      cxobj        *xdevs,
                      cxobj       **vec1,
                      size_t        vec1len,
                      cxobj       **vec2,
                      size_t        vec2len,
                      cvec         *devvec)
{
    int     retval = -1;
    cxobj  *xdev;
    char   *name;
    char   *pattern;

    xdev = NULL;
    while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
        name = xml_name(xdev);
        if (strcmp(name, "device-name") == 0){
            if ((pattern = xml_body(xdev)) == NULL)
                continue;
            if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
                goto done;
        }
        else if (strcmp(name, "device-group") == 0){
            if ((pattern = xml_body(xdev)) == NULL)
                continue;
            if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

static int
clearvec(clixon_handle h,
         cxobj       **vec,
         size_t        veclen)
{
    cxobj *xn;
    int    i;

    for (i=0; i<veclen; i++){
        xn = vec[i];
        xml_flag_reset(xn, XML_FLAG_MARK);
    }
    return 0;
}

/*! Compute diff, construct edit-config and send to device
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[in]  ct      Transaction
 * @param[in]  db      Device datastore
 * @param[out] cberr   Error message
 * @retval     1       OK
 * @retval     0       Failed, cbret set
 * @retval    -1       Error
 * @see devices_diff  for top-level all devices
 */
static int
push_device_one(clixon_handle           h,
                device_handle           dh,
                controller_transaction *ct,
                char                   *db,
                cbuf                  **cberr)
{
    int        retval = -1;
    cxobj     *x0 = NULL;
    cxobj     *x1;
    cxobj     *x1t = NULL;
    cbuf      *cb = NULL;
    char      *name;
    cxobj    **dvec = NULL;
    size_t     dlen;
    cxobj    **avec = NULL;
    size_t     alen;
    cxobj    **chvec0 = NULL;
    cxobj    **chvec1 = NULL;
    size_t     chlen;
    yang_stmt *yspec;
    cbuf      *cbmsg1 = NULL;
    cbuf      *cbmsg2 = NULL;
    cvec      *nsc = NULL;
    int        ret;

    /* Note x0 and x1 are directly modified in device_create_edit_config_diff, cannot do no-copy
       1) get previous device synced xml */
    name = device_handle_name_get(dh);
    if ((ret = device_config_read(h, name, "SYNCED", &x0, cberr)) < 0)
        goto done;
    if (ret == 0)
        goto failed;
    /* 2) get current and compute diff with previous */
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "devices/device[name='%s']/config", name);
    if (xmldb_get0(h, db, YB_MODULE, nsc, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &x1t, NULL, NULL) < 0)
        goto done;
    if ((x1 = xpath_first(x1t, nsc, "%s", cbuf_get(cb))) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(*cberr, "Device not configured");
        goto failed;
    }
    yspec = NULL;
    if (controller_mount_yspec_get(h, name, &yspec) < 0)
        goto done;
    if (yspec == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(*cberr, "No YANGs exists for device %s, is device connected? (set enabled=false)", name);
        goto failed;
    }
    /* What to push to device? diff between synced and actionsdb */
    if (xml_diff(x0, x1,
                 &dvec, &dlen,
                 &avec, &alen,
                 &chvec0, &chvec1, &chlen) < 0)
        goto done;
    /* 3) construct an edit-config, send it and validate it */
    if (dlen || alen || chlen){
        if (device_create_edit_config_diff(h, dh,
                                           x0, x1, yspec,
                                           dvec, dlen,
                                           avec, alen,
                                           chvec0, chvec1, chlen,
                                           &cbmsg1, &cbmsg2) < 0)
            goto done;
        if (cbmsg1)
            device_handle_outmsg_set(dh, 1, cbmsg1);
        if (cbmsg2)
            device_handle_outmsg_set(dh, 2, cbmsg2);
        if (device_send_lock(h, dh, 1) < 0)
            goto done;
        device_handle_tid_set(dh, ct->ct_id);
        if (device_state_set(dh, CS_PUSH_LOCK) < 0)
            goto done;
    }
    else{
        device_handle_tid_set(dh, 0);
    }
    retval = 1;
 done:
    if (dvec)
        free(dvec);
    if (avec)
        free(avec);
    if (chvec0)
        free(chvec0);
    if (chvec1)
        free(chvec1);
    if (cb)
        cbuf_free(cb);
    if (x0)
        xml_free(x0);
    if (x1t)
        xml_free(x1t);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Incoming rpc handler to sync from one or several devices
 *
 * @param[in]  h       Clixon handle
 * @param[in]  dh      Device handle
 * @param[in]  tid     Transaction id
 * @param[in]  state   0: config, 1: config+state
 * @param[in]  xpath   XPath (experimental, unclear semantics)
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int
pull_device_one(clixon_handle h,
                device_handle dh,
                uint64_t      tid,
                int           state,
                char         *xpath,
                cbuf         *cbret)
{
    int  retval = -1;
    int  s;

    clixon_debug(CLIXON_DBG_CTRL, "");
    s = device_handle_socket_get(dh);
    if (device_send_get(h, dh, s, state, xpath) < 0)
        goto done;
    if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
        goto done;
    device_handle_tid_set(dh, tid);
    retval = 1;
 done:
    return retval;
}

/*! Read the config of one or several remote devices
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_config_pull(clixon_handle h,
                cxobj        *xe,
                cbuf         *cbret,
                void         *arg,
                void         *regarg)
{
    int                     retval = -1;
    client_entry           *ce = (client_entry *)arg;
    char                   *pattern = NULL;
    cxobj                  *xret = NULL;
    cxobj                  *xn;
    cvec                   *nsc = NULL;
    int                     groups = 0;
    cvec                   *devvec = NULL;
    cg_var                 *cv;
    cxobj                 **vec1 = NULL;
    size_t                  vec1len;
    cxobj                 **vec2 = NULL;
    size_t                  vec2len;
    char                   *devname;
    device_handle           dh;
    controller_transaction *ct = NULL;
    char                   *str;
    cbuf                   *cberr = NULL;
    int                     transient = 0;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    /* Initiate new transaction */
    if ((ret = controller_transaction_new(h, ce, clicon_username_get(h), "pull", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);
    if ((str = xml_find_body(xe, "transient")) != NULL)
        transient = strcmp(str, "true") == 0;
    ct->ct_pull_transient = transient;
    if ((str = xml_find_body(xe, "merge")) != NULL)
        ct->ct_pull_merge = strcmp(str, "true") == 0;
    if ((ret = xmldb_get_cache(h, "running", &xret, NULL)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_DB, 0, "Error when reading from running_db, unknown error");
        goto done;
    }
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN) /* maybe this is an error? */
            continue;
        if ((ret = pull_device_one(h, dh, ct->ct_id, 0, NULL, cbret)) < 0)
            goto done;
        if (ret == 0) // XXX: Return value has not been checked before
            goto ok;
    }
    if (xmldb_db_reset(h, "tmpdev") < 0) /* Requires root access */
        goto done;
    if (xmldb_copy(h, "running", "tmpdev") < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
    /* No device started, close transaction */
    if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    return retval;
}

/*! Timeout callback of service actions
 *
 * @param[in] s     Socket
 * @param[in] arg   Transaction handle
 * @retval    0     OK
 * @retval   -1     Error
 */
static int
actions_timeout(int   s,
                void *arg)
{
    int                     retval = -1;
    controller_transaction *ct = (controller_transaction *)arg;
    clixon_handle           h;

    clixon_debug(CLIXON_DBG_CTRL, "");
    h = ct->ct_h;
    if (ct->ct_state == TS_DONE)
        goto ok;
    if (ct->ct_state != TS_RESOLVED){ /* 1.3 The transition is not in an error state */
        if (controller_transaction_failed(h, ct->ct_id, ct, NULL, TR_FAILED_DEV_IGNORE, "Actions", "Timeout waiting for action daemon") < 0)
            goto done;
        if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
        }
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Set timeout of services action
 *
 * @param[in] h   Clixon handle
 * @retval    0   OK
 * @retval   -1   Error
 */
static int
actions_timeout_register(controller_transaction *ct)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    int            d;

    clixon_debug(CLIXON_DBG_CTRL, "");
    gettimeofday(&t, NULL);
    d = clicon_data_int_get(ct->ct_h, "controller-device-timeout");
    if (d != -1)
        t1.tv_sec = d;
    else
        t1.tv_sec = CONTROLLER_DEVICE_TIMEOUT_DEFAULT;
    t1.tv_usec = 0;
    clixon_debug(CLIXON_DBG_CTRL, "timeout:%ld s", t1.tv_sec);
    timeradd(&t, &t1, &t);
    if (clixon_event_reg_timeout(t, actions_timeout, ct, "Controller service actions") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Cancel timeout
 *
 * @param[in] h   Clixon handle
 */
static int
actions_timeout_unregister(controller_transaction *ct)
{
    (void)clixon_event_unreg_timeout(actions_timeout, ct);
    return 0;
}

static cxobj *
getservicekey(cxobj *xn)
{
    yang_stmt *yn;
    cvec      *cvv;
    char      *key;
    cxobj     *xkey = NULL;

    if ((yn = xml_spec(xn)) == NULL){
        clixon_err(OE_YANG, 0, "No yangspec of XML service node %s", xml_name(xn));
        goto done;
    }
    if (yang_keyword_get(yn) != Y_LIST){
        clixon_err(OE_YANG, 0, "Yangspec %s is not LIST", yang_argument_get(yn));
        goto done;
    }
    if ((cvv = yang_cvec_get(yn)) == NULL){
        clixon_err(OE_YANG, 0, "Yangspec %s does not have cvv", yang_argument_get(yn));
        goto done;
    }
    if ((key = cvec_i_str(cvv, 0)) == NULL){
        clixon_err(OE_YANG, 0, "Yangspec %s cvv does not have key", yang_argument_get(yn));
        goto done;
    }
    xkey = xml_find_type(xn, NULL, key, CX_ELMNT);
 done:
     return xkey;
}

/*! Get candidate and running, compute diff and return notification
 *
 * @param[in]  h        Clixon handle
 * @param[in]  ct       Transaction
 * @param[in]  td       Local diff transaction
 * @param[out] services 0:  No service configuration, 1: Service config
 * @param[out] cvv      Vector of changed service instances, on the form name:<service> value:<instance>
 * @retval     0        OK, cb including notify msg (or not)
 * @retval    -1        Error
 * @see devices_diff   where diff is constructed
 */
static int
controller_actions_diff(clixon_handle           h,
                        controller_transaction *ct,
                        transaction_data_t     *td,
                        int                    *services,
                        cvec                   *cvv)
{
    int     retval = -1;
    cvec   *nsc = NULL;
    cxobj  *x0s;
    cxobj  *x1s;
    cxobj  *xn;
    char   *instance;
    cxobj  *xi;
    cbuf   *cb = NULL;

    x0s = xpath_first(td->td_src, nsc, "services");
    x1s = xpath_first(td->td_target, nsc, "services");
    if (x0s == NULL && x1s == NULL){
        *services = 0;
        goto ok;
    }
    *services = 1;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Check deleted */
    if (x0s){
        xn = NULL;
        while ((xn = xml_child_each(x0s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE|XML_FLAG_DEL) == 0)
                continue;
            if ((xi = getservicekey(xn)) == NULL ||
                (instance = xml_body(xi)) == NULL)
                continue;
            /* XXX See also service_action_one where tags are also created */
            cprintf(cb, "%s[%s='%s']", xml_name(xn), xml_name(xi), instance);
            if (cvec_add_string(cvv, cbuf_get(cb), NULL) < 0){
                clixon_err(OE_UNIX, errno, "cvec_add_string");
                goto done;
            }
            cbuf_reset(cb);
        }
    }
    /* Check added */
    if (x1s){
        xn = NULL;
        while ((xn = xml_child_each(x1s, xn,  CX_ELMNT)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE|XML_FLAG_ADD) == 0)
                continue;
            if ((xi = getservicekey(xn)) == NULL ||
                (instance = xml_body(xi)) == NULL)
                continue;
            /* XXX See also service_action_one where tags are also created */
            cprintf(cb, "%s[%s='%s']", xml_name(xn), xml_name(xi), instance);
            if (cvec_find(cvv, cbuf_get(cb)) == NULL &&
                cvec_add_string(cvv, cbuf_get(cb), NULL) < 0){
                clixon_err(OE_UNIX, errno, "cvec_add_string");
                goto done;
            }
            cbuf_reset(cb);
        }
    }
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Callback function type for xml_apply
 *
 * @param[in]  x    XML node
 * @param[in]  arg  General-purpose argument
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
xml_add_op(cxobj *x,
           void  *arg)
{
    enum operation_type op = (intptr_t)arg;

    if (xml_flag(x, XML_FLAG_CACHE_DIRTY) != 0x0){
        xml_flag_reset(x, XML_FLAG_CACHE_DIRTY);
        if (xml_add_attr(x, NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE, "xmlns", NULL) == NULL)
            return -1;
        if (xml_add_attr(x, "operation", xml_operation2str(op), NETCONF_BASE_PREFIX, NULL) == NULL)
            return -1;
        return 2;  /* Locally abort this subtree, continue with others, XXX trunc sub-nodes? */
    }
    return 0;
}

/*! Strip all service data in device config
 *
 * Read a datastore, for each device in the datastore, strip data created by services
 * as defined by the services vector cvv. Write back the changed datastore
 * Algorithm:
 *   1) Mark orig xd with MARK ancestors to CHANGE (also cache-dirty to overcome flag copy reset)
 *   2) Copy marked nodes to xedit tree
 *   3) Add operation="delete" to all marked nodes in xedit tree
 *   4) Unmark orig tree
 *   5) Modify tree with xmldb_put
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database
 * @param[in]  cvv  Vector of services, if empty then all
 * @retval     0    OK
 * @retval    -1    Error
 * @note Differentiate between reading created from running while deleting from action-db
 */
static int
strip_service_data_from_device_config(clixon_handle h,
                                      char         *db,
                                      cvec         *cvv)
{
    int     retval = -1;
    cxobj  *xt0 = NULL;
    cxobj  *xt1 = NULL;
    cxobj  *xc0;
    cxobj  *xc1;
    cxobj  *xp;
    cxobj  *xd;
    cbuf   *cbret = NULL;
    int     i;
    cxobj **vec = NULL;
    size_t  veclen;
    cg_var *cv;
    char   *xpath;
    int     touch = 0;
    cxobj  *xedit = NULL;
    int     ret;

    /* Get services/created read-only from running_db for reading */
    if (xmldb_get_cache(h, "running", &xt0, NULL) < 0)
        goto done;
    /* Get services/created and devices from action_db for deleting. */
    if ((xedit = xml_new("config", NULL, CX_ELMNT)) == NULL)
        goto done;
    if (xmldb_get_cache(h, db, &xt1, NULL) < 0)
        goto done;
    /* Go through /services/././created that match cvv service name (NULL means all)
     * then for each xpath find object and purge
     * Also remove created/name itself
     */
    if (cvec_len(cvv) != 0){ /* specific services */
        cv = NULL;
        while ((cv = cvec_each(cvv, cv)) != NULL){
            if ((xc0 = xpath_first(xt0, NULL, "services/%s/created", cv_name_get(cv))) == NULL)
                continue;
            /* Read created from read-only running */
            xp = NULL;
            while ((xp = xml_child_each(xc0, xp, CX_ELMNT)) != NULL) {
                if (strcmp(xml_name(xp), "path") != 0)
                    continue;
                if ((xpath = xml_body(xp)) == NULL)
                    continue;
                if ((xd = xpath_first(xt1, NULL, "%s", xpath)) == NULL)
                    continue;
                /* cache-dirty just to ensure copied to new tree */
                xml_flag_set(xd, XML_FLAG_MARK|XML_FLAG_CACHE_DIRTY);
                xml_apply_ancestor(xd, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
            }
            xc1 = xpath_first(xt1, NULL, "services/%s/created", cv_name_get(cv));
            if (xc1){
                /* cache-dirty just to ensure copied to new tree */
                xml_flag_set(xc1, XML_FLAG_MARK|XML_FLAG_CACHE_DIRTY);
                xml_apply_ancestor(xc1, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
            }
            if (xml_copy_marked(xt1, xedit) < 0)
                goto done;
            if (xml_apply(xt1, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE|XML_FLAG_CACHE_DIRTY)) < 0)
                goto done;
            if (xml_apply(xedit, CX_ELMNT, xml_add_op, (void*)OP_DELETE) < 0)
                goto done;
            touch++;
        }
    }
    else{ /* All services */
        if (xpath_vec(xt0, NULL, "services//created", &vec, &veclen) < 0)
            goto done;
        for (i=0; i<veclen; i++){
            xc0 = vec[i];
            xp = NULL;
            while ((xp = xml_child_each(xc0, xp, CX_ELMNT)) != NULL) {
                if (strcmp(xml_name(xp), "path") != 0)
                    continue;
                if ((xpath = xml_body(xp)) == NULL)
                    continue;
                if ((xd = xpath_first(xt1, NULL, "%s", xpath)) == NULL)
                    continue;
                /* cache-dirty just to ensure copied to new tree */
                xml_flag_set(xd, XML_FLAG_MARK|XML_FLAG_CACHE_DIRTY);
                xml_apply_ancestor(xd, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
                touch++;
            }
        }
        if (vec)
            free(vec);
        if (xpath_vec(xt1, NULL, "services//created", &vec, &veclen) < 0)
            goto done;
        for (i=0; i<veclen; i++){
            xc1 = vec[i];
            if (xc1 && xml_purge(xc1) < 0)
                goto done;
            touch++;
        }
    }
    if (touch){
        if ((cbret = cbuf_new()) == NULL){ // dummy
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if ((ret = xmldb_put(h, db, OP_NONE, xedit, NULL, cbret)) < 0)
            goto done;
        if (ret == 0){
            clixon_err(OE_XML, 0, "xmldb_put failed");
            goto done;
        }
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (cbret)
        cbuf_free(cbret);
    if (xedit)
        xml_free(xedit);
    return retval;
}

/*! Compute diff of candidate + commit and trigger service-commit notify
 *
 * @param[in]  h       Clixon handle
 * @param[in]  ct      Transaction
 * @param[in]  db      From where to compute diffs and push
 * @param[out] cberr   Error message (if retval = 0)
 * @retval     1       OK
 * @retval     0       Failed
 * @retval    -1       Error
 */
static int
controller_commit_push(clixon_handle           h,
                       controller_transaction *ct,
                       char                   *db,
                       cbuf                  **cberr)
{
    int           retval = -1;
    device_handle dh = NULL;
    int           ret;

    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) != ct->ct_id)
            continue;
        if ((ret = push_device_one(h, dh, ct, db, cberr)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto failed;
    }
    retval = 1;
 done:
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Push commit after actions completed, potentially start device push process
 *
 * Devices are removed of no device diff
 * @param[in]  h    Clixon handle
 * @param[in]  ct   Transaction
 * @param[in]  candidate Name of candidate-db
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
commit_push_after_actions(clixon_handle           h,
                          controller_transaction *ct,
                          const char             *candidate)
{
    int           retval = -1;
    cbuf         *cberr = NULL;
    int           ret;

    /* Dump volatile actions db to disk */
    if (ct->ct_actions_type != AT_NONE && strcmp(ct->ct_sourcedb, "actions") == 0) {
        if (xmldb_populate(h, "actions") < 0)
            goto done;
        if (xmldb_write_cache2file(h, "actions") < 0)
            goto done;
        // XXX validate actions?
    }
    if (ct->ct_push_type == PT_NONE){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
    }
    else{
        /* Compute diff of candidate + commit and trigger service
         * If some device diff is zero, then remove device from transaction
         */
        if ((ret = controller_commit_push(h, ct, "actions", &cberr)) < 0)
            goto done;
        if (ret == 0){
            if ((ct->ct_origin = strdup("controller")) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if ((ct->ct_reason = strdup(cbuf_get(cberr))) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
        }
        /* No device started, close transaction */
        else if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
            if (ct->ct_actions_type != AT_NONE && strcmp(ct->ct_sourcedb, "candidate")==0){
                if ((cberr = cbuf_new()) == NULL){
                    clixon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                /* What to copy to candidate and commit to running? */
                if (xmldb_copy(h, "actions", candidate) < 0)
                    goto done;
                /* XXX: recursive creates transaction */
                if ((ret = candidate_commit(h, NULL, candidate, 0, 0, cberr)) < 0){
                    /* Handle that candidate_commit can return < 0 if transaction ongoing */
                    cprintf(cberr, "%s", clixon_err_reason()); // XXX encode
                    ret = 0;
                }
                if (clicon_option_bool(h, "CLICON_AUTOLOCK"))
                    xmldb_unlock(h, candidate);
                if (ret == 0){ // XXX awkward, cb ->xml->cb
                    cxobj *xerr = NULL;
                    cbuf *cberr2 = NULL;
                    if ((cberr2 = cbuf_new()) == NULL){
                        clixon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    if (clixon_xml_parse_string(cbuf_get(cberr), YB_NONE, NULL, &xerr, NULL) < 0)
                        goto done;
                    if (netconf_err2cb(h, xerr, cberr2) < 0)
                        goto done;
                    if (controller_transaction_failed(h, ct->ct_id, ct, NULL, TR_FAILED_DEV_LEAVE,
                                                      NULL,
                                                      cbuf_get(cberr2)) < 0)
                        goto done;
                    if (xerr)
                        xml_free(xerr);
                    if (cberr2)
                        cbuf_free(cberr2);
                    goto ok;
                }
            }
            if ((ct->ct_reason = strdup("No device  configuration changed, no push necessary")) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
        }
        else{
            /* Some or all started */
        }
    }
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    return retval;
}

/*! Send NETCONF service-commit notification
 *
 * @param[in]  h     Clixon handle
 * @param[in]  ct    Transaction
 * @param[in]  cvv   Vector of services with changed configuration
 * @param[in]  diff  Diff of the services configuration
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
services_commit_notify(clixon_handle           h,
                       controller_transaction *ct,
                       cvec                   *cvv,
                       int                     diff)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cg_var *cv;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<services-commit xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%" PRIu64"</tid>", ct->ct_id);
    cprintf(cb, "<source>actions</source>");
    cprintf(cb, "<target>actions</target>");
    if (diff)
        cprintf(cb, "<diff>true</diff>");
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL){
        cprintf(cb, "<service>");
        xml_chardata_cbuf_append(cb, 0, cv_name_get(cv));
        cprintf(cb, "</service>");
    }
    cprintf(cb, "</services-commit>");
    if (stream_notify(h, "services-commit", "%s", cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compute diff of candidate, copy to actions-db and trigger service-commit notify
 *
 * Compute diff of candidate + commit, copy candidate to actions-db and
 * trigger service-commit notify
 * @param[in]  h         Clixon handle
 * @param[in]  ct        Transaction
 * @param[in]  actions   How to trigger service-commit notifications
 * @param[in]  td        Transaction data
 * @param[in]  service_instance Optional service instance if actions=FORCE
 * @param[in]  diff      Diff of the services configuration
 * @param[in]  candidate Name of candidate-db
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
controller_commit_actions(clixon_handle           h,
                          controller_transaction *ct,
                          actions_type            actions,
                          transaction_data_t     *td,
                          char                   *service_instance,
			  int                     diff,
                          const char             *candidate
                          )
{
    int       retval = -1;
    cvec     *cvv = NULL;       /* Format: <service> <instance> */
    int       services = 0;
    db_elmnt *de;

    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    /* Get candidate and running, compute diff and get notification msg in return,
     * and check if there are any services at all
     * XXX may trigger if created paths are manually edited
     */
    if (controller_actions_diff(h, ct, td, &services, cvv) < 0)
        goto done;
    if (actions == AT_FORCE || actions == AT_DELETE){
        cvec_reset(cvv);
        if (service_instance)
            cvec_add_string(cvv, service_instance, NULL);
    }
    /* 1) copy candidate to actions and remove all device config tagged with services */
    if ((de = xmldb_find(h, "actions")) == NULL)
        if ((de = xmldb_new(h, "actions")) == NULL)
            goto done;
#ifdef XMLDB_ACTION_INMEM
    xmldb_clear(h, "actions");
    xmldb_volatile_set(de, 1);
#endif
    if (xmldb_copy(h, candidate, "actions") < 0)
        goto done;
    if (services && actions == AT_DELETE){
        /* Delete service, do not activate/notify actions, just push deletes to devices
           Strip service data in device config */
        if (strip_service_data_from_device_config(h, "actions", cvv) < 0)
            goto done;
        if (commit_push_after_actions(h, ct, candidate) < 0)
            goto done;
    }
    else if (services && (actions == AT_FORCE || cvec_len(cvv) > 0)){
        /* IF Services exist AND
         * either service changes or forced,
         * THEN notify services
         */
        if (services_commit_notify(h, ct, cvv, diff) < 0)
            goto done;
        /* Strip service data in device config for services that changed */
        if (strip_service_data_from_device_config(h, "actions", cvv) < 0)
            goto done;
        controller_transaction_state_set(ct, TS_ACTIONS, -1);
        if (actions_timeout_register(ct) < 0)
            goto done;
    }
    else{ /* No services, proceed to next step */
        if (commit_push_after_actions(h, ct, candidate) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (cvv)
        cvec_free(cvv);
    return retval;
}

/*! Check if any local/meta device fields have changed in the selected device set
 *
 * These fields are ones that effect the connection to a device from clixon-controller.yang:
 * For simplicitly, these are any config leaf under the device container, EXCEPT config
 * In particular: enabled, conn-type, user, addr
 * If local diffs are made, a device push should probably not be done since a connect may be
 * necessary before the push to open/close/change device connections
 * @param[in]  h       Clixon handle
 * @param[in]  td      Transaction diff
 * @param[out] changed Device handle of changed device, if any
 * @retval     0       OK
 * @retval    -1       Error
 * @see devices_diff   where diff is constructed
 */
static int
devices_local_change(clixon_handle       h,
                     transaction_data_t *td,
                     device_handle      *changed)
{
    int    retval = -1;
    cxobj *x0d;
    cxobj *x1d;
    cxobj *xd = NULL;
    cxobj *xi;
    cvec  *nsc = NULL;
    char  *name;

    x0d = xpath_first(td->td_src, nsc, "devices");
    x1d = xpath_first(td->td_target, nsc, "devices");
    if (x0d && td->td_dlen){     /* Check deleted */
        xd = NULL;
        while ((xd = xml_child_each(x0d, xd,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(xd), "device") != 0)
                continue;
            xi = NULL;
            while ((xi = xml_child_each(xd, xi,  CX_ELMNT)) != NULL){
                if (strcmp(xml_name(xi), "config") != 0 &&
                    xml_flag(xi, XML_FLAG_DEL) != 0){
                    break;
                }
            }
            if (xi != NULL)
                break;
        }
    }
    if (xd==NULL && x1d && (td->td_alen || td->td_clen)){ /* Check added or changed */
        xd = NULL;
        while ((xd = xml_child_each(x1d, xd,  CX_ELMNT)) != NULL){
            if (strcmp(xml_name(xd), "device") != 0)
                continue;
            xi = NULL;
            while ((xi = xml_child_each(xd, xi,  CX_ELMNT)) != NULL){
                if (strcmp(xml_name(xi), "config") != 0 &&
                    xml_flag(xi, XML_FLAG_CHANGE|XML_FLAG_ADD) != 0){
                    break;
                }
            }
            if (xi != NULL)
                break;
        }
    }
    if (xd){
        name = xml_find_body(xd, "name");
        if ((*changed = device_handle_find(h, name)) == NULL){
            clixon_err(OE_XML, 0, "device %s not found in transaction", name);
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Diff candidate/running and fill in a diff transaction structure for devices in transaction
 *
 * and check if any changed device is closed
 * @param[in]  h         Clixon handle
 * @param[in]  ct        Controller transaction
 * @param[in]  candidate Name of candidate-db
 * @param[out] td        Diff structure
 * @param[out] closed    (First) Changed device handle which is also closed
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
devices_diff(clixon_handle           h,
             controller_transaction *ct,
             const char             *candidate,
             transaction_data_t     *td,
             device_handle          *closed)
{
    int           retval = -1;
    cvec         *nsc = NULL;
    cxobj        *xn;
    device_handle dh;
    char         *name;
    int           i;

    if (candidate == NULL){
        clixon_err(OE_DB, EINVAL, "candidate is NULL");
        goto done;
    }
    if (xmldb_get_cache(h, candidate, &td->td_target, NULL) < 0)
        goto done;
    if (xmldb_get_cache(h, "running", &td->td_src, NULL) < 0)
        goto done;
    /* Remove devices not in transaction */
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) == ct->ct_id)
            continue;
        name = device_handle_name_get(dh);
        if ((xn = xpath_first(td->td_src, nsc, "devices/device[name='%s']", name)) != NULL)
            xml_flag_set(xn, XML_FLAG_SKIP);
        if ((xn = xpath_first(td->td_target, nsc, "devices/device[name='%s']", name)) != NULL)
            xml_flag_set(xn, XML_FLAG_SKIP);
    }
    if (xml_diff(td->td_src,
                 td->td_target,
                &td->td_dvec,      /* removed: only in running */
                &td->td_dlen,
                &td->td_avec,      /* added: only in candidate */
                &td->td_alen,
                &td->td_scvec,     /* changed: original values */
                &td->td_tcvec,     /* changed: wanted values */
                &td->td_clen) < 0)
        goto done;
    /* Mark flags, see also validate_common */
    for (i=0; i<td->td_dlen; i++){ /* Also down */
        xn = td->td_dvec[i];
        xml_flag_set(xn, XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_alen; i++){ /* Also down */
        xn = td->td_avec[i];
        xml_flag_set(xn, XML_FLAG_ADD|XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_clen; i++){ /* Also up */
        xn = td->td_scvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        xn = td->td_tcvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* Check if any device with changes is closed */
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        int touch = 0;
        if (device_handle_tid_get(dh) != ct->ct_id)
            continue;
        name = device_handle_name_get(dh);
        if ((xn = xpath_first(td->td_src, nsc, "devices/device[name='%s']", name)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE) != 0)
                touch++;
        }
        if ((xn = xpath_first(td->td_target, nsc, "devices/device[name='%s']", name)) != NULL){
            if (xml_flag(xn, XML_FLAG_CHANGE) != 0)
                touch++;
        }
        if (touch){
            if (device_handle_conn_state_get(dh) != CS_OPEN){
                *closed = dh;
                break;
            }
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Helpful error message if a device is closed or changed
 *
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Controller transaction
 * @param[in]  dh     Device handle (reason=0,1)
 * @param[in]  reason 0: closed, 1: changed, 2: no devices, 3: no changes
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @retval     0      OK
 * @retval    -1      Error
 */
static int
device_error(clixon_handle           h,
             controller_transaction *ct,
             device_handle           dh,
             int                     reason,
             cbuf                   *cbret)
{
    int   retval = -1;
    cbuf *cb = NULL;
    char *name = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (dh)
        name = device_handle_name_get(dh);
    switch (reason){
    case 0: /* closed */
        cprintf(cb, "Device is closed: '%s' (try 'connection open' or edit, local commit, and connect)", name);
        break;
    case 1: /* changed */
        cprintf(cb, "Device '%s': local fields are changed (try 'commit local' instead)", name);
        break;
    case 2: /* empty */
        cprintf(cb, "No devices are selected (or no devices exist) and you have requested commit PUSH");
        break;
    case 3: /* unchanged */
        cprintf(cb, "No change to devices");
        break;
    }
    if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cb))< 0)
        goto done;
    if (controller_transaction_done(h, ct, TR_FAILED) < 0)
        goto done;
    if (name && (ct->ct_origin = strdup(name)) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    if ((ct->ct_reason = strdup(cbuf_get(cb))) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Extended commit: trigger actions and device push
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * TODO: device-groups
 */
static int
rpc_controller_commit(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,
                      void         *regarg)
{
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    controller_transaction *ct = NULL;
    char                   *str;
    char                   *pattern = NULL;
    int                     groups = 0;
    cvec                   *devvec = NULL;
    cg_var                 *cv;
    cxobj                  *xret = NULL;
    cxobj                  *xn = NULL;
    cvec                   *nsc = NULL;
    cxobj                 **vec1 = NULL;
    size_t                  vec1len;
    cxobj                 **vec2 = NULL;
    size_t                  vec2len;
    char                   *devname;
    char                   *sourcedb = NULL;
    actions_type            actions = AT_NONE;
    push_type               pusht = PT_NONE;
    cbuf                   *cbtr = NULL;
    cbuf                   *cberr = NULL;
    device_handle           closed = NULL;
    device_handle           changed = NULL;
    transaction_data_t     *td = NULL;
    char                   *service_instance = NULL;
    int                     diff = 0;
    char                   *candidate = NULL;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    if (xn)
        pattern = xml_body(xn);
    else
        pattern = "*";
    if ((str = xml_find_body(xe, "source")) == NULL){ /* on the form ds:running */
        if (netconf_operation_failed(cbret, "application", "sourcedb not supported")< 0)
            goto done;
        goto ok;
    }
    /* strip prefix, eg ds: */
    if (nodeid_split(str, NULL, &sourcedb) < 0)
        goto done;
    if (sourcedb == NULL ||
        (strcmp(sourcedb, "candidate") != 0 && strcmp(sourcedb, "running") != 0)){
        if (netconf_operation_failed(cbret, "application", "sourcedb not supported")< 0)
            goto done;
        goto ok;
    }
    if (xmldb_find_create(h, "candidate", ce->ce_id, NULL, &candidate) < 0)
        goto done;
    if (candidate == NULL){
        clixon_err(OE_DB, 0, "No candidate struct");
        goto done;
    }
    /* Local validate if candidate */
    if (strcmp(sourcedb, "candidate") == 0){
        if ((ret = candidate_validate(h, candidate, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto ok;
    }
    if ((cbtr = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cbtr, "Controller commit");
    if ((str = xml_find_body(xe, "actions")) != NULL){
        actions = actions_type_str2int(str);
        cprintf(cbtr, " actions:%s", str);
    }
    if ((str = xml_find_body(xe, "push")) != NULL){
        pusht = push_type_str2int(str);
        cprintf(cbtr, " push:%s", str);
    }
    service_instance = xml_find_body(xe, "service-instance");

    /* Initiate new transaction.
     * NB: this locks candidate, which always needs to be unlocked, eg by controller_transaction_done
     */
    if ((ret = controller_transaction_new(h, ce, clicon_username_get(h), cbuf_get(cbtr), &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    ct->ct_push_type = pusht;
    ct->ct_actions_type = actions;
    ct->ct_sourcedb = sourcedb;
    sourcedb = NULL;
    /* Mark devices with transaction-id if name matches device pattern */
    if ((ret = xmldb_get_cache(h, "running", &xret, NULL)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_DB, 0, "Error when reading from running_db, unknown error");
        goto done;
    }
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        char *body;
        device_handle dh;

        xn = cv_void_get(cv);
        /* Name of device or device-group */
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        /* Filter if not enabled */
        if ((body = xml_find_body(xn, "enabled")) == NULL)
            continue;
        if (strcmp(body, "true") != 0)
            continue;
        /* Include device in transaction */
        device_handle_tid_set(dh, ct->ct_id);
    }
    /* If there are no devices selected and push != NONE */
    if (controller_transaction_nr_devices(h, ct->ct_id) == 0 && pusht != PT_NONE){
        if (device_error(h, ct, NULL, 2, cbret) < 0)
            goto done;
        goto ok;
    }
    /* Start local commit/diff transaction */
    if ((td = transaction_new()) == NULL)
        goto done;
    /* Diff candidate/running and fill in a diff transaction structure td for future use
     */
    if (devices_diff(h, ct, candidate, td, &closed) < 0)
        goto done;
    /* If device is closed and push != NONE, then error */
    if (closed != NULL && pusht != PT_NONE){
        if (device_error(h, ct, closed, 0, cbret) < 0)
            goto done;
        goto ok;
    }
    /* Check if any local/meta device fields have changed of selected devices */
    if (devices_local_change(h, td, &changed) < 0)
        goto done;
    if (changed != NULL){
        if (device_error(h, ct, changed, 1, cbret) < 0)
            goto done;
        goto ok;
    }
    switch (actions){
    case AT_NONE: /* Bypass actions, directly to push */
        if ((ret = controller_commit_push(h, ct, "running", &cberr)) < 0)
            goto done;
        if (ret == 0){
            if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;
        }
        if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
            /* No device started, close transaction */
            if (netconf_operation_failed(cbret, "application", "No changes to push")< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;
        }
        break;
    case AT_CHANGE:
    case AT_FORCE:
    case AT_DELETE:
        if (strcmp(ct->ct_sourcedb, "candidate") != 0){ // XXX
            if (netconf_operation_failed(cbret, "application", "Only candidates db supported if actions")< 0)
                goto done;
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            goto ok;
        }
	if (pusht == PT_NONE)
	    diff = 1;
        /* Compute diff of candidate, copy to actions, trigger notify */
        if (controller_commit_actions(h, ct, actions, td, service_instance, diff, candidate) < 0)
            goto done;
        break;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (td)
        transaction_free1(td, 0);
    if (sourcedb)
        free(sourcedb);
    if (cbtr)
        cbuf_free(cbtr);
    if (cberr)
        cbuf_free(cberr);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    return retval;
}

/*! Get configuration db of a single device of name 'device-<devname>-<postfix>.xml'
 *
 * Typically this db is retrieved by the pull rpc
 * Should probably be replaced by a more generic function.
 * Possibly just extend get-config with device dbs?";
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_get_device_config(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,
                      void         *regarg)
{
    int                retval = -1;
    client_entry      *ce = (client_entry *)arg;
    char              *pattern;
    int                groups = 0;
    cvec              *devvec = NULL;
    cg_var            *cv;
    cxobj            **vec1 = NULL;
    size_t             vec1len;
    cxobj            **vec2 = NULL;
    size_t             vec2len;
    char              *devname;
    cxobj             *xroot = NULL;
    cxobj             *xroot1; /* dont free */
    char              *config_type;
    cvec              *nsc = NULL;
    cxobj             *xret = NULL;
    cxobj             *xn;
    cbuf              *cb = NULL;
    cbuf              *cberr = NULL;
    device_config_type dt;
    char              *candidate = NULL;
    int                ret;

    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);
    config_type = xml_find_body(xe, "config-type");
    dt = device_config_type_str2int(config_type);
    if (dt == DT_CANDIDATE){
        if (xmldb_find_create(h, "candidate", ce->ce_id, NULL, &candidate) < 0)
            goto done;
        if ((ret = xmldb_get_cache(h, candidate, &xret, NULL)) < 0)
            goto done;
    }
    else{
        if ((ret = xmldb_get_cache(h, "running", &xret, NULL)) < 0)
            goto done;
    }
    if (ret == 0){
        clixon_err(OE_DB, 0, "Error when reading from running_db, unknown error");
        goto done;
    }
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cb, "<config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        switch (dt){
        case DT_RUNNING:
        case DT_CANDIDATE:
        case DT_ACTIONS:
            xroot1 = xpath_first(xn, nsc, "config");
            if (clixon_xml2cbuf1(cb, xroot1, 0, 0, NULL, -1, 0, 0) < 0)
                goto done;
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            if ((ret = device_config_read_cache(h, devname, config_type, &xroot, &cberr)) < 0)
                goto done;
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            if (clixon_xml2cbuf1(cb, xroot, 0, 0, NULL, -1, 0, WITHDEFAULTS_EXPLICIT) < 0)
                goto done;
            if (xroot)
                xroot = NULL;
            break;
        }
    }
    cprintf(cb, "</config>");
    cprintf(cb, "</rpc-reply>");
    cprintf(cbret, "%s", cbuf_get(cb));
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (cberr)
        cbuf_free(cberr);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    if (xroot)
        xml_free(xroot);
    return retval;
}

/*! Change connection of single device
 *
 * @param[in]  h         Clixon handle
 * @param[in]  xn        RPC XML
 * @param[in]  ct        Controller transaction
 * @param[in]  operation CLOSE/OPEN/RECONNECT
 * @param[in]  dh        Device handle
 * @param[in]  enabled   Device is enabled
 * @param[out] tmpdev    Set if tmpdev datastore for device commits
 * @param[out] cbret     Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @retval     1         OK, continue
 * @retval     0         OK, skip
 * @retval    -1         Error
 */
static int
connection_change_one(clixon_handle           h,
                      cxobj                  *xn,
                      controller_transaction *ct,
                      char                   *operation,
                      int                    *tmpdev,
                      cbuf                   *cbret)
{
    int           retval = -1;
    char         *devname;
    device_handle dh;
    char         *body;
    int           enabled;
    char         *reason = NULL;
    int           ret;

    if ((devname = xml_find_body(xn, "name")) == NULL){
        clixon_err(OE_NETCONF, 0, "name not found");
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "%s", devname);
    if ((body = xml_find_body(xn, "enabled")) == NULL){
        clixon_err(OE_NETCONF, 0, "enabled not found");
        goto done;
    }
    enabled = strcmp(body, "true")==0;
    dh = device_handle_find(h, devname);
    /* @see clixon-controller.yang connection-operation */
    if (strcmp(operation, "CLOSE") == 0){
        /* Close if there is a handle and it is OPEN */
        if (dh != NULL && device_handle_conn_state_get(dh) == CS_OPEN){
            if (device_close_connection(dh, "User request") < 0)
                goto done;
        }
    }
    else if (strcmp(operation, "OPEN") == 0){
        /* Open if enabled and handle does not exist or it exists and is closed  */
        if (enabled &&
            (dh == NULL || device_handle_conn_state_get(dh) == CS_CLOSED)){
            if ((ret = controller_connect(h, xn, ct, &reason)) < 0)
                goto done;
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", "%s", reason)< 0)
                    goto done;
                goto ok;
            }
            (*tmpdev)++;
        }
    }
    else if (strcmp(operation, "RECONNECT") == 0){
        /* First close it if there is a handle and it is OPEN */
        if (dh != NULL && device_handle_conn_state_get(dh) == CS_OPEN){
            if (device_close_connection(dh, "User request") < 0)
                goto done;
        }
        /* Then open if enabled */
        if (enabled){
            if ((ret = controller_connect(h, xn, ct, &reason)) < 0)
                goto done;
            if (ret == 0){
                if (netconf_operation_failed(cbret, "application", "%s", reason)< 0)
                    goto done;
                goto ok;
            }
            (*tmpdev)++;
        }
    }
    else {
        clixon_err(OE_NETCONF, 0, "%s is not a connection-operation", operation);
        goto done;
    }
    retval = 1;
 done:
    if (reason)
        free(reason);
    return retval;
 ok:
    retval = 0;
    goto done;
}

/*! Change connection of devices
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_connection_change(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,
                      void         *regarg)
{
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    char                   *pattern = NULL;
    int                     groups = 0;
    cxobj                  *xret = NULL;
    cxobj                  *xn;
    cvec                   *nsc = NULL;
    cvec                   *devvec = NULL;
    cg_var                 *cv;
    cxobj                 **vec1 = NULL;
    size_t                  vec1len;
    cxobj                 **vec2 = NULL;
    size_t                  vec2len;
    controller_transaction *ct = NULL;
    char                   *operation;
    cbuf                   *cberr = NULL;
    cbuf                   *cbtr = NULL;
    int                     tmpdev = 0;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((cbtr = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cbtr, "Controller connect");
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);
    operation = xml_find_body(xe, "operation");
    cprintf(cbtr, " %s", operation);
    if ((ret = controller_transaction_new(h, ce, clicon_username_get(h), cbuf_get(cbtr), &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    if (xmldb_get_cache(h, "running", &xret, NULL) < 0)
        goto done;
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((ret = connection_change_one(h, xn, ct, operation, &tmpdev, cbret)) < 0)
            goto done;
        if (ret == 0) // XXX: Return value has not been checked before
            goto ok;
    }
    /* Initiate tmpdev datastore for device commits */
    if (tmpdev) {
        /* Possibly only copy files / dir */
        if (xmldb_db_reset(h, "tmpdev") < 0) /* Requires root access */
            goto done;
        if (xmldb_copy(h, "running", "tmpdev") < 0)
            goto done;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");
    /* No device started, close transaction */
    if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
        if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (cbtr)
        cbuf_free(cbtr);
    if (cberr)
        cbuf_free(cberr);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    return retval;
}

/*! Terminate an ongoing transaction with an error condition
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_transaction_error(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,
                      void         *regarg)
{
    int                     retval = -1;
    char                   *tidstr;
    char                   *origin;
    char                   *reason;
    uint64_t                tid;
    controller_transaction *ct;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((tidstr = xml_find_body(xe, "tid")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No tid")< 0)
            goto done;
        goto ok;
    }
    if ((ret = parse_uint64(tidstr, &tid, NULL)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "Invalid tid")< 0)
            goto done;
        goto ok;
    }
    if ((ct = controller_transaction_find(h, tid)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No such transaction")< 0)
            goto done;
        goto ok;
    }
    switch (ct->ct_state){
    case TS_RESOLVED:
    case TS_INIT:
    case TS_ACTIONS:
        break;
    case TS_DONE:
        if (netconf_operation_failed(cbret, "application", "Transaction already completed") < 0)
            goto done;
        goto ok;
        break;
    }
    origin = xml_find_body(xe, "origin");
    reason = xml_find_body(xe, "reason");
    if (controller_transaction_failed(h, tid, ct, NULL, TR_FAILED_DEV_IGNORE, origin, reason) < 0)
        goto done;
    if (controller_transaction_done(h, ct, TR_FAILED) < 0)
        goto done;
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Action scripts signal to backend that all actions are completed
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_transactions_actions_done(clixon_handle h,
                              cxobj        *xe,
                              cbuf         *cbret,
                              void         *arg,
                              void         *regarg)
{
    int                     retval = -1;
    char                   *tidstr;
    uint64_t                tid;
    controller_transaction *ct;
    cbuf                   *cberr = NULL;
    cbuf                   *cberr2 = NULL;
    char                   *candidate = NULL;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((tidstr = xml_find_body(xe, "tid")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No tid")< 0)
            goto done;
        goto ok;
    }
    if ((ret = parse_uint64(tidstr, &tid, NULL)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "Invalid tid")< 0)
            goto done;
        goto ok;
    }
    if ((ct = controller_transaction_find(h, tid)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No such transaction")< 0)
            goto done;
        goto ok;
    }
    if (xmldb_find_create(h, "candidate", ct->ct_client_id, NULL, &candidate) < 0)
        goto done;
    switch (ct->ct_state){
    case TS_RESOLVED:
    case TS_INIT:
        if (netconf_operation_failed(cbret, "application", "Transaction in unexpected state") < 0)
            goto done;
        break;
    case TS_ACTIONS:
        actions_timeout_unregister(ct);
        cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
        cprintf(cbret, "<ok/>");
        cprintf(cbret, "</rpc-reply>");
        /* Validate db, second time, after services modification.
         * First is made in rpc_controller_commit.
         * Third is made in CS_PUSH_VALIDATE/WAIT state in device_state_handler
         */
        if ((cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if ((ret = candidate_validate(h, "actions", cberr)) < 0)
            goto done;
        if (ret == 0){
            if ((cberr2 = cbuf_new()) == NULL){
                clixon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            if ((ct->ct_origin = strdup("controller")) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (netconf_cbuf_err2cb(h, cberr, cberr2) < 0)
                goto done;
            if ((ct->ct_reason = strdup(cbuf_get(cberr2))) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            break;
        }
        controller_transaction_state_set(ct, TS_INIT, -1); /* Multiple actions */
        /* Start device push process: compute diff send edit-configs */
        if (commit_push_after_actions(h, ct, candidate) < 0)
            goto done;
        break;
    case TS_DONE:
        if (netconf_operation_failed(cbret, "application", "Transaction already completed(timeout?)") < 0)
            goto done;
        break;
    }
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (cberr2)
        cbuf_free(cberr2);
    return retval;
}

/*! Do NACM read data check, remove violating nodes
 *
 * @param[in]  h     Clixon handle
 * @param[in]  xt    XML top
 * @param[in]  xpath XPath
 * @retval      0    OK
 * @retval     -1    Error
 * see code in backend_get.c
 * get_common(), get_nacm_and_reply()
  *
 */
static int
datastore_diff_nacm_read(clixon_handle h,
                         cxobj        *xt,
                         char         *xpath)
{
    int     retval = -1;
    cxobj  *xnacm;
    char   *username;

    xnacm = clicon_nacm_cache(h);
    username = clicon_username_get(h);
    if (xnacm != NULL){ /* Do NACM validation */
        /* NACM datanode/module purge read access violation */
        if (nacm_datanode_read1(h, xt, username, xnacm) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Given two datastores and xpath, return diff in textual form
 *
 * @param[in]   h      Clixon handle
 * @param[in]   xpath  XPath note
 * @param[in]   db1    First datastore
 * @param[in]   db2    Second datastore
 * @param[in]   format Format of diff
 * @param[out]  cbret  CLIgen buff with NETCONF reply
 * @retval      0      OK
 * @retval     -1      Error
 * @see datastore_diff_device   for intra-device diff
 */
static int
datastore_diff_dsref(clixon_handle    h,
                     char            *xpath,
                     char            *db1,
                     char            *db2,
                     enum format_enum format,
                     cbuf            *cbret)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xt1 = NULL;
    cxobj  *xt2 = NULL;
    cxobj  *x1;
    cxobj  *x2;

    if (xmldb_get_cache(h, db1, &xt1, NULL) < 0)
        goto done;
    if (datastore_diff_nacm_read(h, xt1, xpath) < 0)
        goto done;
    if (xpath)
        x1 = xpath_first(xt1, NULL, "%s", xpath);
    else
        x1 = xt1;
    if (xmldb_get_cache(h, db2, &xt2, NULL) < 0)
        goto done;
    if (datastore_diff_nacm_read(h, xt2, xpath) < 0)
        goto done;
    if (xpath)
        x2 = xpath_first(xt2, NULL, "%s", xpath);
    else
        x2 = xt2;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    switch (format){
    case FORMAT_XML:
        if (clixon_xml_diff2cbuf(cb, x1, x2) < 0)
            goto done;
        break;
    case FORMAT_TEXT:
        if (clixon_text_diff2cbuf(cb, x1, x2) < 0)
            goto done;
        break;
    case FORMAT_JSON:
    case FORMAT_CLI:
    default:
        break;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    xml_chardata_cbuf_append(cbret, 0, cbuf_get(cb));
    cprintf(cbret, "</diff>");
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Given a device pattern, return diff in textual form between different device configs
 *
 * That is diff of configs for same device, only different variants, eg synced, transient, running, etc
 * @param[in]   h       Clixon handle
 * @param[in]   groups  0: device pattern, 1: device-group pattern
 * @param[in]   pattern Glob pattern for selecting devices
 * @param[in]   dt1     Type of device config 1
 * @param[in]   dt2     Type of device config 2
 * @param[in]   format  Format of diff
 * @param[in]   ceid    Client/session id
 * @param[out]  cbret   CLIgen buff with NETCONF reply
 * @retval      0       OK
 * @retval     -1       Error
 * @see datastore_diff_dsref   For inter-datastore diff
 */
static int
datastore_diff_device(clixon_handle      h,
                      int                groups,
                      char              *pattern,
                      device_config_type dt1,
                      device_config_type dt2,
                      enum format_enum   format,
                      uint32_t           ceid,
                      cbuf              *cbret)
{
    int           retval = -1;
    cbuf         *cbxpath = NULL;
    cbuf         *cberr = NULL;
    cbuf         *cb = NULL;
    cxobj        *x1;
    cxobj        *x2;
    cxobj        *x1m = NULL; /* malloced */
    cxobj        *x2m = NULL;
    cvec         *nsc = NULL;
    cxobj        *xret = NULL;
    cxobj        *x1ret = NULL;
    cxobj        *x2ret = NULL;
    cvec         *devvec = NULL;
    cg_var       *cv;
    cxobj       **vec1 = NULL;
    size_t        vec1len;
    cxobj       **vec2 = NULL;
    size_t        vec2len;
    char         *devname;
    cxobj        *xdev;
    device_handle dh;
    char         *ct;
    char         *db;
    int           ret;

    if ((cbxpath = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xmldb_get_cache(h, "running", &xret, NULL) < 0)
        goto done;
    if (datastore_diff_nacm_read(h, xret, NULL) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xdev = cv_void_get(cv);
        if ((devname = xml_find_body(xdev, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        x1 = x1m = NULL;
        switch (dt1){
        case DT_RUNNING:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get_cache(h, "running", &x1ret, NULL) < 0)
                goto done;
            if (datastore_diff_nacm_read(h, x1ret, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_CANDIDATE:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            db = NULL;
            if (xmldb_find_create(h, "candidate", ceid, NULL, &db) < 0)
                goto done;
            if (db == NULL){
                clixon_err(OE_DB, 0, "No candidate");
                goto done;
            }
            if (xmldb_get_cache(h, db, &x1ret, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_ACTIONS:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get_cache(h, "actions", &x1ret, NULL) < 0)
                goto done;
            if (datastore_diff_nacm_read(h, x1ret, NULL) < 0)
                goto done;
            x1 = xpath_first(x1ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            ct = device_config_type_int2str(dt1);
            if ((ret = device_config_read_cache(h, devname, ct, &x1m, &cberr)) < 0)
                goto done;
            if (ret == 0){
                cbuf_reset(cbret);
                if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            if (datastore_diff_nacm_read(h, x1m, NULL) < 0)
                goto done;
            x1 = x1m;
            break;
        }
        x2 = x2m = NULL;
        switch (dt2){
        case DT_RUNNING:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get_cache(h, "running", &x2ret, NULL) < 0)
                goto done;
            if (datastore_diff_nacm_read(h, x2ret, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_CANDIDATE:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            db = NULL;
            if (xmldb_find_create(h, "candidate", ceid, NULL, &db) < 0)
                goto done;
            if (db == NULL){
                clixon_err(OE_DB, 0, "No candidate");
                goto done;
            }
            if (xmldb_get_cache(h, db, &x2ret, NULL) < 0)
                goto done;
            if (datastore_diff_nacm_read(h, x2ret, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_ACTIONS:
            cbuf_reset(cbxpath);
            cprintf(cbxpath, "devices/device[name='%s']/config", devname);
            if (xmldb_get_cache(h, "actions", &x2ret, NULL) < 0)
                goto done;
            if (datastore_diff_nacm_read(h, x2ret, NULL) < 0)
                goto done;
            x2 = xpath_first(x2ret, nsc, "%s", cbuf_get(cbxpath));
            break;
        case DT_SYNCED:
        case DT_TRANSIENT:
            ct = device_config_type_int2str(dt2);
            if ((ret = device_config_read_cache(h, devname, ct, &x2m, &cberr)) < 0)
                goto done;
            if (ret == 0){
                cbuf_reset(cbret);
                if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
                    goto done;
                goto ok;
            }
            if (datastore_diff_nacm_read(h, x2m, NULL) < 0)
                goto done;
            x2 = x2m;
            break;
        }
        switch (format){
        case FORMAT_XML:
            cbuf_reset(cb);
            if (clixon_xml_diff2cbuf(cb, x1, x2) < 0)
                goto done;
            if (cbuf_len(cb)){
                cprintf(cbret, "<diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
                cprintf(cbret, "%s:\n", devname);
                xml_chardata_cbuf_append(cbret, 0, cbuf_get(cb));
                cprintf(cbret, "</diff>");
            }
            break;
        case FORMAT_TEXT:
            cbuf_reset(cb);
            if (clixon_text_diff2cbuf(cb, x1, x2) < 0)
                goto done;
            if (cbuf_len(cb)){
                cprintf(cbret, "<diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
                cprintf(cbret, "%s:\n", devname);
                xml_chardata_cbuf_append(cbret, 0, cbuf_get(cb));
                cprintf(cbret, "</diff>");
            }
            break;
        case FORMAT_JSON:
        case FORMAT_CLI:
        default:
            break;
        }
        if (x1m)
            x1m = NULL;
        if (x2m)
            x2m = NULL;
        if (x1ret)
            x1ret = NULL;
        if (x2ret)
            x2ret = NULL;
    }
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (x1m)
        xml_free(x1m);
    if (x2m)
        xml_free(x2m);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    if (cberr)
        cbuf_free(cberr);
    if (cb)
        cbuf_free(cb);
    if (cbxpath)
        cbuf_free(cbxpath);
    return retval;
}

/*! Compare two data-stores by returning a diff-list in XML
 *
 * Compare two data-stores by returning a diff-list in XML.
 * There are two variants:
 *  1) Regular datastore references, such as running/candidate according to ietf-datastores YANG
 *  2) Clixon-controller specific device datastores
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
rpc_datastore_diff(clixon_handle h,
                   cxobj        *xe,
                   cbuf         *cbret,
                   void         *arg,
                   void         *regarg)
{
    int                retval = -1;
    client_entry      *ce = (client_entry *)arg;
    char              *ds1;
    char              *ds2;
    char              *id1 = NULL;
    char              *id2 = NULL;
    char              *db1 = NULL;
    char              *db2 = NULL;
    device_config_type dt1;
    device_config_type dt2;
    char              *xpath;
    char              *pattern;
    char              *formatstr;
    enum format_enum   format = FORMAT_XML;
    cxobj             *xn;
    int                groups = 0;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((formatstr = xml_find_body(xe, "format")) != NULL){
        if ((int)(format = format_str2int(formatstr)) < 0){
            clixon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
            goto done;
        }
        if (format != FORMAT_XML && format != FORMAT_TEXT){
            if (netconf_operation_failed(cbret, "application", "Format not supported")< 0)
                goto done;
            goto ok;
        }
    }
    if ((ds1 = xml_find_body(xe, "dsref1")) != NULL){ /* Regular datastores */
        xpath = xml_find_body(xe, "xpath");
        if (nodeid_split(ds1, NULL, &id1) < 0)
            goto done;
        // id1 -> candidate
        if ((ds2 = xml_find_body(xe, "dsref2")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No dsref2")< 0)
                goto done;
            goto ok;
        }
        if (nodeid_split(ds2, NULL, &id2) < 0)
            goto done;
        // id2 -> candidate
        if (xmldb_find_create(h, id1, ce->ce_id, NULL, &db1) < 0)
            goto done;
        if (xmldb_find_create(h, id2, ce->ce_id, NULL, &db2) < 0)
            goto done;
        clixon_debug(CLIXON_DBG_CTRL, "diff: %s vs %s", db1, db2);
        if (datastore_diff_dsref(h, xpath, db1, db2, format, cbret) < 0)
            goto done;
    }
    else{ /* Device specific datastores */
        if ((xn = xml_find(xe, "device")) != NULL)
            ;
        else if ((xn = xml_find(xe, "device-group")) != NULL)
            groups++;
        else {
            if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
                goto done;
            goto ok;
        }
        pattern = xml_body(xn);
        if ((ds1 = xml_find_body(xe, "config-type1")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No config-type1")< 0)
                goto done;
            goto ok;
        }
        if ((dt1 = device_config_type_str2int(ds1)) == -1){
            if (netconf_operation_failed(cbret, "application", "Unexpected config-type")< 0)
                goto done;
            goto ok;
        }
        if ((ds2 = xml_find_body(xe, "config-type2")) == NULL){
            if (netconf_operation_failed(cbret, "application", "No config-type1")< 0)
                goto done;
            goto ok;
        }
        if ((dt2 = device_config_type_str2int(ds2)) == -1){
            if (netconf_operation_failed(cbret, "application", "Unexpected config-type")< 0)
                goto done;
            goto ok;
        }
        clixon_debug(CLIXON_DBG_CTRL, "%s diff: %s vs %s", pattern, ds1, ds2);
        if (datastore_diff_device(h, groups, pattern, dt1, dt2, format, ce->ce_id, cbret) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (id1)
        free(id1);
    if (id2)
        free(id2);
    return retval;
}

/*! Intercept services-commit create-subscription and deny if there is already one
 *
 * The registration should be made from plugin-init to ensure the check is made before
 * the regular from_client_create_subscription callback
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see clixon-controller.yang notification services-commit
 */
int
check_services_commit_subscription(clixon_handle h,
                                   cxobj        *xe,
                                   cbuf         *cbret,
                                   void         *arg,
                                   void         *regarg)
{
    int                  retval = -1;
    char                *stream = "NETCONF";
    cxobj               *x; /* Generic xml tree */
    cvec                *nsc = NULL;
    event_stream_t      *es;
    struct stream_subscription *ss;
    int                         i;

    clixon_debug(CLIXON_DBG_CTRL, "");
    /* XXX should use prefix cf edit_config */
    if ((nsc = xml_nsctx_init(NULL, EVENT_RFC5277_NAMESPACE)) == NULL)
        goto done;
    if ((x = xpath_first(xe, nsc, "//stream")) == NULL ||
        (stream = xml_find_value(x, "body")) == NULL ||
        (es = stream_find(h, stream)) == NULL)
        goto ok;
    if (strcmp(stream, "services-commit") != 0)
        goto ok;
    if ((ss = es->es_subscription) != NULL){
        i = 0;
        do {
            ss = NEXTQ(struct stream_subscription *, ss);
            i++;
        } while (ss && ss != es->es_subscription);
        if (i>0){
            cbuf_reset(cbret);
            if (netconf_operation_failed(cbret, "application", "services-commit client already registered")< 0)
                goto done;
        }
    }
 ok:
    retval = 0;
 done:
    if (nsc)
        xml_nsctx_free(nsc);
    return retval;
}

/*! Transform XML of variables to cligen variable vector
 *
 * @param[in]  xvars  XML tree on the form: variables/variable/name,value
 * @param[out] cvv0   Cvec, free with cvec_free
 * @retval     0      OK
 * @retval    -1      Error
 */
static int
xvars2cvv(cxobj  *xvars,
          cvec  **cvv0)
{
    int    retval = -1;
    cxobj *xv;
    char  *name;
    char  *value;
    cvec  *cvv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    xv = NULL;
    while ((xv = xml_child_each(xvars, xv, CX_ELMNT)) != NULL) {
        name = xml_find_body(xv, "name");
        value = xml_find_body(xv, "value");
        if (cvec_add_string(cvv, name, value) < 0){
            clixon_err(OE_UNIX, errno, "cvec_add_string");
            goto done;
        }
    }
    *cvv0 = cvv;
    retval = 0;
 done:
    return retval;
}

/*! Apply config-template
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, eg client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see rpc_device_rpc_template_apply  RPC template
 */
static int
rpc_device_config_template_apply(clixon_handle h,
                                 cxobj        *xe,
                                 cbuf         *cbret,
                                 void         *arg,
                                 void         *regarg)
{
    int           retval = -1;
    client_entry *ce = (client_entry *)arg;
    cxobj        *xret = NULL;
    cvec         *nsc = NULL;
    int           groups = 0;
    cvec         *devvec = NULL;
    cg_var       *cv;
    cxobj       **vec1 = NULL;
    size_t        vec1len;
    cxobj       **vec2 = NULL;
    size_t        vec2len;
    cxobj        *xn;
    char         *tmplname;
    cxobj        *xtmpl;
    cxobj        *xvars;
    cxobj        *xvars0;
    cvec         *cvv = NULL;
    cxobj        *xv;
    char         *varname;
    char         *devname;
    char         *pattern;
    cxobj        *xerr = NULL;
    cxobj        *xtc = NULL;
    cxobj        *xroot = NULL;
    cxobj        *xmnt = NULL;
    cxobj        *x;
    cbuf         *cb = NULL;
    device_handle dh;
    yang_stmt    *yspec0;
    yang_stmt    *yspec1;
    char         *candidate = NULL;
    int           ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    yspec0 = clicon_dbspec_yang(h);
    if (xmldb_find_create(h, "candidate", ce->ce_id, NULL, &candidate) < 0)
        goto done;
    /* get template and device names
     * XXX: Destructively substitutes in xml_template_apply
     */
    if (xmldb_get0(h, "running", YB_MODULE, nsc, "devices", 1, WITHDEFAULTS_REPORT_ALL, &xret, NULL, NULL) < 0)
        goto done;
    if ((tmplname = xml_find_body(xe, "template")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No template in rpc")< 0)
            goto done;
        goto ok;
    }
    if ((xtmpl = xpath_first(xret, nsc, "devices/template[name='%s']/config", tmplname)) == NULL){
        if (netconf_operation_failed(cbret, "application", "Template not found")< 0)
            goto done;
        goto ok;
    }
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);
    xvars = xml_find_type(xe, NULL, "variables", CX_ELMNT);
    xvars0 = xpath_first(xret, nsc, "devices/template[name='%s']/variables", tmplname);
    /* Match actual parameters in xvars with formal paremeters in xvars0 */
    xv = NULL;
    while ((xv = xml_child_each(xvars, xv, CX_ELMNT)) != NULL) {
        varname = xml_find_body(xv, "name");
        if (xpath_first(xvars0, nsc, "variable[name='%s']", varname) == NULL){
            if (netconf_unknown_element(cbret, "application", varname, "No such template variable")< 0)
                goto done;
            goto ok;
        }
    }
    xv = NULL;
    while ((xv = xml_child_each(xvars0, xv, CX_ELMNT)) != NULL) {
        varname = xml_find_body(xv, "name");
        if (xpath_first(xvars, nsc, "variable[name='%s']", varname) == NULL){
            if (netconf_missing_element(cbret, "application", varname, "Template variable")< 0)
                goto done;
            goto ok;
        }
    }
    if (xvars2cvv(xvars, &cvv) < 0)
        goto done;
    /* Destructively substitute variables in xtempl
     * Maybe work on a copy instead?
     */
    if (cvv && xml_apply(xtmpl, CX_ELMNT, xml_template_apply, cvv) < 0)
        goto done;
    if (xml_sort_recurse(xtmpl) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<devices xmlns=\"%s\" xmlns:%s=\"%s\" %s:operation=\"merge\">",
            CONTROLLER_NAMESPACE,
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE, NETCONF_BASE_PREFIX);
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_state_mount_point_get(devname, yspec0, &xroot, &xmnt) < 0)
            goto done;
        yspec1 = NULL;
        if (controller_mount_yspec_get(h, devname, &yspec1) < 0)
            goto done;
        if (yspec1 == NULL){
            device_close_connection(dh, "No YANGs available");
            goto done;
        }
        if ((xtc = xml_dup(xtmpl)) == NULL)
            goto done;
        if ((ret = xml_bind_yang(h, xtc, YB_MODULE, yspec1, 0, &xerr)) < 0)
            goto done;
        if (ret == 0){
            if (clixon_xml2cbuf1(cbret, xerr, 0, 0, NULL, -1, 0, 0) < 0)
                goto done;
            goto ok;
        }
        while ((x = xml_child_i_type(xtc, 0, CX_ELMNT)) != NULL) {
            if (xml_addsub(xmnt, x) < 0)
                goto done;
        }
        if ((ret = xmldb_put(h, candidate, OP_MERGE, xroot, NULL, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto ok;
        xml_rm(xroot);
        if (xtc){
            xml_free(xtc);
            xtc = NULL;
        }
        if (xroot){
            xml_free(xroot);
            xroot = NULL;
        }
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cvv)
        cvec_free(cvv);
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xerr)
        xml_free(xerr);
    if (xtc)
        xml_free(xtc);
    if (xroot)
        xml_free(xroot);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! Send generic RPC to device within rpc transaction
 *
 * @param[in]  h        Clixon handle
 * @param[in]  dh       Device handle
 * @param[in]  tid      Transaction id
 * @param[in]  xconfig  RPC xml body
 * @param[out] cbret    Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1        OK
 * @retval     0        Fail, cbret set
 * @retval    -1        Error
 * @see device_send_generic_rpc
 */
static int
device_send_rpc_one(clixon_handle h,
                    device_handle dh,
                    uint64_t      tid,
                    cxobj        *xconfig,
                    cbuf         *cbret)
{
    int  retval = -1;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if (device_send_generic_rpc(h, dh, xconfig) < 0)
        goto done;
    if (device_state_set(dh, CS_RPC_GENERIC) < 0)
        goto done;
    device_handle_tid_set(dh, tid);
    retval = 1;
 done:
    return retval;
}

/*! Apply rpc-template
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, eg client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see rpc_device_config_template_apply  Config template
 */
static int
rpc_device_rpc_template_apply(clixon_handle h,
                              cxobj        *xe,
                              cbuf         *cbret,
                              void         *arg,
                              void         *regarg)
{
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    cxobj                  *xret = NULL;
    cvec                   *nsc = NULL;
    controller_transaction *ct = NULL;
    cbuf                   *cberr = NULL;
    int                     groups = 0;
    cvec                   *devvec = NULL;
    cg_var                 *cv;
    cxobj                 **vec1 = NULL;
    size_t                  vec1len;
    cxobj                 **vec2 = NULL;
    size_t                  vec2len;
    cxobj                  *xn;
    char                   *tmplname;
    cxobj                  *xinline;
    cxobj                  *xconfig = NULL;
    cxobj                  *xvars;
    cxobj                  *xvars0;
    cvec                   *cvv = NULL;
    cxobj                  *xv;
    char                   *varname;
    char                   *devname;
    char                   *pattern;
    device_handle           dh;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    /* Get template and device names.
     * XXX May destructively substitute xret in apply
     */
    if (xmldb_get0(h, "running", YB_MODULE, nsc, "devices", 1, WITHDEFAULTS_EXPLICIT, &xret, NULL, NULL) < 0)
        goto done;
    if ((tmplname = xml_find_body(xe, "template")) != NULL){
        if ((xconfig = xpath_first(xret, nsc, "devices/rpc-template[name='%s']/config", tmplname)) == NULL){
            if (netconf_operation_failed(cbret, "application", "Template config not found")< 0)
                goto done;
            goto ok;
        }
        xvars0 = xpath_first(xret, nsc, "devices/rpc-template[name='%s']/variables", tmplname);
    }
    else if ((xinline = xml_find_type(xe, NULL, "inline", CX_ELMNT)) != NULL) {
        if ((xconfig = xml_find_type(xinline, NULL, "config", CX_ELMNT)) == NULL) {
            if (netconf_operation_failed(cbret, "application", "Inline template config not found")< 0)
                goto done;
            goto ok;
        }
        xvars0 = xml_find_type(xinline, NULL, "variables", CX_ELMNT);
    }
    else{
        if (netconf_operation_failed(cbret, "application", "No template in rpc")< 0)
            goto done;
        goto ok;
    }
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);
    xvars = xml_find_type(xe, NULL, "variables", CX_ELMNT);

    /* Match actual parameters in xvars with formal paremeters in xvars0 */
    xv = NULL;
    while ((xv = xml_child_each(xvars, xv, CX_ELMNT)) != NULL) {
        varname = xml_find_body(xv, "name");
        if (xpath_first(xvars0, nsc, "variable[name='%s']", varname) == NULL){
            if (netconf_unknown_element(cbret, "application", varname, "No such template variable")< 0)
                goto done;
            goto ok;
        }
    }
    xv = NULL;
    while ((xv = xml_child_each(xvars0, xv, CX_ELMNT)) != NULL) {
        varname = xml_find_body(xv, "name");
        if (xpath_first(xvars, nsc, "variable[name='%s']", varname) == NULL){
            if (clixon_xml_parse_va(YB_NONE, NULL, &xvars, NULL, "<variable><name>%s</name><value></value></variable>", varname) < 0)
                goto done;
        }
    }
    if (xvars2cvv(xvars, &cvv) < 0)
        goto done;
    /* Destructively substitute variables in xtempl
     * Maybe work on a copy instead?
     */
    if (cvv && xconfig){
        if (xml_apply(xconfig, -1, xml_template_apply, cvv) < 0)
            goto done;
        if (xml_sort_recurse(xconfig) < 0)
            goto done;
    }
    /* Initiate new transaction */
    if ((ret = controller_transaction_new(h, ce, clicon_username_get(h), "rpc", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN)
            continue;
        if ((ret = device_send_rpc_one(h, dh, ct->ct_id, xconfig, cbret)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto ok;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");

    /* No device started, close transaction */
    if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
        if (controller_transaction_failed(h, ct->ct_id, ct, NULL, TR_FAILED_DEV_IGNORE, "backend", "No device connected") < 0)
            goto done;
        if (controller_transaction_done(h, ct, TR_FAILED) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (cvv)
        cvec_free(cvv);
    if (xret)
        xml_free(xret);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! Apply device template callback, see clixon-controller.yang: devices/template/apply
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, eg client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see rpc_device_rpc_template_apply  RPC template
 */
int
rpc_device_template_apply(clixon_handle h,
                          cxobj        *xe,
                          cbuf         *cbret,
                          void         *arg,
                          void         *regarg)
{
    int   retval = -1;
    char *type;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((type = xml_find_body(xe, "type")) == NULL){
        if (netconf_operation_failed(cbret, "application", "No type in rpc")< 0)
            goto done;
    }
    else if (strcmp(type, "CONFIG") == 0){
        if (rpc_device_config_template_apply(h, xe, cbret, arg, regarg) < 0)
            goto done;
    }
    else if (strcmp(type, "RPC") == 0){
        if (rpc_device_rpc_template_apply(h, xe, cbret, arg, regarg) < 0)
            goto done;
    }
    else {
        if (netconf_operation_failed(cbret, "application", "Invalid type in RPC")< 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Apply device template callback, see clixon-controller.yang: devices/template/apply
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     Domain specific arg, eg client-entry or FCGX_Request
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see rpc_device_rpc_template_apply  RPC template
 */
int
rpc_device_rpc(clixon_handle h,
               cxobj        *xe,
               cbuf         *cbret,
               void         *arg,
               void         *regarg)
{
    client_entry           *ce = (client_entry *)arg;
    int                     retval = -1;
    cxobj                  *xret = NULL;
    cvec                   *nsc = NULL;
    controller_transaction *ct = NULL;
    cbuf                   *cberr = NULL;
    int                     groups = 0;
    cvec                   *devvec = NULL;
    cg_var                 *cv;
    cxobj                 **vec1 = NULL;
    size_t                  vec1len;
    cxobj                 **vec2 = NULL;
    size_t                  vec2len;
    cxobj                  *xn;
    char                   *syncstr;
    cxobj                  *xconfig = NULL;
    char                   *devname;
    char                   *pattern;
    device_handle           dh;
    int                     ret;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if ((syncstr = xml_find_body(xe, "sync")) != NULL){
        if (strcmp(syncstr, "true") == 0){
            if (netconf_operation_failed(cbret, "application", "sync=true not allowed to backend")< 0)
                goto done;
            goto ok;
        }
    }
    /* Get device names */
    if (xmldb_get0(h, "running", YB_MODULE, nsc, "devices", 1, WITHDEFAULTS_EXPLICIT, &xret, NULL, NULL) < 0)
        goto done;
    if ((xn = xml_find(xe, "device")) != NULL)
        ;
    else if ((xn = xml_find(xe, "device-group")) != NULL)
        groups++;
    else {
        if (netconf_operation_failed(cbret, "application", "No device or device-group")< 0)
            goto done;
        goto ok;
    }
    pattern = xml_body(xn);

    if ((xconfig = xml_find_type(xe, NULL, "config", CX_ELMNT)) == NULL) {
        if (netconf_operation_failed(cbret, "application", "Inline template config not found")< 0)
            goto done;
        goto ok;
    }
    /* Initiate new transaction */
    if ((ret = controller_transaction_new(h, ce, clicon_username_get(h), "rpc", &ct, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_operation_failed(cbret, "application", "%s", cbuf_get(cberr))< 0)
            goto done;
        goto ok;
    }
    if ((devvec = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xpath_vec(xret, nsc, "devices/device", &vec1, &vec1len) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device-group", &vec2, &vec2len) < 0)
        goto done;
    if (!groups){
        if (iterate_device(h, pattern, vec1, vec1len, devvec) < 0)
            goto done;
    }
    else{
        if (iterate_device_group(h, pattern, vec1, vec1len, vec2, vec2len, devvec) < 0)
            goto done;
    }
    clearvec(h, vec1, vec1len);
    clearvec(h, vec2, vec2len);
    if (vec1){
        free(vec1);
        vec1 = NULL;
    }
    if (vec2){
        free(vec2);
        vec2 = NULL;
    }
    cv = NULL;
    while ((cv = cvec_each(devvec, cv)) != NULL){
        xn = cv_void_get(cv);
        if ((devname = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN)
            continue;
        if ((ret = device_send_rpc_one(h, dh, ct->ct_id, xconfig, cbret)) < 0)
            goto done;
        if (ret == 0)  /* Failed but cbret set */
            goto ok;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<tid xmlns=\"%s\">%" PRIu64"</tid>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");

    /* No device started, close transaction */
    if (controller_transaction_nr_devices(h, ct->ct_id) == 0){
        if (controller_transaction_failed(h, ct->ct_id, ct, NULL, TR_FAILED_DEV_IGNORE, "backend", "No device connected") < 0)
            goto done;
        if (controller_transaction_done(h, ct, TR_FAILED) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (xret)
        xml_free(xret);
    if (vec1)
        free(vec1);
    if (vec2)
        free(vec2);
    if (devvec)
        cvec_free(devvec);
    if (nsc)
        cvec_free(nsc);
    return retval;
}

/*! Given an attribute name and its expected namespace, find its value
 *
 * An attribute may have a prefix(or NULL). The routine finds the associated
 * xmlns binding to find the namespace: <namespace>:<name>.
 * If such an attribute is not found, failure is returned with cbret set,
 * If such an attribute is found, its string value is returned and removed from XML
 * @param[in]  x         XML node (where to look for attribute)
 * @param[in]  name      Attribute name
 * @param[in]  ns        (Expected) Namespace of attribute
 * @param[out] cbret     Error message (if retval=0)
 * @param[out] valp      Malloced value (if retval=1)
 * @retval     1         OK
 * @retval     0         Failed (cbret set)
 * @retval    -1         Error
 * @note as a side.effect the attribute is removed
 * @note slightly modified from clixon_datastore_write.c
 */
static int
attr_ns_value(cxobj *x,
              char  *name,
              char  *ns,
              char **valp)
{
    int    retval = -1;
    cxobj *xa;
    char  *ans = NULL; /* attribute namespace */
    char  *val = NULL;

    /* prefix=NULL since we do not know the prefix */
    if ((xa = xml_find_type(x, NULL, name, CX_ATTR)) != NULL){
        if (xml2ns(xa, xml_prefix(xa), &ans) < 0)
            goto done;
        if (ans == NULL){ /* the attribute exists, but no namespace */
            goto fail;
        }
        /* the attribute exists, but not w expected namespace */
        if (ns == NULL ||
            strcmp(ans, ns) == 0){
            if ((val = strdup(xml_value(xa))) == NULL){
                clixon_err(OE_UNIX, errno, "malloc");
                goto done;
            }
            xml_purge(xa);
        }
    }
    *valp = val;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Look for creator attributes in edit-config, remove it and create entry in service instance
 *
 * Callback function type for xml_apply
 * @param[in]  x    XML node
 * @param[in]  arg  General-purpose argument
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
creator_applyfn(cxobj *x,
                void  *arg)
{
    int        retval = -1;
    cxobj     *xserv = (cxobj*)arg;
    char      *creator = NULL;
    char      *service = NULL;
    cvec      *nsc = 0;
    char      *xpath = NULL;
    cxobj     *xi;
    cxobj     *xc;
    char      *instance;
    char      *p;
    char       q;
    yang_stmt *yserv;
    yang_stmt *yi;
    char      *ns;
    cvec      *cvk;
    char      *key;
    char      *ykey;
    int        ret;

    /* Special clixon-lib attribute for keeping track of creator of objects */
    if ((ret = attr_ns_value(x, "creator", CLIXON_LIB_NS, &creator)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (creator != NULL){
        if (xml2xpath(x, nsc, 0, 0, &xpath) < 0)
            goto done;
        /* Find existing entry in xserv, if not found create it */
        if ((xi = xpath_first(xserv, NULL, "%s", creator)) != NULL){
            if ((xc = xml_find_type(xi, NULL, "created", CX_ELMNT)) == NULL)
                goto ok;
            if (xpath_first(xc, 0, "path[.='%s']", xpath) != NULL)
                goto ok; /* duplicate: silently drop */
            clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "Created path: %s %s", xpath, creator);
            if ((ret = clixon_xml_parse_va(YB_PARENT, NULL, &xc, NULL, "<path>%s</path>", xpath)) < 0)
                goto done;
            if (ret == 0)
                goto ok;
        }
        else {
            /* split creator into service, key and instance, assuming creator is on the form:
             * service[key='myname']
             */
            if ((service = strdup(creator)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if ((p = index(service, '[')) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no instance: [] in %s", creator);
                goto done;
            }
            *p++ = '\0';
            key = p;
            if ((p = index(p, '=')) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no instance = in %s", creator);
                goto done;
            }
            *p++ = '\0';
            q = *p++; /* assume quote */
            instance = p;
            if ((p = index(p, q)) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no quote in %s", creator);
                goto done;
            }
            *p = '\0';
            yserv = xml_spec(xserv);
            if ((yi = yang_find(yserv, Y_LIST, service)) == NULL){
                clixon_err(OE_YANG, 0, "Invalid creator service name in %s", creator);
                goto done;
            }
            if ((cvk = yang_cvec_get(yi)) == NULL)
                goto ok;
            if ((ykey = cvec_i_str(cvk, 0)) == NULL)
                goto ok;
            if (strcmp(key, ykey) != 0){
                clixon_err(OE_YANG, 0, "Creator tag: \"%s\": Invalid key: \"%s\", expected: \"%s\"", creator, key, ykey);
                goto done;
            }
            if ((ns = yang_find_mynamespace(yi)) == NULL)
                goto ok;
            clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "Created path: %s %s", xpath, creator);
            if ((ret = clixon_xml_parse_va(YB_PARENT, NULL, &xserv, NULL,
                                           "<%s xmlns=\"%s\"><%s>%s</%s>"
                                           "<created nc:operation=\"merge\">"
                                           "<path>%s</path></created></%s>",
                                           service,
                                           ns,
                                           ykey,
                                           instance,
                                           ykey,
                                           xpath,
                                           service)) < 0)
                goto done;
            if (ret == 0)
                goto ok;
        }
    }
 ok:
    retval = 0;
 done:
    if (creator)
        free(creator);
    if (service)
        free(service);
    if (xpath)
        free(xpath);
    return retval;
 fail:
    retval = 1;
    goto done;
}

/*! Controller wrapper of edit-config
 *
 * Find and remove creator attributes and create services/../created structures.
 * Ignore all semantic errors, trust base function error-handling
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see from_client_edit_config
 */
int
controller_edit_config(clixon_handle h,
                       cxobj        *xe,
                       cbuf         *cbret,
                       void         *arg,
                       void         *regarg)
{
    int        retval = -1;
    cxobj     *xc;
    cvec      *nsc = NULL;
    char      *target;
    yang_stmt *yspec;
    cxobj     *xconfig = NULL;
    cxobj     *xserv;
    int        ret;

    clixon_debug(CLIXON_DBG_CTRL, "wrapper");
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_YANG, ENOENT, "No yang spec9");
        goto done;
    }
    if (xml_nsctx_node(xe, &nsc) < 0)
        goto done;
    if ((target = netconf_db_find(xe, "target")) == NULL)
        goto ok;
    /* Get config element */
    if ((xc = xpath_first(xe, nsc, "%s", NETCONF_INPUT_CONFIG)) == NULL){
        goto ok;
    }
    if ((ret = xml_bind_yang(h, xc, YB_MODULE, yspec, 0, NULL)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
    if ((xconfig = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
        goto done;
    if (clixon_xml_parse_va(YB_NONE, NULL, &xconfig, NULL,
                            "<services xmlns=\"%s\" xmlns:nc=\"%s\"/>",
                            CONTROLLER_NAMESPACE,
                            NETCONF_BASE_NAMESPACE) < 0){
        goto ok;
    }
    if ((xserv = xml_find_type(xconfig, NULL, "services", CX_ELMNT)) == NULL)
        goto ok;
    if ((ret = xml_bind_yang0(h, xserv, YB_MODULE, yspec, 0, NULL)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
    if (xml_spec(xserv) == NULL)
        goto ok;
    if ((ret = xml_apply(xc, CX_ELMNT, creator_applyfn, xserv)) < 0)
        goto done;
    if (ret == 1){
        if (netconf_operation_failed(cbret, "application", "Translation for creator attributes to created tag")< 0)
            goto done;
        goto ok;
    }
    if (xml_child_nr_type(xserv, CX_ELMNT) == 0)
        goto ok;
    clixon_debug_xml(CLIXON_DBG_CTRL, xserv, "Objects created in %s-db", target);
    if ((ret = xmldb_put(h, target, OP_NONE, xconfig, NULL, cbret)) < 0){
        if (netconf_operation_failed(cbret, "protocol", "%s", clixon_err_reason())< 0)
            goto done;
        goto ok;
    }
 ok:
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xconfig)
        xml_free(xconfig);
    return retval;
}

/*! Register callback for rpc calls
 */
int
controller_rpc_init(clixon_handle h)
{
    int retval = -1;

    if (rpc_callback_register(h, rpc_config_pull,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "config-pull"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_controller_commit,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "controller-commit"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_connection_change,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "connection-change"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_get_device_config,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "get-device-config"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_transaction_error,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "transaction-error"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_transactions_actions_done,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "transaction-actions-done"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_datastore_diff,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "datastore-diff"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_device_template_apply,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "device-template-apply"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_device_rpc,
                              NULL,
                              CONTROLLER_NAMESPACE,
                              "device-rpc"
                              ) < 0)
        goto done;
    /* Check that services subscriptions is just done once */
    if (rpc_callback_register(h, check_services_commit_subscription,
                              NULL,
                              EVENT_RFC5277_NAMESPACE, "create-subscription") < 0)
        goto done;
    /* Wrapper of standard RPCs */
    if (rpc_callback_register(h, controller_edit_config,
                              NULL,
                              NETCONF_BASE_NAMESPACE,
                              "edit-config"
                              ) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
