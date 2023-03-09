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

  * Meta-transaction for client/controller/device commit and rollback
  */

#ifndef _CONTROLLER_TRANSACTION_H
#define _CONTROLLER_TRANSACTION_H

/*! clixon controller meta transactions
 */
struct controller_transaction_t{
    qelem_t       ct_qelem;      /* List header */
    uint64_t      ct_id;         /* transaction-id */
    uint32_t      ct_client_id;  /* Client id of originator */
    int           ct_dryrun;     /* Validate on device dont commit (push) */
    int           ct_merge;      /* Merge instead of replace (pull) */
};
typedef struct controller_transaction_t controller_transaction;
    
/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

int controller_transaction_notify(clixon_handle h, uint64_t tid, int status, char *origin, char *reason);
int controller_transaction_new(clicon_handle h, controller_transaction **ct);
controller_transaction *controller_transaction_find(clixon_handle h, const uint64_t id);
    
#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_TRANSACTION_H */
