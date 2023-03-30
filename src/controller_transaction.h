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

/* Controller transaction id beyond 16-bit to != pid? */
#define TRANSACTION_CLIENT_ID 0x199999

/*! Clixon controller meta transactions spanning device operation
 */
struct controller_transaction_t{
    qelem_t  ct_qelem;           /* List header */
    uint64_t ct_id;              /* transaction-id */
    transaction_state  ct_state; /* Transaction state */
    transaction_result ct_result;/* Transaction result */
    void    *ct_h;               /* Back-pointer to clixon handle (for convenience in timeout callbacks) */
    uint32_t ct_client_id;       /* Client id of originator */
    int      ct_pull_transient;  /* pull: dont commit locally */
    int      ct_pull_merge;      /* pull: Merge instead of replace */
    int      ct_push_validate;   /* push: Validate on remote device dont commit */
    char    *ct_description;     /* Description of transaction */
    char    *ct_origin;          /* Originator of error (if result=0) */
    char    *ct_reason;          /* Reason of error (if result=0) */
    struct timeval ct_timestamp; /* Timestamp when entering current state */
};
typedef struct controller_transaction_t controller_transaction;
    
/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

int   controller_transaction_state_set(controller_transaction *ct, transaction_state state, transaction_result result);
int   controller_transaction_notify(clixon_handle h, controller_transaction *ct);
int   controller_transaction_new(clicon_handle h, char *description, controller_transaction **ct, cbuf **cberr);
int   controller_transaction_free(clicon_handle h, controller_transaction *ct);
int   controller_transaction_done(clicon_handle h, controller_transaction *ct, transaction_result result);

controller_transaction *controller_transaction_find(clixon_handle h, const uint64_t id);
int   controller_transaction_devices(clicon_handle h, uint64_t tid);
int   controller_transaction_failed(clicon_handle h, uint64_t tid, controller_transaction *ct, device_handle dh,
                                    int devclose, char *origin, char *reason);
int   controller_transaction_wait(clicon_handle h, uint64_t tid);
int   controller_transaction_wait_trigger(clicon_handle h, uint64_t tid, int commit);
int   controller_transactions_statedata(clixon_handle h, cvec *nsc, char *xpath, cxobj *xstate);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_TRANSACTION_H */
