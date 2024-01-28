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
  *
  * Device handle, hidden C struct and accessor functions
  */

#ifndef _CONTROLLER_DEVICE_HANDLE_H
#define _CONTROLLER_DEVICE_HANDLE_H

/*
 * Types
 */
/* Abstract device handle, see struct controller_device_handle for concrete struct */
typedef void *device_handle;

/*
 * Prototypes
 */

#ifdef __cplusplus
extern "C" {
#endif

device_handle device_handle_new(clixon_handle h, const char *name);
int    device_handle_free(device_handle dh);
int    device_handle_free_all(clixon_handle h);
device_handle device_handle_find(clixon_handle h, const char *name);
device_handle  device_handle_each(clixon_handle h, device_handle dhprev);
int    device_handle_connect(device_handle dh, clixon_client_type socktype, const char *dest,
                             int stricthostkey);
int    device_handle_disconnect(device_handle dh);

/* Accessor functions */
char  *device_handle_name_get(device_handle dh);
int    device_handle_socket_get(device_handle dh);
uint64_t device_handle_msg_id_getinc(device_handle dh);
uint64_t device_handle_tid_get(device_handle dh);
int      device_handle_tid_set(device_handle dh, uint64_t tid);
clixon_handle device_handle_handle_get(device_handle dh);
conn_state    device_handle_conn_state_get(device_handle dh);
yang_config_t device_handle_yang_config_get(device_handle dh);
int    device_handle_yang_config_set(device_handle dh, char *yfstr);
int    device_handle_conn_state_set(device_handle dh, conn_state  state);
int    device_handle_conn_time_get(device_handle dh, struct timeval *t);
int    device_handle_conn_time_set(device_handle dh, struct timeval *t);
int    device_handle_frame_state_get(device_handle dh);
int    device_handle_frame_state_set(device_handle dh, int state);
size_t device_handle_frame_size_get(device_handle dh);
int    device_handle_frame_size_set(device_handle dh, size_t size);
cbuf  *device_handle_frame_buf_get(device_handle dh);
netconf_framing_type device_handle_framing_type_get(device_handle dh);
int    device_handle_framing_type_set(device_handle dh, netconf_framing_type ft);
cxobj *device_handle_capabilities_get(device_handle dh);
int    device_handle_capabilities_set(device_handle dh, cxobj *xcaps);
int    device_handle_capabilities_find(clixon_handle ch, const char *name);
cxobj *device_handle_yang_lib_get(device_handle dh);
int    device_handle_yang_lib_set(device_handle dh, cxobj *xylib);
int    device_handle_yang_lib_append(device_handle dh, cxobj *xylib);
int    device_handle_sync_time_get(device_handle dh, struct timeval *t);
int    device_handle_sync_time_set(device_handle dh, struct timeval *t);
int    device_handle_nr_schemas_get(device_handle dh);
int    device_handle_nr_schemas_set(device_handle dh, int nr);
char  *device_handle_schema_name_get(device_handle dh);
int    device_handle_schema_name_set(device_handle dh, char *schema_name);
char  *device_handle_schema_rev_get(device_handle dh);
int    device_handle_schema_rev_set(device_handle dh, char  *schema_rev);
char  *device_handle_logmsg_get(device_handle dh);
int    device_handle_logmsg_set(device_handle dh, char *logmsg);
cbuf  *device_handle_outmsg_get(device_handle dh);
int    device_handle_outmsg_set(device_handle dh, cbuf *cb);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_DEVICE_HANDLE_H */
