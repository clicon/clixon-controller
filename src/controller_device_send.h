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

  * Routines for sending netconf messages to devices
  */

#ifndef _CONTROLLER_DEVICE_SEND_H
#define _CONTROLLER_DEVICE_SEND_H

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif
    
int device_send_sync(clixon_handle h, device_handle ch, int s);
int device_send_get_schema_next(clixon_handle h, device_handle dh, int s, int *nr);
int device_send_get_schema_list(clixon_handle h, device_handle dh, int s);
int device_create_edit_config_diff(clixon_handle h, device_handle dh,
                                   cxobj *x0, cxobj *x1, yang_stmt *yspec,
                                   cxobj **dvec, int dlen,
                                   cxobj **avec, int alen,
                                   cxobj **chvec0, cxobj **chvec1, int chlen,
                                   cbuf **cbret);
int device_send_validate(clixon_handle h, device_handle dh);
int device_send_commit(clixon_handle h, device_handle dh);
int device_send_discard_changes(clixon_handle h, device_handle dh);
    
#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_DEVICE_SEND_H */
