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
  * XXX Temporary new clixon_client.c to replace the one in lib/src
  */

#ifndef _CLIXON_CLIENT2_H
#define _CLIXON_CLIENT2_H

/*
 * Types
 */
typedef void *clixon_handle;
typedef void *clixon_client_handle;

/*! State of connection
 */
enum conn_state{
    CS_CLOSED = 0,  /* Closed */
    CS_CONNECTED,   /* Connect() called */
    CS_OPEN,        /* Connection established and Hello sent to device. */
    CS_WRESP,         /* Request sent, waiting for reply */
};
typedef enum conn_state conn_state_t;

/*
 * Prototypes
 */

#ifdef __cplusplus
extern "C" {
#endif
    
clixon_client_handle clixon_client2_new(clicon_handle h, const char *name);
int    clixon_client2_free(clixon_client_handle ch);
clixon_client_handle clixon_client2_find(clicon_handle h, const char *name);
char  *controller_state_int2str(conn_state_t state);
conn_state_t controller_state_str2int(char *str);
int    clixon_client2_connect(clixon_client_handle ch, clixon_client_type socktype, const char *dest);
int    clixon_client2_disconnect(clixon_client_handle ch);

/* Accessor functions */
char  *clixon_client2_name_get(clixon_client_handle ch);
int    clixon_client2_socket_get(clixon_client_handle ch);
clicon_handle clixon_client2_handle_get(clixon_client_handle ch);
conn_state_t clixon_client2_conn_state_get(clixon_client_handle ch);
int    clixon_client2_conn_state_set(clixon_client_handle ch, conn_state_t state);    
int    clixon_client2_frame_state_get(clixon_client_handle ch);
int    clixon_client2_frame_state_set(clixon_client_handle ch, int state);
size_t clixon_client2_frame_size_get(clixon_client_handle ch);
int    clixon_client2_frame_size_set(clixon_client_handle ch, size_t size);
cbuf  *clixon_client2_frame_buf_get(clixon_client_handle ch);
int    clixon_client2_capabilities_set(clixon_client_handle ch, cxobj *xcaps);
int    clixon_client2_capabilities_find(clicon_handle ch, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _CLIXON_CLIENT2_H */
