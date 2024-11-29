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

  * Routines for receiving netconf messages from devices
  */

#ifndef _CONTROLLER_DEVICE_RECV_H
#define _CONTROLLER_DEVICE_RECV_H

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

int device_recv_hello(clixon_handle h, device_handle dh, int s, cxobj *xmsg,
                      char *rpcname, conn_state  conn_state);
int device_recv_config(clixon_handle h, device_handle dh, cxobj *xmsg,
                       yang_stmt *yspec0, char *rpcname, conn_state conn_state,
                       int force_transient, int force_merge);
int device_recv_schema_list(device_handle dh, cxobj *xmsg, char *rpcname,
                            conn_state conn_state);
int device_recv_get_schema(device_handle dh, cxobj *xmsg, char *rpcname,
                           conn_state conn_state);
int device_recv_ok(clixon_handle h, device_handle dh, cxobj *xmsg, char *rpcname,
                   conn_state conn_state, cbuf **cberr);
int device_recv_generic_rpc(clixon_handle h, device_handle dh, controller_transaction *ct, cxobj *xmsg,
                            char *rpcname, conn_state conn_state, cbuf **cberr);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_DEVICE_RECV_H */
