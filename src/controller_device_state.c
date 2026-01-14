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
  Device connection state machine:

 CS_CLOSED \
     ^      \ connect
     |       v        send get
     |<-- CS_CONNECTING
     |       |
     |       v
     |<-- CS_SCHEMA_LIST
     |       |       \
     |       |        v
     |<-------- CS_SCHEMA_ONE(n) ---+
     |       |       /           <--+
     |       v      v
     |<-- CS_DEVICE_SYNC
     |      /
     |     /
 CS_OPEN <+

Push state-machine:

 PUSH-WAIT -------
     ^            \
     |             v
 PUSH-VALIDATE   PUSH-COMMIT
     ^      \               \
     |       v               v
 PUSH-EDIT2  PUSH-DISCARD   PUSH-COMMIT-SYNC*
     ^            |          /
     |            v         /
 PUSH-EDIT   PUSH-UNLOCK <--
     ^            |
     |            |
 PUSH-LOCK        |
     ^           /
     |          /
 CS_OPEN <------

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
#include "controller_lib.h"
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_device_recv.h"

/*! Mapping between enum conn_state and yang connection-state
 *
 * @see clixon-controller@2023-01-01.yang connection-state
 */
static const map_str2int csmap[] = {
    {"CLOSED",           CS_CLOSED},
    {"OPEN",             CS_OPEN},
    /* Connect state machine */
    {"CONNECTING",       CS_CONNECTING},
    {"SCHEMA-LIST",      CS_SCHEMA_LIST},
    {"SCHEMA-ONE",       CS_SCHEMA_ONE}, /* substate is schema-nr */
    {"DEVICE-SYNC",      CS_DEVICE_SYNC},
    /* Push state machine */
    {"PUSH-LOCK",        CS_PUSH_LOCK},
    {"PUSH-CHECK",       CS_PUSH_CHECK},
    {"PUSH-EDIT",        CS_PUSH_EDIT},
    {"PUSH-EDIT2",       CS_PUSH_EDIT2},
    {"PUSH-VALIDATE",    CS_PUSH_VALIDATE},
    {"PUSH-WAIT",        CS_PUSH_WAIT},
    {"PUSH-COMMIT",      CS_PUSH_COMMIT},
    {"PUSH-COMMIT-SYNC", CS_PUSH_COMMIT_SYNC},
    {"PUSH-DISCARD",     CS_PUSH_DISCARD},
    {"PUSH-UNLOCK",      CS_PUSH_UNLOCK},
    /* Generic RPC state machine */
    {"RPC-GENERIC",      CS_RPC_GENERIC},
    {NULL,              -1}
};

/*! Mapping between enum yang_config and yang config
 *
 * How to bind device configuration to YANG
 * @see clixon-controller@2023-01-01.yang yang-config
 * @see enum yang_config
 */
static const map_str2int yfmap[] = {
    {"NONE",     YF_NONE},
    {"BIND",     YF_BIND},
    {"VALIDATE", YF_VALIDATE},
    {NULL,       -1}
};

/*! Map controller device connection state from int to string
 *
 * @param[in]  state  Device state as int
 * @retval     str    Device state as string
 */
char *
device_state_int2str(conn_state state)
{
    return (char*)clicon_int2str(csmap, state);
}

/*! Map controller device connection state from string to int
 *
 * @param[in]  str    Device state as string
 * @retval     state  Device state as int
 */
conn_state
device_state_str2int(char *str)
{
    return clicon_str2int(csmap, str);
}

/*! Map yang config from string to int
 *
 * @param[in]  str    Yang config as string
 * @retval     yf     Yang config as int
 */
yang_config_t
yang_config_str2int(char *str)
{
    return clicon_str2int(yfmap, str);
}

/*! Close connection, unregister events and timers
 *
 * @param[in]  dh      Clixon device handle.
 * @param[in]  format  Format string for Log message or NULL
 * @retval     0       OK
 * @retval    -1       Error
 * @note If device is part of transaction, you should call controller_transaction_failed() either:
 *       - instead with devclose = TR_FAILED_DEV_CLOSE
 *       - sfter with devclose = TR_FAILED_DEV_LEAVE
 */
int
device_close_connection(device_handle dh,
                        const char   *format, ...)
{
    int      retval = -1;
    va_list  ap;
    int      len;
    char    *str = NULL;
    int      s;
    char    *name;

    name = device_handle_name_get(dh);
    device_handle_outmsg_set(dh, 1, NULL);
    device_handle_outmsg_set(dh, 2, NULL);
    if (format == NULL){
        clixon_debug(CLIXON_DBG_CTRL, "%s", name);
        device_handle_logmsg_set(dh, NULL);
    }
    else {
        va_start(ap, format); /* dryrun */
        if ((len = vsnprintf(NULL, 0, format, ap)) < 0) /* dryrun, just get len */
            goto done;
        va_end(ap);
        if ((str = malloc(len+1)) == NULL){
            clixon_err(OE_UNIX, errno, "malloc");
            goto done;
        }
        va_start(ap, format); /* real */
        if (vsnprintf(str, len+1, format, ap) < 0){
            clixon_err(OE_UNIX, errno, "vsnprintf");
            goto done;
        }
        va_end(ap);
        clixon_debug(CLIXON_DBG_CTRL, "%s %s", name, str);
    }
    /* Handle case already closed */
    if ((s = device_handle_socket_get(dh)) != -1){
        clixon_event_unreg_fd(s, device_input_cb); /* deregister events */
        if (device_handle_disconnect(dh) < 0) /* close socket, reap sub-processes */
            goto done;
    }
#if 0
    /* Clear yangs for domain changes, upgrade etc on close,
     * The drawback is you cannot run the CLI on disconnected devices
     * Alternatively clear in controller_connect()
     */
    device_handle_yang_lib_set(dh, NULL);
#endif
    if (device_state_set(dh, CS_CLOSED) < 0)
        goto done;
    if (str){
        device_handle_logmsg_set(dh, str);
        str = NULL;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "retval: %d", retval);
    if (str)
        free(str);
    return retval;
}

/*! Handle input data from device, whole or part of a frame, called by event loop
 *
 * @param[in] s    Socket
 * @param[in] arg  Device handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int
device_input_cb(int   s,
                void *arg)
{
    int                     retval = -1;
    device_handle           dh = (device_handle)arg;
    clixon_handle           h;
    unsigned char           buf[BUFSIZ]; /* from stdio.h, typically 8K */
    ssize_t                 buflen = sizeof(buf);
    char                   *buferr=NULL; /* from stderr.h, typically 8K */
    ssize_t                 buferrlen = 1024;
    int                     eom = 0;
    int                     eof = 0;
    int                     frame_state; /* only used for chunked framing not eom */
    size_t                  frame_size;
    netconf_framing_type    framing_type;
    cbuf                   *cbmsg;
    cbuf                   *cberr = NULL;
    cxobj                  *xtop = NULL;
    cxobj                  *xmsg;
    cxobj                  *xerr = NULL;
    unsigned char          *p = buf;
    ssize_t                 len;
    size_t                  plen;
    char                   *name;
    uint64_t                tid;
    controller_transaction *ct = NULL;
    int                     sockerr;
    int                     ret;

    h = device_handle_handle_get(dh);
    frame_state = device_handle_frame_state_get(dh);
    frame_size = device_handle_frame_size_get(dh);
    cbmsg = device_handle_frame_buf_get(dh);
    name = device_handle_name_get(dh);
    if ((tid = device_handle_tid_get(dh)) != 0)
        ct = controller_transaction_find(h, tid);
    /* Read input data from socket and append to cbbuf */
    if ((len = netconf_input_read2(s, buf, buflen, &eof)) < 0)
        goto done;
    if (eof){
        if ((sockerr = device_handle_sockerr_get(dh)) != -1){
            if ((buferr = malloc(buferrlen)) == NULL){
                clixon_err(OE_UNIX, errno, "malloc");
                goto done;
            }
            memset(buferr, 0, buferrlen);
            if (clixon_event_poll(sockerr) == 0){
                strncpy(buferr, "ssh sub-process killed", buferrlen);
            }
            else {
                if ((len = read(sockerr, buferr, buferrlen-1)) < 0){ // XXX hangs on SIGCHLD?
                    free(buferr);
                    buferr = NULL;
                }
                /* Special case for removing CR at end of stderr string */
                while (len>0 && (buferr[len-1] == '\r' || buferr[len-1] == '\n')) {
                    buferr[len - 1] = '\0';
                    len--;
                }
            }
        }
        if (ct){
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name,
                                              buferr?buferr:"Closed by device"
                                              ) < 0)
                goto done;
        }
        else{
            if (buferr)
                device_close_connection(dh, "%s", buferr);
            else
                device_close_connection(dh, "Closed by device");
        }
        goto ok;
    }
    p = buf;
    plen = len;
    while (!eof && plen > 0){
        framing_type = device_handle_framing_type_get(dh);
        if (netconf_input_msg2(&p, &plen,
                               cbmsg,
                               framing_type,
                               &frame_state,
                               &frame_size,
                               &eom) < 0)
            goto done;
        if (eom == 0){ /* frame not complete */
            clixon_debug(CLIXON_DBG_MSG | CLIXON_DBG_DETAIL2, "frame: %lu", cbuf_len(cbmsg));
            /* Extra data to read, save data and continue on next round */
            break;
        }
        if (clixon_debug_detail())
            clixon_debug(CLIXON_DBG_MSG | CLIXON_DBG_DETAIL, "Recv [%s]: %s", name, cbuf_get(cbmsg));
        else
            clixon_debug(CLIXON_DBG_MSG, "Recv [%s] len: %lu", name, cbuf_len(cbmsg));
        if ((ret = netconf_input_frame2(cbmsg, YB_NONE, NULL, &xtop, &xerr)) < 0)
            goto done;
        cbuf_reset(cbmsg);
        if (ret == 0){
            if ((cberr = cbuf_new()) == NULL){
                clixon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            if (netconf_err2cb(h, xerr, cberr) < 0)
                goto done;
            if (ct){
                // use XXX cberr but its XML
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, "Invalid frame") < 0)
                    goto done;
            }
            else
                device_close_connection(dh, "Invalid frame");
            goto ok;
        }
        xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
        if ((xmsg = xml_child_i_type(xtop, 0, CX_ELMNT)) != NULL) {
            /* Main state machine for controller transactions+devices */
            if (device_state_handler(h, dh, s, xmsg) < 0)
                goto done;
        }
    } /* while */
    device_handle_frame_state_set(dh, frame_state);
    device_handle_frame_size_set(dh, frame_size);
 ok:
    retval = 0;
 done:
    if (buferr)
        free(buferr);
    if (cberr)
        cbuf_free(cberr);
    if (xerr)
        xml_free(xerr);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Given devicename and XML tree, create XML tree and device mount-point
 *
 * @param[in]  devicename Name of device
 * @param[in]  yspec      Top level Yang spec
 * @param[out] xt         Top of created tree (Deallocate with xml_free)
 * @param[out] xroot      XML mount-point in created tree
 * @retval     0          OK
 * @retval    -1          Error
 */
int
device_state_mount_point_get(char      *devicename,
                             yang_stmt *yspec,
                             cxobj    **xtp,
                             cxobj    **xrootp)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;
    cxobj *xroot;
    int    ret;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<devices xmlns=\"%s\" xmlns:nc=\"%s\"><device><name>%s</name>",
            CONTROLLER_NAMESPACE,
            NETCONF_BASE_NAMESPACE, /* needed for nc:op below */
            devicename);
    cprintf(cb, "<config/>");
    cprintf(cb, "</device></devices>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &xt, NULL)) < 0)
        goto done;
    if (xml_name_set(xt, "config") < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/config")) == NULL){
        clixon_err(OE_XML, 0, "device/config mountpoint not found");
        goto done;
    }
    *xtp = xt;
    xt = NULL;
    *xrootp = xroot;
    retval = 0;
 done:
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! All schemas (yang_lib) ready from one device, load them from file into yspec mount
 *
 * @param[in] h        Clixon handle.
 * @param[in] dh       Clixon client handle.
 * @param[in] xyanglib XML tree of yang module-set
 * @retval    1        OK
 * @retval    0        Fail, parse or other error, device is closed
 * @retval   -1        Error
 * @see device_send_get_schema_next  Check local/ send a schema request
 */
static int
device_schemas_mount_parse(clixon_handle h,
                           device_handle dh,
                           cxobj        *xyanglib)
{
    int        retval = -1;
    yang_stmt *yspec1;
    char      *domain;
    int        ret;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    if (xpath_first(xyanglib, 0, "module-set/module") == NULL){
        /* No modules: No local and no */
        device_close_connection(dh, "Empty set of YANG modules");
        goto fail;
    }
    if (controller_mount_yspec_get(h,
                                   device_handle_name_get(dh),
                                   &yspec1) < 0)
        goto done;
    if (yspec1 == NULL){
        clixon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    if ((domain = device_handle_domain_get(dh)) == NULL){
        clixon_err(OE_YANG, 0, "No YANG domain");
        goto done;
    }
    /* Given yang-lib, actual parsing of all modules into yspec */
    if ((ret = yang_lib2yspec(h, xyanglib, device_handle_name_get(dh), domain, yspec1)) < 0)
        goto done;
    if (ret == 0){
        device_close_connection(dh, "%s", clixon_err_reason());
        clixon_err_reset();
        goto fail;
    }
    retval = 1;
 done:
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "retval %d", retval);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check local yang schemas exist
 *
 * @param[in] h        Clixon handle.
 * @param[in] dh       Clixon client handle.
 * @param[in] yspec0   Top-level Yang specs
 * @param[in] xyanglib XML tree of yang module-set
 * @retval    1        OK
 * @retval    0        A yang is not found locally, device is closed
 * @retval   -1        Error
 */
static int
device_schemas_local_check(clixon_handle h,
                           device_handle dh,
                           cxobj        *xyanglib)

{
    int     retval = -1;
    cxobj **vec = NULL;
    size_t  veclen;
    cxobj  *xi;
    int     i;
    char   *name;
    char   *revision;
    cvec   *nsc = NULL;
    char   *domain = NULL;
    int     ret;

    if ((domain = device_handle_domain_get(dh)) == NULL){
        clixon_err(OE_YANG, 0, "No YANG domain");
        goto done;
    }
    if (xpath_vec(xyanglib, nsc, "module-set/module", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xi = vec[i];
        if ((name = xml_find_body(xi, "name")) == NULL)
            continue;
        revision = xml_find_body(xi, "revision");
        if ((ret = yang_file_find_match(h, name, revision, domain, NULL)) < 0)
            goto done;
        if (ret == 0){
            if (device_close_connection(dh, "Yang \"%s\" not found in the list of CLICON_YANG_DIRs", name) < 0)
                goto done;
            goto fail;
        }
    }
    retval = 1;
 done:
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Timeout callback of transient states, close connection
 *
 * @param[in] arg    In effect client handle
 * @retval    0      OK
 * @retval   -1      Error
 */
static int
device_state_timeout(int   s,
                     void *arg)
{
    int                     retval = -1;
    device_handle           dh = (device_handle)arg;
    uint64_t                tid;
    controller_transaction *ct = NULL;
    clixon_handle           h;
    char                   *name;

    name = device_handle_name_get(dh);
    clixon_debug(CLIXON_DBG_CTRL, "%s", name);
    h = device_handle_handle_get(dh);
    clixon_log(h, LOG_NOTICE, "%s Device state timeout. Waiting for device %s to change state from %s",
               __func__, name, device_state_int2str(device_handle_conn_state_get(dh)));
    if ((tid = device_handle_tid_get(dh)) != 0){
        ct = controller_transaction_find(h, tid);
    }
    if (ct){
        if (controller_transaction_failed(device_handle_handle_get(dh), tid, ct, dh, TR_FAILED_DEV_CLOSE, name, "Timeout waiting for remote peer") < 0)
            goto done;
    }
    else if (device_close_connection(dh, "Timeout waiting for remote peer") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Set timeout of transient device state
 *
 * @param[in] dh  Device handle
 * @retval    0      OK
 * @retval   -1      Error
 */
int
device_state_timeout_register(device_handle dh)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    int            d;
    clixon_handle  h;
    cbuf          *cb = NULL;
    char          *name;

    name = device_handle_name_get(dh);
    gettimeofday(&t, NULL);
    h = device_handle_handle_get(dh);
    if ((d = clicon_data_int_get(h, "controller-device-timeout")) < 0)
        t1.tv_sec = CONTROLLER_DEVICE_TIMEOUT_DEFAULT;
    else
        t1.tv_sec = d;
    t1.tv_usec = 0;
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "timeout:%ld s", t1.tv_sec);
    timeradd(&t, &t1, &t);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "Device %s in state %s",
            name, device_state_int2str(device_handle_conn_state_get(dh)));
    if (clixon_event_reg_timeout(t, device_state_timeout, dh, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Cancel timeout of transient device state
 *
 * @param[in] dh  Device handle
 */
int
device_state_timeout_unregister(device_handle dh)
{
    char *name;

    name = device_handle_name_get(dh);
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "%s", name);
    (void)clixon_event_unreg_timeout(device_state_timeout, dh);
    return 0;
}

/*! Restart timer (stop; start)
 *
 * @param[in] dh  Device handle
 * @retval    0   OK
 * @retval   -1   Error
 */
static int
device_state_timeout_restart(device_handle dh)
{
    int retval = -1;

    if (device_state_timeout_unregister(dh) < 0)
        goto done;
    if (device_state_timeout_register(dh) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Combined function to both change device state and set/reset/unregister timeout
 *
 * And possibly other "high-level" action associated with state change
 * @param[in]   dh     Device handle
 * @param[in]   state  State
 * @retval      0      OK
 * @retval     -1      Error
 */
int
device_state_set(device_handle dh,
                 conn_state    state)
{
    int        retval = -1;
    conn_state state0;

    /* From state handling */
    state0 = device_handle_conn_state_get(dh);
    if (state0 != CS_CLOSED && state0 != CS_OPEN){
        if (device_state_timeout_unregister(dh) < 0)
            goto done;
    }
    /* To state handling */
    device_handle_conn_state_set(dh, state);
    if (state != CS_CLOSED && state != CS_OPEN){
        if (device_state_timeout_register(dh) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Write device config to db file without sanity of yang checks
 *
 * @param[in]  h           Clixon handle.
 * @param[in]  devname     Device name
 * @param[in]  config_type Device config tyoe
 * @param[in]  xdata       XML tree to write
 * @param[out] cbret       Initialized cligen buffer. On exit contains XML if retval == 0
 * @retval     1           OK
 * @retval     0           Fail (cbret set)
 * @retval    -1           Error
 */
int
device_config_write(clixon_handle h,
                    char         *devname,
                    char         *config_type,
                    cxobj        *xdata,
                    cbuf         *cbret)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    char  *db;

    if (devname == NULL || config_type == NULL){
        clixon_err(OE_UNIX, EINVAL, "devname or config_type is NULL");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "device-%s-%s", devname, config_type);
    db = cbuf_get(cb);
    if (xmldb_db_reset(h, db) < 0)
        goto done;
    retval = xmldb_put(h, db, OP_REPLACE, xdata, clicon_username_get(h), cbret);
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Get local copy device datastore
 *
 * @param[in]  h           Clixon handle
 * @param[in]  name        Device name
 * @param[in]  config_type Device config tyoe
 * @param[out] xdatap      Device config XML (if retval=1)
 * @param[out] cberr       Error message (if retval=0)
 * @retval     1           OK
 * @retval     0           Failed (No such device tree)
 * @retval    -1           Error
 */
int
device_config_read(clixon_handle h,
                   char         *devname,
                   char         *config_type,
                   cxobj       **xdatap,
                   cbuf        **cberr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    char  *db;
    cvec  *nsc = NULL;
    cxobj *xt = NULL;
    cxobj *xroot;

    if (devname == NULL || config_type == NULL){
        clixon_err(OE_UNIX, EINVAL, "devname or config_type is NULL");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "device-%s-%s", devname, config_type);
    db = cbuf_get(cb);
    if (xmldb_get0(h, db, YB_MODULE, nsc, NULL, 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/config")) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(*cberr, "No such device tree");
        goto failed;
    }
    if (xdatap){
        xml_rm(xroot);
        *xdatap = xroot;
    }
    retval = 1;
 done:
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Get local cache device datastore
 *
 * @param[in]  h           Clixon handle
 * @param[in]  name        Device name
 * @param[in]  config_type Device config tyoe
 * @param[out] xdatap      Device config XML (if retval=1)
 * @param[out] cberr       Error message (if retval=0)
 * @retval     1           OK
 * @retval     0           Failed (No such device tree)
 * @retval    -1           Error
 */
int
device_config_read_cache(clixon_handle h,
                         char         *devname,
                         char         *config_type,
                         cxobj       **xdatap,
                         cbuf        **cberr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    char  *db;
    cxobj *xt = NULL;
    cxobj *xroot;

    if (devname == NULL || config_type == NULL){
        clixon_err(OE_UNIX, EINVAL, "devname or config_type is NULL");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "device-%s-%s", devname, config_type);
    db = cbuf_get(cb);
    if (xmldb_get_cache(h, db, &xt, NULL) < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/config")) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(*cberr, "No such device tree");
        goto failed;
    }
    if (xdatap){
        *xdatap = xroot;
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

/*! Get local (cached) device datastore
 *
 * @param[in]  h        Clixon handle
 * @param[in]  devname  Name of device
 * @param[in]  from     from config-type
 * @param[in]  to       to config-type
 * @retval     0        OK
 * @retval    -1        Error
 */
int
device_config_copy(clixon_handle h,
                   char         *devname,
                   char         *from,
                   char         *to)
{
    int    retval = -1;
    cbuf  *db0 = NULL;
    cbuf  *db1 = NULL;

    if (devname == NULL || from == NULL || to == NULL){
        clixon_err(OE_UNIX, EINVAL, "devname, from or to is NULL");
        goto done;
    }
    if ((db0 = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if ((db1 = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(db0, "device-%s-%s", devname, from);
    cprintf(db1, "device-%s-%s", devname, to);
    if (xmldb_copy(h, cbuf_get(db0), cbuf_get(db1)) < 0)
        goto done;
    retval = 0;
 done:
    if (db0)
        cbuf_free(db0);
    if (db1)
        cbuf_free(db1);
    return retval;
}

/*! Compare transient and last synced
 *
 * @param[in]  h      Clixon handle.
 * @param[in]  dh     Device handle.
 * @param[in]  name   Device name
 * @param[out] cberr0 Reason why inequal (retval = 1)
 * @retval     2      OK and equal
 * @retval     1      Not equal, cberr0 set
 * @retval     0      Closed
 * @retval    -1      Error
 */
static int
device_config_compare(clixon_handle           h,
                      device_handle           dh,
                      char                   *name,
                      controller_transaction *ct,
                      cbuf                  **cberr0)
{
    int     retval = -1;
    cxobj  *x0 = NULL;
    cxobj  *x1 = NULL;
    cbuf   *cberr = NULL;
    int     eq;
    int     ret;

    if ((ret = device_config_read_cache(h, name, "SYNCED", &x0, &cberr)) < 0)
        goto done;
    if (ret && (ret = device_config_read_cache(h, name, "TRANSIENT", &x1, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (device_close_connection(dh, "%s", cbuf_get(cberr)) < 0)
            goto done;
        goto closed;
    }
    /* 0: Equal, 1: not equal */
    if ((eq = xml_tree_equal(x0, x1)) != 0 && cberr0){
        if ((*cberr0 = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(*cberr0, "Device %s has changed config. See: diff device-%s-SYNCED_db device-%s-TRANSIENT_db",
                name, name, name);
         retval = 1;
    }
    else
        retval = 2;
 done:
    if (cberr)
        cbuf_free(cberr);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Check if there is another equivalent xyanglib and if so reuse that yspec
 *
 * Prereq: schema-list (xyanglib) is completely known.
 * Look for an existing equivalent schema-list among other devices.
 * If found, re-use that YANG-SPEC.
 * @param[in]  h         Clixon handle
 * @param[in]  dh        Clixon device handle.
 * @param[in]  xylib1    Yang-lib in XML format
 * @param[out] yspec1    New or shared yang-spec
 * @retval     0         OK
 * @retval    -1         Error
 * @see yang_schema_find_share  Similar
 */
static int
device_shared_yspec(clixon_handle h,
                    device_handle dh0,
                    cxobj        *xylib1,
                    const char   *digest1,
                    yang_stmt   **yspec1)
{
    int           retval = -1;
    yang_stmt    *yspec = NULL;
    device_handle dh1;
    cxobj        *xylib2;
    char         *domain0;
    char         *digest2 = NULL;

    if (clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT_SHARE")) {
        domain0 = device_handle_domain_get(dh0);
        /* New yspec only on first connect */
        dh1 = NULL;
        while ((dh1 = device_handle_each(h, dh1)) != NULL){
            if (dh1 == dh0)
                continue;
            if ((xylib2 = device_handle_yang_lib_get(dh1)) == NULL)
                continue;
            if (strcmp(domain0, device_handle_domain_get(dh1)) != 0)
                continue;
            if (digest2){
                free(digest2);
                digest2 = NULL;
            }
            if (xyanglib_digest(xylib2, &digest2) < 0)
                goto done;
            if (clicon_strcmp(digest1, digest2) == 0)
                break;
        }
        if (dh1 != NULL){
            if (controller_mount_yspec_get(h, device_handle_name_get(dh1), &yspec) < 0)
                goto done;
        }
    }
    if (yspec1)
        *yspec1 = yspec;
    retval = 0;
 done:
    if (digest2)
        free(digest2);
    return retval;
}

/*! Helper device_state_handler: check if transaction has ended, if so send [discard;]lock
 *
 * @param[in]  h       Clixon handle
 * @param[in]  dh      Device handle.
 * @param[in]  ct      Controller transaction
 * @param[in]  discard 0: Send only unlock; 1: Send discard; unlock
 * @retval     1       Transaction PK, continue
 * @retval     0       Transaction has failed, break
 * @retval    -1       Error
 */
static int
device_state_check_fail(clixon_handle           h,
                        device_handle           dh,
                        controller_transaction *ct,
                        int                     discard)
{
    int retval = -1;

    if (ct->ct_state == TS_RESOLVED) {
        if (ct->ct_result == TR_SUCCESS){
            clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
            goto done;
        }
        else if (discard){        /* Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
        }
        else {                    /* Trigger UNLOCK of the device */
            if (device_send_lock(h, dh, 0) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_UNLOCK) < 0)
                goto done;
        }
        retval = 0;
    }
    else
        retval = 1;
 done:
    return retval;
}

/*! Helper device_state_handler: check if state of remaining devices in transaction
 *
 * After device itself is OK check, set to OPEN,
 * If no other devices are left in the transaction, then as last device resolve transaction:
 * - To failed if already resolved
 * - To success if not failures yet
 * @param[in]  h     Clixon handle
 * @param[in]  dh    Device handle.
 * @param[in]  ct    Controller transaction
 * @retval     1     OK
 * @retval     0     Failed
 * @retval    -1     Error
 */
static int
device_state_check_ok(clixon_handle           h,
                      device_handle           dh,
                      controller_transaction *ct)
{
    int      retval = -1;
    uint64_t tid;

    tid = ct->ct_id;
    if (ct->ct_state == TS_RESOLVED && ct->ct_result == TR_SUCCESS){
        clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
        goto done;
    }
    if (device_state_set(dh, CS_OPEN) < 0)
        goto done;
    /* 2.2.2.1 Leave transaction */
    device_handle_tid_set(dh, 0);
    /* 2.2.2.2 If no devices in transaction, mark as OK and close it*/
    if (controller_transaction_nr_devices(h, tid) == 0){
        if (ct->ct_state != TS_RESOLVED){
            controller_transaction_state_set(ct, TS_RESOLVED, TR_SUCCESS);
        /* Garbage-collect yspecs with no mount-points */
            if (1) /* Causes SEGV when reconnect */
                if (yang_mount_cleanup(h) < 0)
                    goto done;
        }
        if (controller_transaction_done(h, ct, -1) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Helper device_state_handler: check sanity of message and transaction parameters
 *
 * @param[in]  dh          Device handle.
 * @param[in]  tid         Transaction id
 * @param[in]  ct          Controller transaction
 * @param[in]  name        Device name
 * @param[in]  conn_state  Device connection state
 * @param[in]  rpc_name    RPC name of message
 * @retval     1           OK
 * @retval     0           Sanity fail
 */
static int
device_state_check_sanity(device_handle           dh,
                          uint64_t                tid,
                          controller_transaction *ct,
                          char                   *name,
                          conn_state              conn_state,
                          char                   *rpcname)
{
    if (tid == 0){
        device_close_connection(dh, "Unexpected rpc %s from device %s in state %s: device is not part of any transaction (device may have left earlier)",
                                rpcname, name, device_state_int2str(conn_state));
        return 0;
    }
    if (ct == NULL){
        device_close_connection(dh, "Unexpected rpc %s from device %s in state %s: Device part of transaction with unknown id: %" PRIu64,
                                rpcname, name, device_state_int2str(conn_state), tid);
        return 0;
    }

    if (ct->ct_state != TS_INIT && ct->ct_state != TS_RESOLVED){
        clixon_debug(CLIXON_DBG_CTRL, "%s: Unexpected msg %s in state %s",
                     name, rpcname, transaction_state_int2str(ct->ct_state));
        return 0;
    }
    return 1;
}

/*! Commit db to running after pull transaction done
 *
 * @param[in] h     Clixon handle
 * @param[in] dh    Clixon device handle.
 * @param[in] ct    Transaction
 * @param[in] db0   Database name
 * @retval    0     OK
 * @retval   -1     Error
 * @note some complexities: db is a special candidate (eg tmpdev) with some potential changes of device config
 *       One cannot use (or overwrite) candidate itself since it may have changes you dont want overwritten that
 *       are in the parts that were NOT pulled, ie other devices or top-level.
 *       Also, using privcand, if a connection has been opened, pre-open device config can be stale so the
 *       orig cand is deleted. Maybe this should be done only under certain circumstances, and maybe it should be
 *       copied from candidate instead?
 */
static int
commit_after_pull(clixon_handle           h,
                  device_handle           dh,
                  controller_transaction *ct,
                  const char             *db)
{
    int       retval = -1;
    cbuf     *cbret = NULL;
    char     *db1 = NULL;
    uint32_t  ceid;
    int       ret;

    ceid = ct->ct_client_id;
    if ((cbret = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (clicon_option_bool(h, "CLICON_XMLDB_PRIVATE_CANDIDATE")){
        if (xmldb_candidate_find(h, "candidate-orig", ceid, NULL, &db1) < 0)
            goto done;
        if (db1)
            xmldb_delete(h, db1); /* Delete orig since its content may have been overwritten in device_recv_config */
    }
    if ((ret = candidate_commit(h, NULL, db, 0, 0, cbret)) < 0){
        /* Handle that candidate_commit can return < 0 if transaction ongoing */
        cprintf(cbret, "Failed to commit: %s", clixon_err_reason());
        ret = 0;
    }
    if (clicon_option_bool(h, "CLICON_AUTOLOCK"))
        xmldb_unlock(h, db);
    if (ret == 0){ /* discard */
        clixon_debug(CLIXON_DBG_CTRL, "%s", cbuf_get(cbret));
        if (device_close_connection(dh, "%s", cbuf_get(cbret)) < 0)
            goto done;
        if (controller_transaction_failed(h, ct->ct_id, ct, dh, TR_FAILED_DEV_LEAVE,
                                          device_handle_name_get(dh),
                                          device_handle_logmsg_get(dh)) < 0)
            goto done;
        goto failed;
    }
    retval = 1;
 done:
    if (cbret)
        cbuf_free(cbret);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Map device capabilities to local settings
 *
 * Based on dcapabilities announced by the device, and the local configuration,
 * set local settings.
 * From: RFC 6241:
 * If more than one protocol version URI in common is present,
 * then the highest numbered (most recent) protocol version MUST be used
 * by both peers.
 * From RFC 6242:
 * If the :base:1.1 capability is advertised by both
 * peers, the chunked framing mechanism (see Section 4.2) is used for
 * the remainder of the NETCONF session.  Otherwise, the old end-of-
 * message-based mechanism (see Section 4.3) is used.
 * @param[in] h     Clixon handle
 * @param[in] dh    Clixon device handle.
 * @retval    1     OK
 * @retval    0     Fail: close connection
 * @retval   -1     Error
 *
 * Truth values:
 client 10 | client 11 | config 10 | config 11 | Result
-----------+-----------+-----------+-----------+-----------+
 0         | 0         | -         | -         | Error
 0         | 0         | -         | -         | Error
 0         | 0         | -         | -         | Error
 0         | 1         | 0         | 0         | 11
 0         | 1         | 0         | 1         | 11
 0         | 1         | 1         | 0         | Error
 1         | 0         | 0         | 0         | 10
 1         | 0         | 0         | 1         | Error
 1         | 0         | 1         | 0         | 10
 1         | 1         | 0         | 0         | 11
 1         | 1         | 0         | 1         | 11
 1         | 1         | 1         | 0         | 10

 */
static int
device_capabilities2settings(clixon_handle h,
                             device_handle dh)
{
    int                  retval = -1;
    cxobj               *xcaps;
    int                  client10;
    int                  client11;
    int                  config10;
    int                  config11;
    netconf_framing_type framing;

    if ((xcaps = device_handle_capabilities_get(dh)) == NULL){
        clixon_err(OE_XML, 0, "No capabilities");
        goto done;
    }
    client10 = device_handle_capabilities_find(dh, NETCONF_BASE_CAPABILITY_1_0);
    client11 = device_handle_capabilities_find(dh, NETCONF_BASE_CAPABILITY_1_1);
    if (client10 == 0 && client11 == 0){
        device_close_connection(dh, "Neither NETCONF base 10 or 11 capability found in hello protocol from device %s", device_handle_name_get(dh));
        goto fail;
    }
    config10 = device_handle_flag_get(dh, DH_FLAG_NETCONF_BASE10);
    config11 = device_handle_flag_get(dh, DH_FLAG_NETCONF_BASE11);
    if (config10 == 1 && config11 == 1){
        clixon_err(OE_XML, 0, "Invalid config: Both base10 and base11 are configured to 1");
        goto done;
    }
    if (client10 == 0 && client11 == 1){
        if (config10){
            device_close_connection(dh, "NETCONF framing is configured to 10 but device announces only base 11");
            goto fail;
        }
        framing = NETCONF_SSH_CHUNKED;
    }
    else if (client10 == 1 && client11 == 0){
        if (config11){
            device_close_connection(dh, "NETCONF framing is configured to 11 but device announces only base 10");
            goto fail;
        }
        framing = NETCONF_SSH_EOM;
    }
    else{ /* both 1 */
        if (config10)
            framing = NETCONF_SSH_EOM;
        else
            framing = NETCONF_SSH_CHUNKED;
    }
    clixon_debug(CLIXON_DBG_CTRL, "netconf framing: %s", netconf_framing_int2str(framing));
    //    framing = 0; //NETCONF_SSH_EOM; // XXX
    device_handle_framing_type_set(dh, framing);

    /* Private candidate, only if both configured and frm device is set */
    if (device_handle_flag_get(dh, DH_FLAG_PRIVATE_CANDIDATE) &&
        !device_handle_capabilities_find(dh, NETCONF_PRIVATE_CANDIDATE_CAPABILITY)){
        device_handle_flag_reset(dh, DH_FLAG_PRIVATE_CANDIDATE);
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Main state machine for controller transactions+devices
 *
 * @param[in]  h     Clixon handle
 * @param[in]  dh    Device handle.
 * @param[in]  s     Socket
 * @param[in]  xmsg  XML tree of incoming message, on the form: <rpc-reply>...
 * @retval     0     OK
 * @retval    -1     Error
 */
int
device_state_handler(clixon_handle h,
                     device_handle dh,
                     int           s,
                     cxobj        *xmsg)
{
    int         retval = -1;
    char       *rpcname;
    char       *name;
    conn_state  conn_state;
    yang_stmt  *yspec0;
    int         new = 0;
    yang_stmt  *yspec1 = NULL;
    int         nr;
    uint64_t    tid;
    controller_transaction *ct = NULL;
    cbuf       *cberr = NULL;
    cbuf       *cbmsg;
    cxobj      *xyanglib;
    char       *candidate = NULL;
    db_elmnt   *de = NULL;
    char       *digest = NULL;
    char       *domain;
    cbuf       *cbxpath = NULL;
    int         ret;

    rpcname = xml_name(xmsg);
    conn_state = device_handle_conn_state_get(dh);
    name = device_handle_name_get(dh);
    clixon_debug(CLIXON_DBG_CTRL|CLIXON_DBG_DETAIL, "rpc:%s dev:%s", rpcname, name);
    yspec0 = clicon_dbspec_yang(h);
    if ((tid = device_handle_tid_get(dh)) != 0)
        ct = controller_transaction_find(h, tid);
    switch (conn_state){
        /* Here starts states of OPEN transaction */
    case CS_CONNECTING:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive hello from device */
        if ((ret = device_recv_hello(h, dh, s, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* Map device capabilities to local settings */
        if (device_capabilities2settings(h, dh) < 0)
            goto done;
        /* Send hello to device*/
        if (clixon_client_hello(s, device_handle_name_get(dh),
                                device_handle_framing_type_get(dh) == NETCONF_SSH_EOM,
                                device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED,
                                device_handle_flag_get(dh, DH_FLAG_PRIVATE_CANDIDATE)) < 0)
            goto done;
        /* The device is OK */
        if (ct->ct_state == TS_RESOLVED && ct->ct_result == TR_SUCCESS){
            clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
            goto done;
        }
        /* Reset YANGs */
        if ((xyanglib = device_handle_yang_lib_get(dh)) != NULL){
            /* If local schemas, check if they exist as local file */
            if ((ret = device_schemas_local_check(h, dh, xyanglib)) < 0)
                goto done;
            if (ret == 0){
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)

                    goto done;
                break;
            }
        }
        if (!device_handle_capabilities_find(dh, NETCONF_MONITORING_NAMESPACE)){
            clixon_debug(CLIXON_DBG_CTRL, "Device %s: Netconf monitoring capability %s not announced in hello protocol",
                         name,
                         NETCONF_MONITORING_NAMESPACE);
            if (xyanglib == NULL){
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name,
                                                  "Netconf monitoring capability not announced in hello protocol and no local models found") < 0)
                    goto done;
                break;
            }
            if (xpath_first(xyanglib, 0, "module-set/module") == NULL){
                /* see controller_connect/xdev2yang_library */
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, "No monitor caps or local YANGs") < 0)
                    goto done;
                break;
            }
            /* Check if there is another equivalent xyanglib */
            new = 0;
            yspec1 = NULL;
            domain = device_handle_domain_get(dh);
            if (xyanglib_digest(xyanglib, &digest) < 0)
                goto done;
            if (yang_mount_get_xpath(h, domain, digest, &yspec1, NULL) < 0)
                goto done;
            if (yspec1 != NULL){
                if (controller_mount_xpath_get(name, &cbxpath) < 0)
                    goto done;
            }
            else { /* It may already exist? */
                yang_stmt *ymounts;
                yang_stmt *ydomain;

                if (controller_mount_xpath_get(name, &cbxpath) < 0)
                    goto done;
                if ((ymounts = clixon_yang_mounts_get(h)) == NULL){
                    clixon_err(OE_YANG, ENOENT, "Top-level yang mounts not found");
                    goto done;
                }
                if ((ydomain = yang_find(ymounts, Y_DOMAIN, domain)) == NULL){
                    if ((ydomain = ydomain_new(h, domain)) == NULL)
                        goto done;
                }
                if (device_shared_yspec(h, dh, xyanglib, digest, &yspec1) < 0)
                    goto done;
                if (yspec1 == NULL){
                    if ((yspec1 = yspec_new1(h, domain, digest)) == NULL)
                        goto done;
                    new++;
                    yang_flag_set(yspec1, YANG_FLAG_SPEC_MOUNT);
                }
                if (yang_cvec_add(yspec1, CGV_STRING, cbuf_get(cbxpath)) == NULL)
                    goto done;
                if (controller_mount_yspec_set(h, name, yspec1) < 0)
                    goto done;
            }
            /* All schemas ready, parse them (may do device_close) */
            if (new){
                if ((ret = device_schemas_mount_parse(h, dh, xyanglib)) < 0)
                    goto done;
                if (ret == 0){
                    if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                        goto done;
                    break;
                }
            }
            /* Send a <get-config> request to a device */
            if (device_send_get(h, dh, s, 0, NULL) < 0)
                goto done;
            if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
                goto done;
            break;
        }
        else
            clixon_debug(CLIXON_DBG_CTRL, "Device %s: Netconf monitoring capability announced", name);
        if ((ret = device_send_get_schema_list(h, dh, s)) < 0)
            goto done;
        if (device_state_set(dh, CS_SCHEMA_LIST) < 0)
            goto done;
        break;
    case CS_SCHEMA_LIST:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive netconf-state schema list from device, append schemas to device xyanglib */
        if ((ret = device_recv_schema_list(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (ct->ct_state == TS_RESOLVED && ct->ct_result == TR_SUCCESS){
            clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
            goto done;
        }
        if ((xyanglib = device_handle_yang_lib_get(dh)) == NULL){
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, "No YANG device lib 1") < 0)
                goto done;
            break;
        }
        /* Check if there is another equivalent xyanglib
         */
        new = 0;
        yspec1 = NULL;
        domain =  device_handle_domain_get(dh);
        if (xyanglib_digest(xyanglib, &digest) < 0)
            goto done;
        if (yang_mount_get_xpath(h, domain, digest, &yspec1, NULL) < 0)
            goto done;
        if (yspec1 != NULL){
            if (controller_mount_xpath_get(name, &cbxpath) < 0)
                goto done;
        }
        else {
            yang_stmt *ymounts;
            yang_stmt *ydomain;

            if (controller_mount_xpath_get(name, &cbxpath) < 0)
                goto done;
            if ((ymounts = clixon_yang_mounts_get(h)) == NULL){
                clixon_err(OE_YANG, ENOENT, "Top-level yang mounts not found");
                goto done;
            }
            if ((ydomain = yang_find(ymounts, Y_DOMAIN, domain)) == NULL){
                if ((ydomain = ydomain_new(h, domain)) == NULL)
                    goto done;
            }
            if (device_shared_yspec(h, dh, xyanglib, digest, &yspec1) < 0)
                goto done;
            if (yspec1 == NULL){
                if ((yspec1 = yspec_new1(h, domain, digest)) == NULL)
                    goto done;
                yang_flag_set(yspec1, YANG_FLAG_SPEC_MOUNT);
                new++;
            }
        }
        if (yang_cvec_add(yspec1, CGV_STRING, cbuf_get(cbxpath)) == NULL)
            goto done;
        if (controller_mount_yspec_set(h, name, yspec1) < 0)
            goto done;
        nr = 0;
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 1){ /* Device closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        else if (ret == 0){ /* None found */
            if (new){
                /* All schemas ready, parse them */
                if ((ret = device_schemas_mount_parse(h, dh, xyanglib)) < 0)
                    goto done;
                if (ret == 0){
                    if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                        goto done;
                    break;
                }
            }
            /* Unconditionally sync */
            if (device_send_get(h, dh, s, 0, NULL) < 0)
                goto done;
            if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
                goto done;
            break;
        }
        device_handle_nr_schemas_set(dh, nr);
        if (device_state_set(dh, CS_SCHEMA_ONE) < 0)
            goto done;
        break;
    case CS_SCHEMA_ONE:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive get-schema and write to local yang file */
        if ((ret = device_recv_get_schema(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (ct->ct_state == TS_RESOLVED && ct->ct_result == TR_SUCCESS){
            clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
            goto done;
        }
        /* Check if all schemas are received */
        nr = device_handle_nr_schemas_get(dh);
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 1){ /* Device closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        if (ret == 0){ /* None sent */
            /* All schemas ready, parse them */
            if ((xyanglib = device_handle_yang_lib_get(dh)) == NULL){
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, "No YANG device lib 2") < 0)
                    goto done;
                break;
            }
            if ((ret = device_schemas_mount_parse(h, dh, xyanglib)) < 0)
                goto done;
            if (ret == 0){
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                    goto done;
                break;
            }
            /* Unconditionally sync */
            if (device_send_get(h, dh, s, 0, NULL) < 0)
                goto done;
            if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
                goto done;
            break;
        }
        device_handle_nr_schemas_set(dh, nr);
        device_state_timeout_restart(dh);
        clixon_debug(CLIXON_DBG_CTRL, "%s: %s(%d) -> %s(%d)",
                     name,
                     device_state_int2str(conn_state), nr-1,
                     device_state_int2str(conn_state), nr);
        break;
    case CS_DEVICE_SYNC:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive config data from device and add config to mount-point */
        if ((ret = device_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state, 0, 0)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        if (controller_transaction_nr_devices(h, tid) == 1 &&
            !ct->ct_pull_transient) {
            /* See puts from each device in device_recv_config() */
            if ((ret = commit_after_pull(h, dh, ct, "tmpdev")) < 0)
                goto done;
            if (ret == 0)
                break;
            xmldb_delete(h, "tmpdev");
        }
        /* The device is OK */
        if (device_state_check_ok(h, dh, ct) < 0)
            goto done;
        break;
    case CS_PUSH_LOCK:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, cbuf_get(cberr)) < 0)
                goto done;
            if (device_state_set(dh, CS_OPEN) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /*
                              1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_IGNORE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 0)) < 0)
            goto done;
        if (ret == 0)
            break;
        if (device_send_get(h, dh, s, 0, NULL) < 0)
            goto done;
        device_handle_tid_set(dh, ct->ct_id);
        if (device_state_set(dh, CS_PUSH_CHECK) < 0)
            goto done;
        break;
        /* Here starts states of PUSH transaction */
    case CS_PUSH_CHECK:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive config data, force transient, ie do not commit */
        if ((ret = device_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state, 1, 0)) < 0)
            goto done;
        /* Compare transient with last sync 0: closed, 1: unequal, 2: is equal */
        if (ret && (ret = device_config_compare(h, dh, name, ct, &cberr)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* unequal */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_IGNORE, name, cbuf_get(cberr)) < 0)
                goto done;
            if (device_send_lock(h, dh, 0) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_UNLOCK) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 1)) < 0)
            goto done;
        if (ret == 0)
            break;
        /* 2.2 The transaction is OK
           Proceed to next step: get saved edit-msg and send it */
        if ((cbmsg = device_handle_outmsg_get(dh, 1)) == NULL){
            if ((cbmsg = device_handle_outmsg_get(dh, 2)) == NULL){
                device_close_connection(dh, "Device %s no edit-msg in state %s",
                                        name, device_state_int2str(conn_state));
                if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                    goto done;
                break;
            }
            if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
                if (clixon_msg_send11(s, device_handle_name_get(dh), cbmsg) < 0)
                    goto done;
            }
            else if (clixon_msg_send10(s, device_handle_name_get(dh), cbmsg) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_EDIT2) < 0)
                goto done;
            break;
        }
        if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
            if (clixon_msg_send11(s, device_handle_name_get(dh), cbmsg) < 0)
                goto done;
        }
        else if (clixon_msg_send10(s, device_handle_name_get(dh), cbmsg) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_EDIT) < 0)
            goto done;
        break;
    case CS_PUSH_EDIT:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_IGNORE, name, cbuf_get(cberr)) < 0)
                goto done;
            /* 1.1 The error is "recoverable" (eg validate fail) */
            /* --> 1.1.1 Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 1)) < 0)
            goto done;
        if (ret == 0)
            break;
        /* 2.2 The transaction is OK
           Proceed to next step */
        if ((cbmsg = device_handle_outmsg_get(dh, 2)) == NULL){
            if ((ret = device_send_validate(h, dh)) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_VALIDATE) < 0)
                goto done;
            break;
        }
        if (device_handle_framing_type_get(dh) == NETCONF_SSH_CHUNKED){
            if (clixon_msg_send11(s, device_handle_name_get(dh), cbmsg) < 0)
                goto done;
        }
        else if (clixon_msg_send10(s, device_handle_name_get(dh), cbmsg) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_EDIT2) < 0)
            goto done;
        break;
    case CS_PUSH_EDIT2:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_IGNORE, name, cbuf_get(cberr)) < 0)
                goto done;
            /* 1.1 The error is "recoverable" (eg validate fail) */
            /* --> 1.1.1 Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 1)) < 0)
            goto done;
        if (ret == 0)
            break;
        /* 2.2 The transaction is OK
           Proceed to next step */
        if ((ret = device_send_validate(h, dh)) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_VALIDATE) < 0)
            goto done;
        break;
    case CS_PUSH_VALIDATE:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_IGNORE, name, cbuf_get(cberr)) < 0)
                goto done;
            /* 1.1 The error is "recoverable" (eg validate fail) */
            /* --> 1.1.1 Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        if (ct->ct_push_type == PT_VALIDATE){
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 1)) < 0)
            goto done;
        if (ret == 0)
            break;
        if (device_state_set(dh, CS_PUSH_WAIT) < 0)
            goto done;

        /* 2.2 The transaction is OK */
        /* 2.2.1 Check if all devices are in WAIT (none are in EDIT/VALIDATE) */
        if ((ret = controller_transaction_wait(h, tid)) < 0)
            goto done;
        if (ret == 1){
            /* Check if action, skip if dryrun */
            if (ct->ct_actions_type != AT_NONE && strcmp(ct->ct_sourcedb, "candidate")==0){
                if ((cberr = cbuf_new()) == NULL){
                    clixon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                /* What to copy to candidate and commit to running? */
                if (xmldb_candidate_find(h, "candidate", ct->ct_client_id, &de, &candidate) < 0)
                    goto done;
                if (candidate == NULL){
                    if (controller_transaction_failed(h, ct->ct_id, ct, dh, TR_FAILED_DEV_LEAVE, name, "Candidate does not exist (privcand exit?)") < 0)
                        goto done;
                    break;
                }
                if (xmldb_copy(h, "actions", candidate) < 0)
                    goto done;
                /* Third validate,
                 * first in rpc_controller_commit,
                 * second in rpc_transactions_actions_done
                 * candidate should NOT have changed here
                 */
                if (clicon_option_bool(h, "CLICON_XMLDB_PRIVATE_CANDIDATE")){
                    /* Rebase private candidate with running */
                    if ((ret = backend_update(h, ct->ct_client_id, de, cberr)) < 0)
                        goto done;
                    if (ret == 0){
                        if (controller_transaction_failed(h, ct->ct_id, ct, dh, TR_FAILED_DEV_IGNORE, name, cbuf_get(cberr)) < 0)
                            goto done;
                        /* 1.1 The error is "recoverable" (eg validate fail) */
                        /* --> 1.1.1 Trigger DISCARD of the device */
                        if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                            goto done;
                        break;
                    }
                }
                if ((ret = candidate_commit(h, NULL, candidate, 0, 0, cberr)) < 0){
                    /* Handle that candidate_commit can return < 0 if transaction ongoing */
                    cprintf(cberr, "Controller commit error");
                    if (strlen(clixon_err_reason()) > 0)
                        cprintf(cberr, " %s", clixon_err_reason());
                    if (controller_transaction_failed(h, ct->ct_id, ct, dh, TR_FAILED_DEV_LEAVE, name, cbuf_get(cberr)) < 0)
                        goto done;
                    break;
                }
                if (clicon_option_bool(h, "CLICON_AUTOLOCK"))
                    xmldb_unlock(h, candidate);
                if (ret == 0){
                    cbuf  *cberr2 = NULL;
                    if ((cberr2 = cbuf_new()) == NULL){
                        clixon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    cprintf(cberr2, "Validation: ");
                    if (netconf_cbuf_err2cb(h, cberr, cberr2) < 0)
                        goto done;
                    if (controller_transaction_failed(h, ct->ct_id, ct, dh, TR_FAILED_DEV_IGNORE, "controller", cbuf_get(cberr2)) < 0)
                        goto done;
                    if (cberr2)
                        cbuf_free(cberr2);
                    if (controller_transaction_wait_trigger(h, tid, 0) < 0)
                        goto done;
                    break;
                }
            }
            if (controller_transaction_wait_trigger(h, tid, 1) < 0)
                goto done;
        } /* All devices are in WAIT state */
        break;
    case CS_PUSH_COMMIT:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, cbuf_get(cberr)) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if ((ret = device_state_check_fail(h, dh, ct, 1)) < 0)
            goto done;
        if (ret == 0)
            break;
#ifdef CONTROLLER_EXTRA_PUSH_SYNC
        /* Pull for commited db in the case the device changes it post-commit */
        if (device_send_get(h, dh, s, 0, NULL) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_COMMIT_SYNC) < 0)
            goto done;
#else
        if (device_send_lock(h, dh, 0) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_UNLOCK) < 0)
            goto done;
        if (conn_state == CS_PUSH_COMMIT){
            cxobj *xt = NULL;
            cbuf  *cb = NULL;

            /* Copy transient to device config (last sync)
               XXXX in commit push
            */
            if ((cb = cbuf_new()) == NULL){
                clixon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            if ((cberr = cbuf_new()) == NULL){
                clixon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "devices/device[name='%s']/config", name);
            /* xt is used to put which requires a copy */
            if (ct->ct_actions_type == AT_NONE){
                if (xmldb_get0(h, ct->ct_sourcedb, YB_MODULE, NULL, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
                    goto done;
            }
            else{
                if (xmldb_get0(h, "actions", YB_MODULE, NULL, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
                    goto done;
            }
            if (xt != NULL){
                if ((ret = device_config_write(h, name, "SYNCED", xt, cberr)) < 0)
                    goto done;
                if (ret == 0){
                    clixon_err(OE_XML, 0, "%s", cbuf_get(cberr));
                    goto done;
                }
            }
            if (cb)
                cbuf_free(cb);
            if (xt)
                xml_free(xt);
        }
#endif /* CONTROLLER_EXTRA_PUSH_SYNC*/
        break;
    case CS_PUSH_DISCARD:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, cbuf_get(cberr)) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (ct->ct_state == TS_RESOLVED) {
            if (ct->ct_result == TR_SUCCESS){
                clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
                goto done;
            }
            else ;  /* XXX What to do if transaction failed on discard? */
        }
        if (ct->ct_state == TS_RESOLVED && ct->ct_result == TR_SUCCESS){
            clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
            goto done;
        }
        if (device_send_lock(h, dh, 0) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_UNLOCK) < 0)
            goto done;
        break;
#ifdef CONTROLLER_EXTRA_PUSH_SYNC
    case CS_PUSH_COMMIT_SYNC:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive config data from device and add config to mount-point */
        if ((ret = device_recv_get(h, dh, xmsg, yspec0, rpcname, conn_state, 0, 1, 0)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (ct->ct_state == TS_RESOLVED) {
            if (ct->ct_result == TR_SUCCESS){
                clixon_err(OE_XML, 0, "Transaction unexpected SUCCESS state");
                goto done;
            }
            else ;  /* XXX What to do if transaction failed on sync? */
        }
        if (device_send_lock(h, dh, 0) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_UNLOCK) < 0)
            goto done;
#endif /* CONTROLLER_EXTRA_PUSH_SYNC */
    case CS_PUSH_UNLOCK:
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_ok(h, dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_CLOSE, name, cbuf_get(cberr)) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (device_state_check_ok(h, dh, ct) < 0)
            goto done;
        break;
    case CS_RPC_GENERIC: /* Accept rpc reply, store, and go to open */
        if (device_state_check_sanity(dh, tid, ct, name, conn_state, rpcname) == 0)
            break;
        /* Receive RPC data from device and store
           XXX: Retval: 2 OK, 1 Closed, 0 Failed, -1 Error */
        if ((ret = device_recv_generic_rpc(h, dh, ct, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, cbuf_get(cberr)) < 0)
                goto done;
            if (device_state_set(dh, CS_OPEN) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, NULL) < 0)
                goto done;
            break;
        }
        /* The device is OK */
        if (device_state_check_ok(h, dh, ct) < 0)
            goto done;
        break;
    case CS_PUSH_WAIT:
    case CS_CLOSED:
    case CS_OPEN:
    default:
        clixon_debug(CLIXON_DBG_CTRL, "%s: Unexpected msg %s in state %s",
                     name, rpcname,
                     device_state_int2str(conn_state));
        device_close_connection(dh, "Unexpected msg %s in state %s",
                                rpcname, device_state_int2str(conn_state));
        if (ct != NULL){
            if (controller_transaction_failed(h, tid, ct, dh, TR_FAILED_DEV_LEAVE, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
        }
        break;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL|CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (cbxpath)
        cbuf_free(cbxpath);
    if (digest)
        free(digest);
    if (cberr)
        cbuf_free(cberr);
    return retval;
}

/*! Get netconf device statedata
 *
 * The state returned includes per-device:
 * - connection state
 * - capabilities
 * - Last connection state-change time
 * - Last sync-time
 * - Log msg
 * @param[in]    h        Clixon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPath syntax. or NULL for all
 * @param[out]   xstate   XML tree, <config/> on entry.
 * @retval       0        OK
 * @retval      -1        Error
 */
int
devices_statedata(clixon_handle   h,
                  cvec           *nsc,
                  char           *xpath,
                  cxobj          *xstate)
{
    int            retval = -1;
    char          *name;
    device_handle  dh;
    cbuf          *cb = NULL;
    conn_state     state;
    char          *logmsg;
    struct timeval tv;
    cxobj         *xcaps;
    cxobj         *x;
    char          *xb;
    char           timestr[28];

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        name = device_handle_name_get(dh);
        cprintf(cb, "<devices xmlns=\"%s\"><device><name>%s</name>",
                CONTROLLER_NAMESPACE,
                name);
        state = device_handle_conn_state_get(dh);
        cprintf(cb, "<conn-state>%s</conn-state>", device_state_int2str(state));
        if ((xcaps = device_handle_capabilities_get(dh)) != NULL){
            cprintf(cb, "<capabilities>");
            x = NULL;
            while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
                if ((xb = xml_body(x)) == NULL)
                    continue;
                cprintf(cb, "<capability>");
                xml_chardata_cbuf_append(cb, 0, xb);
                cprintf(cb, "</capability>");
            }
            cprintf(cb, "</capabilities>");
        }
        device_handle_conn_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            if (time2str(&tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<conn-state-timestamp>%s</conn-state-timestamp>", timestr);
        }
        device_handle_sync_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            if (time2str(&tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<sync-timestamp>%s</sync-timestamp>", timestr);
        }
        if ((logmsg = device_handle_logmsg_get(dh)) != NULL){
            cprintf(cb, "<logmsg>");
            xml_chardata_cbuf_append(cb, 0, logmsg);
            cprintf(cb, "</logmsg>");
        }
        if (device_handle_flag_get(dh, DH_FLAG_PRIVATE_CANDIDATE))
            cprintf(cb, "<private-candidate-state>true</private-candidate-state>");
        cprintf(cb, "<netconf-framing-type>%s</netconf-framing-type>",
                netconf_framing_int2str(device_handle_framing_type_get(dh)));
        cprintf(cb, "</device></devices>");
        if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xstate, NULL) < 0)
            goto done;
        cbuf_reset(cb);
    } /* devices */
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}
