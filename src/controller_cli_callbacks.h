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
  * These functions are called from CLISPECs, ie controller_operation.cli or /_configure.cli
  */

#ifndef _CONTROLLER_CLI_CALLBACKS_H
#define _CONTROLLER_CLI_CALLBACKS_H

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

int rpc_get_yanglib_mount_match(clixon_handle h, char *pattern, int single, int yanglib, cxobj **xdevsp);
int cli_show_auto_devs(clixon_handle h, cvec *cvv, cvec *argv);
int cli_rpc_pull(clixon_handle h, cvec *cvv, cvec *argv);
int cli_rpc_controller_commit(clixon_handle h, cvec *cvv, cvec *argv);
int cli_connection_change(clixon_handle h, cvec *cvv, cvec *argv);
int cli_show_connections(clixon_handle h, cvec *cvv, cvec *argv);
int cli_show_transactions(clixon_handle h, cvec *cvv, cvec *argv);
int compare_device_db_sync(clixon_handle h, cvec *cvv, cvec *argv);
int compare_device_db_dev(clixon_handle h, cvec *cvv, cvec *argv);
int check_device_db(clixon_handle h, cvec *cvv, cvec *argv);
int cli_auto_set_devs(clixon_handle h, cvec *cvv, cvec *argv);
int cli_auto_merge_devs(clixon_handle h, cvec *cvv, cvec *argv);
int cli_auto_del_devs(clixon_handle h, cvec *cvv, cvec *argv);
int cli_auto_load_devs(clixon_handle h, cvec *cvv0, cvec *argv);
int cli_controller_show_version(clixon_handle h, cvec *vars, cvec *argv);
int show_yang_revisions(clixon_handle h, cvec *cvv, cvec *argv);
int show_device_capability(clixon_handle h, cvec *cvv, cvec *argv);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_CLI_CALLBACKS_H */
