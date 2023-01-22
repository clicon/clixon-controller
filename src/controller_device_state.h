/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
     |       |
     |       v
     |<-- CS_SCHEMA_ONE(n) ---+
     |       |             <--+
     |       v             
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
typedef void *device_handle;

/*! State of connection
 * Only closed and open are "stable", the others are transient and timeout to closed
 * @see clixon-controller@2023-01-01.yang for mirror enum and descriptions
 * @see csmap translation table
 */
enum conn_state{
    CS_CLOSED = 0,  /* Closed, also "closed" if handle non-existent but then no state */
    CS_CONNECTING,  /* Connect() called, expect to receive hello from device
                       May fail due to (1) connect fails or (2) hello not receivd */
    CS_SCHEMA_LIST, /* Get ietf-netconf-monitor schema state */
    CS_SCHEMA_ONE,      /* Connection established and Hello sent to device. */
    CS_DEVICE_SYNC, /* Get all config (and state) */
    CS_OPEN,        /* Connection established and Hello sent to device. */
    CS_WRESP,       /* Request sent, waiting for reply */
};
typedef enum conn_state conn_state_t;

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif
    
char        *device_state_int2str(conn_state_t state);
conn_state_t device_state_str2int(char *str);
int          device_close_connection(device_handle ch, const char *format, ...) __attribute__ ((format (printf, 2, 3)));
int          device_input_cb(int s, void *arg);
int          device_send_sync(clixon_handle h, device_handle ch, int s);
int          device_state_timeout_register(device_handle ch);
int          device_state_timeout_unregister(device_handle ch);
int          device_state_handler(clixon_handle h, device_handle ch, int s, cxobj *xmsg);
    
#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_DEVICE_STATE_H */
