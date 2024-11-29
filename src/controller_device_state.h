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

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  State machine:

  Device connection state machine:

  CS_CLOSED
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
     |       |
     |       v
  CS_OPEN <-+
  */

#ifndef _CONTROLLER_DEVICE_STATE_H
#define _CONTROLLER_DEVICE_STATE_H

/*
 * Types
 */
typedef void *device_handle; // duplicated from controller_device_handle.h

/*! State of connection
 *
 * Only closed and open are "stable", the others are transient and timeout to closed
 * @see clixon-controller@2023-01-01.yang connection-state
 * @see csmap translation table
 */
enum conn_state_t {
    CS_CLOSED = 0,    /* Closed, also "closed" if handle non-existent but then no state */
    CS_OPEN,          /* Connection established and Hello sent to device. */

    /* Connect state machine */
    CS_CONNECTING,    /* Connect() called, expect to receive hello from device
                         May fail due to (1) connect fails or (2) hello not receivd */
    CS_SCHEMA_LIST,   /* Get ietf-netconf-monitor schema state */
    CS_SCHEMA_ONE,    /* Connection established and Hello sent to device (nr substate) */
    CS_DEVICE_SYNC,   /* Get all config (transient+merge are sub-state parameters) */

    /* Push state machine */
    CS_PUSH_LOCK,     /* Lock device candidate */
    CS_PUSH_CHECK,    /* sync device transient to check if device is unchanged */
    CS_PUSH_EDIT,     /* First edit-config sent (if any) waiting for reply */
    CS_PUSH_EDIT2,    /* Second edit-config sent (if any), waiting for reply */
    CS_PUSH_VALIDATE, /* validate sent, waiting for reply  */
    CS_PUSH_WAIT,     /* Waiting for other devices to validate */
    CS_PUSH_COMMIT,   /* commit sent, waiting for reply ok */
    CS_PUSH_COMMIT_SYNC, /* After remote commit, received remote config, commit it in
                     controller (only used if CONTROLLER_EXTRA_PUSH_SYNC */
    CS_PUSH_DISCARD,  /* discard sent, waiting for reply ok */
    CS_PUSH_UNLOCK,   /* Unlock device candidate */

    /* Generic RPC state machine */
    CS_RPC_GENERIC,   /* Sent a generic RPC to device, wait for reply */
};
typedef enum conn_state_t conn_state;

/*! How to bind device configuration to YANG
 *
 * @see clixon-controller@2023-01-01.yang yang-config
 * @see yfmap translation table
 * @see validate_level
 * XXX Could this be same as validate_level?
 */
enum yang_config_t {
    YF_NONE,     /* Do not bind YANG to config */
    YF_BIND,     /* Bind YANG model to config, but do not fully validate */
    YF_VALIDATE, /* Fully validate device config */
};
typedef enum yang_config_t yang_config_t;

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

char        *device_state_int2str(conn_state state);
conn_state   device_state_str2int(char *str);
yang_config_t  yang_config_str2int(char *str);
int          device_close_connection(device_handle ch, const char *format, ...) __attribute__ ((format (printf, 2, 3)));
int          device_input_cb(int s, void *arg);
int          device_send_get(clixon_handle h, device_handle ch, int s, int state, char *xpath);
int          device_state_mount_point_get(char *devicename, yang_stmt *yspec,
                                          cxobj **xtp, cxobj **xrootp);
int          device_state_timeout_register(device_handle ch);
int          device_state_timeout_unregister(device_handle ch);
int          device_state_set(device_handle dh, conn_state state);
int          device_config_read(clixon_handle h, char *devname, char *config_type, cxobj **xrootp, cbuf **cberr);
int          device_config_write(clixon_handle h, char *name, char *config_type, cxobj *xdata, cbuf *cbret);
int          device_state_handler(clixon_handle h, device_handle ch, int s, cxobj *xmsg);
int          devices_statedata(clixon_handle h, cvec *nsc, char *xpath, cxobj *xstate);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_DEVICE_STATE_H */
