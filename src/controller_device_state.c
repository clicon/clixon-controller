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
    {"PUSH_EDIT",    CS_PUSH_EDIT},
    {"PUSH_VALIDATE",CS_PUSH_VALIDATE},
    {"PUSH_WAIT",    CS_PUSH_WAIT},
    {"PUSH_COMMIT",  CS_PUSH_COMMIT},
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
 * @param[in]  state  Device state as int
 * @retval     str    Device state as string
 */
char *
device_state_int2str(conn_state_t state)
{
    return (char*)clicon_int2str(csmap, state);
}

/*! Map controller device connection state from string to int 
 * @param[in]  str    Device state as string
 * @retval     state  Device state as int
 */
conn_state_t
device_state_str2int(char *str)
{
    return clicon_str2int(csmap, str);
}

/*! Map yang config from string to int
 * @param[in]  str    Yang config as string
 * @retval     yf     Yang config as int
 */
yang_config_t
yang_config_str2int(char *str)
{
    return clicon_str2int(yfmap, str);
}

/*! Close connection, unregister events and timers
 * @param[in]  dh      Clixon client handle.
 * @param[in]  format  Format string for Log message
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
    
    s = device_handle_socket_get(dh);
    clixon_event_unreg_fd(s, device_input_cb); /* deregister events */
    device_handle_disconnect(dh);              /* close socket, reap sub-processes */
    device_handle_yang_lib_set(dh, NULL);
    if (device_state_set(dh, CS_CLOSED) < 0)
        goto done;
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
        clicon_debug(1, "%s %s: %s", __FUNCTION__, device_handle_name_get(dh), str);
    }
    retval = 0;
 done:
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
    device_handle dh = (device_handle)arg;
    clixon_handle h;
    int           retval = -1;
    int           eom = 0;
    int           eof = 0;
    int           frame_state; /* only used for chunked framing not eom */
    size_t        frame_size;
    cbuf         *cb;
    yang_stmt    *yspec;
    cxobj        *xtop = NULL;
    cxobj        *xmsg;
    int           ret;
    char         *name;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    h = device_handle_handle_get(dh);
    frame_state = device_handle_frame_state_get(dh);
    frame_size = device_handle_frame_size_get(dh);
    cb = device_handle_frame_buf_get(dh);
    name = device_handle_name_get(dh);
    /* Read data, if eom set a frame is read
     */
    if (netconf_input_msg(s,
                          clicon_option_int(h, "netconf-framing"),
                          &frame_state, &frame_size,
                          cb, &eom, &eof) < 0)
        goto done;
    if (eof){         /* Close connection, unregister events, free mem */
        clicon_debug(1, "%s %s: eom:%d eof:%d len:%lu Remote socket endpoint closed", __FUNCTION__,
                     name, eom, eof, cbuf_len(cb));
        device_close_connection(dh, "Remote socket endpoint closed");
        goto ok;
    }
    device_handle_frame_state_set(dh, frame_state);
    device_handle_frame_size_set(dh, frame_size);
    if (eom == 0){ /* frame not complete */
        clicon_debug(CLIXON_DBG_DETAIL, "%s %s: frame: %lu strlen:%lu", __FUNCTION__,
                     name, cbuf_len(cb), strlen(cbuf_get(cb)));
        goto ok;
    }
    clicon_debug(1, "%s %s: frame: %lu strlen:%lu", __FUNCTION__,
                 name, cbuf_len(cb), strlen(cbuf_get(cb)));
    cbuf_trunc(cb, cbuf_len(cb));
    clicon_debug(CLIXON_DBG_MSG, "Recv dev: %s", cbuf_get(cb));
    yspec = clicon_dbspec_yang(h);
    if ((ret = netconf_input_frame(cb, yspec, &xtop)) < 0)
        goto done;
    cbuf_reset(cb);
    if (ret==0){
        device_close_connection(dh, "Invalid frame");
        goto ok;
    }
    xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
    if (xmsg && device_state_handler(h, dh, s, xmsg) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DETAIL, "%s retval:%d", __FUNCTION__, retval);
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
    cprintf(cb, "<root/>");
    cprintf(cb, "</device></devices>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &xt, NULL)) < 0)
        goto done;
    if (xml_name_set(xt, "config") < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/root")) == NULL){
        clicon_err(OE_XML, 0, "device/root mountpoint not found");
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

#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
/*! Rewrite of junos YANGs after parsing
 *
 * Add grouping command-forwarding in junos-rpc yangs if not exists
 * tried to make other less intrusive solutions or make a generic way in the
 * original function, but the easiest was just to rewrite the function.
 * @param[in] h       Clicon handle
 * @param[in] yanglib XML tree on the form <yang-lib>...
 * @param[in] yspec   Will be populated with YANGs, is consumed
 * @retval    1       OK
 * @retval    0       Parse error
 * @retval    -1      Error
 * @see yang_lib2yspec  the original function
 */
static int
yang_lib2yspec_junos_patch(clicon_handle h,
                           cxobj        *yanglib,
                           yang_stmt    *yspec)
{
    int        retval = -1;
    cxobj     *xi;
    char      *name;
    char      *revision;
    cvec      *nsc = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;
    yang_stmt *ymod;
    yang_stmt *yrev;
    int        modmin = 0;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (xpath_vec(yanglib, nsc, "module-set/module", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xi = vec[i];
        if ((name = xml_find_body(xi, "name")) == NULL)
            continue;
        if ((revision = xml_find_body(xi, "revision")) == NULL)
            continue;
        if ((ymod = yang_find(yspec, Y_MODULE, name)) != NULL ||
            (ymod = yang_find(yspec, Y_SUBMODULE, name)) != NULL){
            /* Skip if matching or no revision 
             * Note this algorithm does not work for multiple revisions
             */
            if ((yrev = yang_find(ymod, Y_REVISION, NULL)) == NULL){
                modmin++;
                continue;
            }
            if (strcmp(yang_argument_get(yrev), revision) == 0){
                modmin++;
                continue;
            }
        }
        if (yang_parse_module(h, name, revision, yspec, NULL) == NULL)
            goto fail;
    }
    /* XXX: Ensure yang-lib is always there otherwise get state dont work for mountpoint */
    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-yang-library")) != NULL &&
        (yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL &&
        strcmp(yang_argument_get(yrev), "2019-01-04") == 0){
        modmin++;
    }
    else if (yang_parse_module(h, "ietf-yang-library", "2019-01-04", yspec, NULL) < 0)
        goto fail;
    clicon_debug(1, "%s yang_parse_post", __FUNCTION__);
    if (yang_parse_post(h, yspec, modmin) < 0)
        goto done;
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}
#endif  /* CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING */

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
    device_handle dh = (device_handle)arg;

    device_close_connection(dh, "Timeout waiting for remote peer");
    return 0;
}

/*! Set timeout of transient device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_register(device_handle dh)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    uint32_t       d;
    clixon_handle  h;
    cbuf          *cb = NULL;

    gettimeofday(&t, NULL);
    h = device_handle_handle_get(dh);
    d = clicon_option_int(h, "controller_device_timeout");
    if (d)
        t1.tv_sec = d;
    else
        t1.tv_sec = 60;
    t1.tv_usec = 0;
    timeradd(&t, &t1, &t);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "Device %s in state %s",
            device_handle_name_get(dh),
            device_state_int2str(device_handle_conn_state_get(dh)));
    if (clixon_event_reg_timeout(t, device_state_timeout, dh,
                                 cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Cancel timeout of transiet device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_unregister(device_handle dh)
{
    int retval = -1;

    if (clixon_event_unreg_timeout(device_state_timeout, dh) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
    
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
                 conn_state_t  state)
{
    int retval = -1;
    conn_state_t state0;

    /* From state handling */
    state0 = device_handle_conn_state_get(dh);
    if (state0 != CS_CLOSED && state0 != CS_OPEN){
        if (device_state_timeout_unregister(dh) < 0)
            goto done;
    }
    /* To state handling */
    device_handle_conn_state_set(dh, state);
    if (state == CS_CLOSED) 
        device_handle_tid_set(dh, 0); /* Ensure no transaction id in closed device */
    if (state != CS_CLOSED && state != CS_OPEN){
        if (device_state_timeout_register(dh) < 0)
            goto done;
    }
    if (state == CS_DEVICE_SYNC)
        device_handle_dryrun_set(dh, 0);
    retval = 0;
 done:
    return retval;
}

/*! Check device push transaction
 *
 * Go through all devices in a transaction
 * If all are in wait state, then start commiting all devices in that transaction
 * @note This is best-effort and only handles one transaction and no discard
 * @param[in] h     Clixon handle.
 * @param[in] dh    Clixon client handle.
 * @retval    1     OK
 * @retval    0     Closed
 * @retval   -1     Error
 */
static int
device_push_check(clicon_handle h,
                  device_handle dh)
{
    int           retval = -1;
    device_handle dh1;
    int           s;

    if (device_state_set(dh, CS_PUSH_WAIT) < 0)
        goto done;
    dh1 = NULL;
    while ((dh1 = device_handle_each(h, dh1)) != NULL){
        if (device_handle_conn_state_get(dh1) == CS_PUSH_EDIT ||
            device_handle_conn_state_get(dh1) == CS_PUSH_VALIDATE)
            break;
    }
    if (dh1 == NULL){ /* None are in PUSH_EDIT/VALIDATE */
        dh1 = NULL;
        while ((dh1 = device_handle_each(h, dh1)) != NULL){
            if (device_handle_conn_state_get(dh1) != CS_PUSH_WAIT)
                continue;
            s = device_handle_socket_get(dh1);            
            if (device_send_commit(h, dh1, s) < 0)
                goto done;
            if (device_state_set(dh1, CS_PUSH_COMMIT) < 0)
                goto done;
        }
    }
    retval = 1;
 done:
    return  retval;
// closed:
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
    int            retval = -1;
    char          *rpcname;
    char          *name;
    conn_state_t   conn_state;
    yang_stmt     *yspec0;
    yang_stmt     *yspec1;
    int            nr;
    int            ret;
    uint64_t       tid;

    rpcname = xml_name(xmsg);
    conn_state = device_handle_conn_state_get(dh);
    name = device_handle_name_get(dh);
    yspec0 = clicon_dbspec_yang(h);
    switch (conn_state){
    case CS_CONNECTING:
        /* Receive hello from device, send hello */
        if ((ret = device_state_recv_hello(h, dh, s, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
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
        /* Receive netconf-state schema list from device */
        if ((ret = device_state_recv_schema_list(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
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
            if (device_send_sync(h, dh, s) < 0)
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
        /* Receive get-schema and write to local yang file */
        if ((ret = device_state_recv_get_schema(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
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
            if (device_send_sync(h, dh, s) < 0)
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
        /* Receive config data from device and add config to mount-point */
        if ((ret = device_state_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        if (device_state_set(dh, CS_OPEN) < 0)
            goto done;
        /* XXX More logic here */
        if ((tid = device_handle_tid_get(dh)) != 0){
            if (controller_transaction_notify(h, tid, 1, NULL) < 0)
                goto done;
        }
        break;
    case CS_PUSH_EDIT:
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        if ((ret = device_send_validate(h, dh, s)) < 0)
            goto done;
        if (device_state_set(dh, CS_PUSH_VALIDATE) < 0)
            goto done;
        break;
    case CS_PUSH_VALIDATE:
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        if ((ret = device_push_check(h, dh)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        break;
    case CS_PUSH_COMMIT:
        if ((ret = device_state_recv_ok(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* XXX closed actually this is more dangerous */
            break;
        if (device_state_set(dh, CS_OPEN) < 0)
            goto done;
        /* XXX more logic here */
        if ((tid = device_handle_tid_get(dh)) != 0){
            if (controller_transaction_notify(h, tid, 1, NULL) < 0)
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
    return retval;
}
