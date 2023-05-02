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
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_device_recv.h"
#include "controller_transaction.h"

/*! Mapping between enum conn_state and yang connection-state
 * @see clixon-controller@2023-01-01.yang connection-state
 */
static const map_str2int csmap[] = {
    {"CLOSED",       CS_CLOSED},
    {"CONNECTING",   CS_CONNECTING},
    {"SCHEMA_LIST",  CS_SCHEMA_LIST},
    {"SCHEMA_ONE",   CS_SCHEMA_ONE}, /* substate is schema-nr */
    {"DEVICE-SYNC",  CS_DEVICE_SYNC},
    {"OPEN",         CS_OPEN},
    {"PUSH_CHECK",   CS_PUSH_CHECK},
    {"PUSH_EDIT",    CS_PUSH_EDIT},
    {"PUSH_VALIDATE",CS_PUSH_VALIDATE},
    {"PUSH_WAIT",    CS_PUSH_WAIT},
    {"PUSH_COMMIT",  CS_PUSH_COMMIT},
    {"PUSH_DISCARD", CS_PUSH_DISCARD},
    {NULL,           -1}
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
 * @param[in]  dh      Clixon client handle.
 * @param[in]  format  Format string for Log message or NULL
 * @retval     0       OK
 * @retval    -1       Error
 */
int
device_close_connection(device_handle dh,
                        const char   *format, ...)
{
    int            retval = -1;
    va_list        ap;
    size_t         len;
    char          *str = NULL;
    int            s;
    char          *name;

    name = device_handle_name_get(dh);
    clicon_debug(1, "%s %s", __FUNCTION__, name);
    s = device_handle_socket_get(dh);
    clixon_event_unreg_fd(s, device_input_cb); /* deregister events */
    if (device_handle_disconnect(dh) < 0) /* close socket, reap sub-processes */
        goto done;
    device_handle_yang_lib_set(dh, NULL);
    if (device_state_set(dh, CS_CLOSED) < 0)
        goto done;
    device_handle_outmsg_set(dh, NULL);
    if (format == NULL)
        device_handle_logmsg_set(dh, NULL);
    else {
        va_start(ap, format); /* dryrun */
        if ((len = vsnprintf(NULL, 0, format, ap)) < 0) /* dryrun, just get len */
            goto done;
        va_end(ap);
        if ((str = malloc(len+1)) == NULL){
            clicon_err(OE_UNIX, errno, "malloc");
            goto done;
        }
        va_start(ap, format); /* real */
        if (vsnprintf(str, len+1, format, ap) < 0){
            clicon_err(OE_UNIX, errno, "vsnprintf");
            goto done;
        }
        va_end(ap);
        device_handle_logmsg_set(dh, str);
        clicon_debug(1, "%s %s: %s", __FUNCTION__, name, str);
    }
    retval = 0;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    return retval;
}

/*! Handle input data from device, whole or part of a frame ,called by event loop
 * @param[in] s   Socket
 * @param[in] arg Device handle
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
    int                     ret;
    
    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
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
            clicon_debug(CLIXON_DBG_DETAIL, "%s: frame: %lu", __FUNCTION__, cbuf_len(cbmsg));
            /* Extra data to read, save data and continue on next round */
            break;
        }
        clicon_debug(CLIXON_DBG_MSG, "Recv dev %s: %s", name, cbuf_get(cbmsg));
        if ((ret = netconf_input_frame2(cbmsg, YB_NONE, NULL, &xtop, &xerr)) < 0)
            goto done;
        clicon_debug(1, "%s ret:%d", __FUNCTION__, ret);
        cbuf_reset(cbmsg);
        if (ret == 0){
            if ((cberr = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            if (netconf_err2cb(xerr, cberr) < 0)
                goto done;
            if (ct){
                // use XXX cberr but its XML
                if (controller_transaction_failed(h, tid, ct, dh, 2, name, "Invalid frame") < 0)
                    goto done;
            }
            else
                device_close_connection(dh, "Invalid frame");
            goto ok;
        }
        xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
        if (xmsg && device_state_handler(h, dh, s, xmsg) < 0)
            goto done;
    } /* while */
    device_handle_frame_state_set(dh, frame_state);
    device_handle_frame_size_set(dh, frame_size);
 ok:
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DETAIL, "%s retval:%d", __FUNCTION__, retval);
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
 */
int
device_state_mount_point_get(char      *devicename,
                             yang_stmt *yspec,
                             cxobj    **xtp,
                             cxobj    **xrootp)
{
    int    retval = -1;
    int    ret;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;
    cxobj *xroot;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
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
        clicon_err(OE_XML, 0, "device/config mountpoint not found");
        goto done;
    }
    *xtp = xt;
    *xrootp = xroot;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! All schemas ready from one device, parse the locally
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @retval    1          OK
 * @retval    0          YANG parse error
 * @retval   -1          Error
 */
static int
device_state_schemas_ready(clixon_handle h,
                           device_handle dh,
                           yang_stmt    *yspec0)
{
    int        retval = -1;
    yang_stmt *yspec1;
    cxobj     *yanglib;
    cxobj     *xt = NULL;
    cxobj     *xmount;
    char      *devname;
    int        ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((yspec1 = device_handle_yspec_get(dh)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    yanglib = device_handle_yang_lib_get(dh);
    /* Given yang-lib, parse all modules into yspec */
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    /* Added extra JUNOS patch to mod YANGs */
    if ((ret = yang_lib2yspec_junos_patch(h, yanglib, yspec1)) < 0)
        goto done;
#else
    if ((ret = yang_lib2yspec(h, yanglib, yspec1)) < 0)
        goto done;
#endif
    if (ret == 0)
        goto fail;
    devname = device_handle_name_get(dh);
    /* Create XML tree and device mount-point */
    if (device_state_mount_point_get(devname, yspec0, &xt, &xmount) < 0)
        goto done;
    if (xml_yang_mount_set(xmount, yspec1) < 0)
        goto done;    
    if (ret == 0)
        goto fail;
    yspec1 = NULL;
    retval = 1;
 done:
    clicon_debug(1, "%s retval %d", __FUNCTION__, retval);
    if (retval<0 && yspec1)
        ys_free(yspec1);
    if (xt)
        xml_free(xt);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Timeout callback of transient states, close connection
 * @param[in] arg    In effect client handle
 */
static int
device_state_timeout(int   s,
                     void *arg)
{
    int                     retval = -1;
    device_handle           dh = (device_handle)arg;
    uint64_t                tid;
    controller_transaction *ct = NULL;
    clicon_handle           h;
    char                   *name;
    
    name = device_handle_name_get(dh);
    clicon_debug(1, "%s %s", __FUNCTION__, name);
    h = device_handle_handle_get(dh);

    if ((tid = device_handle_tid_get(dh)) != 0)
        ct = controller_transaction_find(h, tid);
    if (ct){
        if (controller_transaction_failed(device_handle_handle_get(dh), tid, ct, dh, 2, name, "Timeout waiting for remote peer") < 0)
            goto done;
    }
    else if (device_close_connection(dh, "Timeout waiting for remote peer") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Set timeout of transient device state
 * @param[in] dh  Device handle
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
    char                   *name;
    
    name = device_handle_name_get(dh);
    clicon_debug(1, "%s %s", __FUNCTION__, name);
    gettimeofday(&t, NULL);
    h = device_handle_handle_get(dh);
    d = clicon_data_int_get(h, "controller-device-timeout");
    if (d != -1)
        t1.tv_sec = d;
    else
        t1.tv_sec = 60;
    t1.tv_usec = 0;
    clicon_debug(1, "%s timeout:%ld s", __FUNCTION__, t1.tv_sec);
    timeradd(&t, &t1, &t);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
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
 * @param[in] dh  Device handle
 */
int
device_state_timeout_unregister(device_handle dh)
{
    char *name;
    
    name = device_handle_name_get(dh);
    clicon_debug(1, "%s %s", __FUNCTION__, name);
    (void)clixon_event_unreg_timeout(device_state_timeout, dh);
    return 0;
}
    
/*! Restart timer (stop; start)
 * @param[in] dh  Device handle
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
 * @param[in]   dh   Device handle
 * @param[in]   state  State
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
 * @retval    1            OK
 * @retval    0            Fail (cbret set)
 * @retval   -1            Error
 */
int
device_config_write(clixon_handle h,
                    char         *devname,
                    char         *config_type,
                    cxobj        *xdata,
                    cbuf         *cbret)
{
    int    retval = -1;
    cbuf  *cbdb = NULL;
    char  *db;

    if (devname == NULL || config_type == NULL){
        clicon_err(OE_UNIX, EINVAL, "devname or config_type is NULL");
        goto done;
    }
    if ((cbdb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }   
    cprintf(cbdb, "device-%s-%s", devname, config_type);
    db = cbuf_get(cbdb);
    if (xmldb_db_reset(h, db) < 0)
        goto done;
    retval = xmldb_put(h, db, OP_REPLACE, xdata, clicon_username_get(h), cbret);
 done:
    if (cbdb)
        cbuf_free(cbdb);
    return retval;
}

/*! Get local (cached) device datastore
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
device_config_read(clicon_handle h,
                   char         *devname,
                   char         *config_type,
                   cxobj       **xdatap,
                   cbuf        **cberr)
{
    int    retval = -1;
    cbuf  *cbdb = NULL;
    char  *db;
    cvec  *nsc = NULL;
    cxobj *xt = NULL;
    cxobj *xroot;
        
    if (devname == NULL || config_type == NULL){
        clicon_err(OE_UNIX, EINVAL, "devname or config_type is NULL");
        goto done;
    }
    if ((cbdb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }   
    cprintf(cbdb, "device-%s-%s", devname, config_type);
    db = cbuf_get(cbdb);
    if (xmldb_get0(h, db, Y_MODULE, nsc, NULL, 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/config")) == NULL){
        if ((*cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
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
    if (cbdb)
        cbuf_free(cbdb);
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
device_config_copy(clicon_handle h,
                   char         *devname,
                   char         *from,
                   char         *to)
{
    int    retval = -1;
    cbuf  *db0 = NULL;
    cbuf  *db1 = NULL;
    
    if (devname == NULL || from == NULL || to == NULL){
        clicon_err(OE_UNIX, EINVAL, "devname, from or to is NULL");
        goto done;
    }
    if ((db0 = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if ((db1 = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
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
 * @param[out] eq     If equal==0
 * @retval     1      OK
 * @retval     0      Closed
 * @retval    -1      Error
 */
static int
device_config_compare(clicon_handle           h,
                      device_handle           dh,
                      char                   *name,
                      controller_transaction *ct,
                      int                    *eq)
{
    int    retval = -1;
    cxobj *x0 = NULL;
    cxobj *x1 = NULL;
    cbuf  *cberr = NULL;
    int    ret;
            
    if ((ret = device_config_read(h, name, "SYNCED", &x0, &cberr)) < 0)
        goto done;
    if (ret && (ret = device_config_read(h, name, "TRANSIENT", &x1, &cberr)) < 0)
        goto done;
    if (ret == 0){
        if (device_close_connection(dh, "%s", cbuf_get(cberr)) < 0)
            goto done;
        goto closed;
    }
    *eq = xml_tree_equal(x0, x1);
    retval = 1;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (x0)
        xml_free(x0);
    if (x1)
        xml_free(x1);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Handle controller device state machine
 *
 * @param[in]  h       Clixon handle
 * @param[in]  dh      Clixon client handle.
 * @param[in]  s       Socket
 * @param[in]  xmsg    XML tree of incoming message
 * @retval     0       OK
 * @retval    -1       Error
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
    yang_stmt  *yspec1;
    int         nr;
    int         ret;
    uint64_t    tid;
    controller_transaction *ct = NULL;
    cbuf       *cberr = NULL;
    cbuf       *cbmsg;
    int         eq = 0;

    rpcname = xml_name(xmsg);
    conn_state = device_handle_conn_state_get(dh);
    name = device_handle_name_get(dh);
    yspec0 = clicon_dbspec_yang(h);
    if ((tid = device_handle_tid_get(dh)) != 0)
        ct = controller_transaction_find(h, tid);
    switch (conn_state){
    case CS_CONNECTING:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        /* Receive hello from device, send hello */
        if ((ret = device_state_recv_hello(h, dh, s, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        /* Reset YANGs */
        if ((yspec1 = device_handle_yspec_get(dh)) == NULL){
            if ((yspec1 = yspec_new()) == NULL)
                goto done;
            device_handle_yspec_set(dh, yspec1);
        }
        if (!device_handle_capabilities_find(dh, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")){
            device_close_connection(dh, "No method to get schemas");
            break;
        }
        if ((ret = device_send_get_schema_list(h, dh, s)) < 0)
            goto done;
        if (device_state_set(dh, CS_SCHEMA_LIST) < 0)
            goto done;
        break;
    case CS_SCHEMA_LIST:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        /* Receive netconf-state schema list from device */
        if ((ret = device_state_recv_schema_list(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        nr = 0;
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 0){ /* None found */
            /* All schemas ready, parse them */
            if ((ret = device_state_schemas_ready(h, dh, yspec0)) < 0)
                goto done;
            if (ret == 0){
                device_close_connection(dh, "YANG parse error");
                break;
            }
            /* Unconditionally sync */
            if (device_send_get_config(h, dh, s) < 0)
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
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        /* Receive get-schema and write to local yang file */
        if ((ret = device_state_recv_get_schema(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        /* Check if all schemas are received */
        nr = device_handle_nr_schemas_get(dh);
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 0){ /* None sent, done */
            /* All schemas ready, parse them */
            if ((ret = device_state_schemas_ready(h, dh, yspec0)) < 0)
                goto done;
            if (ret == 0){
                device_close_connection(dh, "YANG parse error");
                break;
            }
            /* Unconditionally sync */
            if (device_send_get_config(h, dh, s) < 0)
                goto done;
            if (device_state_set(dh, CS_DEVICE_SYNC) < 0)
                goto done;
            break;
        }
        device_handle_nr_schemas_set(dh, nr);
        device_state_timeout_restart(dh);
        clicon_debug(1, "%s: %s(%d) -> %s(%d)",
                     name,
                     device_state_int2str(conn_state), nr-1,
                     device_state_int2str(conn_state), nr);
        break;
    case CS_DEVICE_SYNC:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        /* Receive config data from device and add config to mount-point */
        if ((ret = device_state_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        if (device_state_set(dh, CS_OPEN) < 0)
            goto done;
        /* 2.2.2.1 Leave transaction */
        device_handle_tid_set(dh, 0);
        /* 2.2.2.2 If no devices in transaction, mark as OK and close it*/
        if (controller_transaction_devices(h, tid) == 0){
            if (ct->ct_state != TS_RESOLVED){
                controller_transaction_state_set(ct, TS_RESOLVED, TR_SUCCESS);
                if (controller_transaction_notify(h, ct) < 0)
                    goto done;
            }
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
        }
        break;
    case CS_PUSH_CHECK:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        /* Receive config data, force transient, ie do not commit */
        ct->ct_pull_transient = 1;
        if ((ret = device_state_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state)) < 0)
            goto done;
         /* Compare transient with last sync*/
        if (ret && (ret = device_config_compare(h, dh, name, ct, &eq)) < 0)
            goto done;
        if (ret == 0){ /* closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, device_handle_logmsg_get(dh)) < 0)
                goto done;
            break;
        }
        if (eq != 0){
            if (controller_transaction_failed(h, tid, ct, dh, 0, name, "Device changed config") < 0)
                goto done;
            if (device_state_set(dh, CS_OPEN) < 0)
                goto done;
            /* 2.2.2.1 Leave transaction */
            device_handle_tid_set(dh, 0);
            /* 2.2.2.2 If no devices in transaction, mark as OK and close it*/
            if (controller_transaction_devices(h, tid) == 0){
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            }
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        /* 2.2 The transaction is OK 
           Proceed to next step: get saved edit-msg and send it */
        if ((cbmsg = device_handle_outmsg_get(dh)) == NULL){
            device_close_connection(dh, "Device %s no edit-msg in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (clicon_msg_send1(s, cbmsg) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_EDIT) < 0)
            goto done;        
        break;  
    case CS_PUSH_EDIT:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, 0, name, cbuf_get(cberr)) < 0)
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
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, NULL) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
            /* 2.1.1 Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }        
        /* 2.2 The transaction is OK 
           Proceed to next step */
        if ((ret = device_send_validate(h, dh)) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_VALIDATE) < 0)
            goto done;
       break;
    case CS_PUSH_VALIDATE:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                                    name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, 0, name, cbuf_get(cberr)) < 0)
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
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, NULL) < 0)
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
        /* 2. The device is OK */
        if (device_state_set(dh, CS_PUSH_WAIT) < 0)
            goto done;
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
            /* 2.1.1 Trigger DISCARD of the device */
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
            break;
        }        
        /* 2.2 The transaction is OK */
        /* 2.2.1 Check if all devices are in WAIT (none are in EDIT/VALIDATE) */
        if ((ret = controller_transaction_wait(h, tid)) < 0)
            goto done;
        if (ret == 1){
            /* 2.2.1 All devices are in WAIT (none are in EDIT/VALIDATE) 
               2.2.1.1 Trigger COMMIT of all devices */
            if (controller_transaction_wait_trigger(h, tid, 1) < 0)
                goto done;            
        }
        break;
    case CS_PUSH_DISCARD:
    case CS_PUSH_COMMIT:
        if (tid == 0 || ct == NULL){
            device_close_connection(dh, "Device %s not associated with transaction in state %s",
                name, device_state_int2str(conn_state));
            break;
        }
        if (ct->ct_state != TS_INIT && ct->ct_state == TS_RESOLVED){
            clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                         __FUNCTION__, name, rpcname, transaction_state_int2str(ct->ct_state));
            break;
        }
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state, &cberr)) < 0)
            goto done;
        if (ret == 0){      /* 1. The device has failed: received rpc-error/not <ok>  */
            if (controller_transaction_failed(h, tid, ct, dh, 2, name, cbuf_get(cberr)) < 0)
                goto done;
            break;
        }
        else if (ret == 1){ /* 1. The device has failed and is closed */
            if (controller_transaction_failed(h, tid, ct, dh, 1, name, NULL) < 0)
                goto done;
            break;
        }
        /* 2. The device is OK */
        if (ct->ct_state == TS_RESOLVED){ 
            /* 2.1 But transaction is in error state */
            assert(ct->ct_result != TR_SUCCESS);
        }
        if (device_state_set(dh, CS_OPEN) < 0)
            goto done;
        /* 2.2.2.1 Leave transaction */
        device_handle_tid_set(dh, 0);
        if (conn_state == CS_PUSH_COMMIT){
            cxobj *xt = NULL;
            cbuf  *cb = NULL;
    
            /* Copy transient to device config (last sync) 
               XXXX in commit push
             */
            if ((cb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            if ((cberr = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "devices/device[name='%s']/config", name);
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
                    clicon_err(OE_XML, 0, "%s", cbuf_get(cberr));
                    goto done;
                }
            }
            if (cb)
                cbuf_free(cb);
            if (xt)
                xml_free(xt);
        }
        /* 2.2.2.2 If no devices in transaction, mark as OK and close it*/
        if (controller_transaction_devices(h, tid) == 0){
            /* If source datastore is candidate, then commit (from actions) 
             * - if actions=AT_COMMIT, commit is made from actions-db
             * - otherwise if datastore is candidate, commit is made from candidate
             */
            if (ct->ct_actions_type != AT_NONE && strcmp(ct->ct_sourcedb, "candidate")==0){
                if ((cberr = cbuf_new()) == NULL){
                    clicon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                /* What to copy to candidate and commit to running? */
                if (xmldb_copy(h, "actions", "candidate") < 0)
                    goto done;
                if ((ret = candidate_commit(h, NULL, "candidate", 0, 0, cberr)) < 0){
                    /* Handle that candidate_commit can return < 0 if transaction ongoing */        
                    cprintf(cberr, "%s", clicon_err_reason);
                    ret = 0;
                }
                if (ret == 0){ // XXX awkward, cb ->xml->cb
                    cxobj *xerr = NULL;
                    cbuf *cberr2 = NULL;
                    if ((cberr2 = cbuf_new()) == NULL){
                        clicon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    if (clixon_xml_parse_string(cbuf_get(cberr), YB_NONE, NULL, &xerr, NULL) < 0)
                        goto done;
                    if (netconf_err2cb(xerr, cberr2) < 0)
                        goto done;
                    if (controller_transaction_failed(h, ct->ct_id, ct, dh, 1, name, cbuf_get(cberr2)) < 0)
                        goto done;
                    if (xerr)
                        xml_free(xerr);
                    if (cberr2)
                        cbuf_free(cberr2);
                    break;
                }
            }
            if (ct->ct_state != TS_RESOLVED){
                controller_transaction_state_set(ct, TS_RESOLVED, TR_SUCCESS);
                if (controller_transaction_notify(h, ct) < 0)
                    goto done;
            }
            if (controller_transaction_done(h, ct, TR_SUCCESS) < 0)
                goto done;
        }
        break;
    case CS_PUSH_WAIT:
    case CS_CLOSED:
    case CS_OPEN:
    default:
        clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                     __FUNCTION__, name, rpcname,
                     device_state_int2str(conn_state));
        clicon_debug_xml(2, xmsg, "Message");
        device_close_connection(dh, "Unexpected msg %s in state %s",
                                rpcname, device_state_int2str(conn_state));
        break;
    }
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    return retval;
}

/*! Get netconf device statedata
 *
 * @param[in]    h        Clicon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPATH syntax. or NULL for all
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
    cxobj        **vec = NULL;
    size_t         veclen;
    cxobj         *xret = NULL;
    int            i;
    cxobj         *xn;
    char          *name;
    device_handle  dh;
    cbuf          *cb = NULL;
    conn_state     state;
    char          *logmsg;
    struct timeval tv;
    cxobj         *xcaps;
    cxobj         *x;
    char          *xb;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((name = xml_find_body(xn, "name")) == NULL)
            continue;
        if ((dh = device_handle_find(h, name)) == NULL)
            continue;
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
                xml_chardata_cbuf_append(cb, xb);
                cprintf(cb, "</capability>");
            }
            cprintf(cb, "</capabilities>");
        }
        device_handle_conn_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<conn-state-timestamp>%s</conn-state-timestamp>", timestr);
        }
        device_handle_sync_time_get(dh, &tv);
        if (tv.tv_sec != 0){
            char timestr[28];            
            if (time2str(tv, timestr, sizeof(timestr)) < 0)
                goto done;
            cprintf(cb, "<sync-timestamp>%s</sync-timestamp>", timestr);
        }
        if ((logmsg = device_handle_logmsg_get(dh)) != NULL){
            cprintf(cb, "<logmsg>");
            xml_chardata_cbuf_append(cb, logmsg);
            cprintf(cb, "</logmsg>");
        }
        cprintf(cb, "</device></devices>");
        if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xstate, NULL) < 0)
            goto done;
        cbuf_reset(cb);
    } /* devices */
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}
