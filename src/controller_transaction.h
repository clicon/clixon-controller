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

/* Keep transaction devices data structures (replies from device rpc or state requests) in s, 0 means no limit */
#define TRANSACTION_DEVICES_TIMEOUT 300

/*! Keep this many transactions in list, 0 means unlimited */
#define TRANSACTION_MAX_NR 100

/*! Clixon controller distributed transactions spanning device operation
 */
struct controller_transaction_t{
    qelem_t            ct_qelem;         /* List header */
    uint64_t           ct_id;            /* Transaction-id */
    transaction_state  ct_state;         /* Transaction state */
    transaction_result ct_result;        /* Transaction result */
    void              *ct_h;             /* Back-pointer to clixon handle (for convenience in timeout callbacks) */
    uint32_t           ct_client_id;     /* Client id of originator (may be stale) */
    char              *ct_username;      /* Client username creating the transaction */
    int                ct_pull_transient;/* pull: dont commit locally */
    int                ct_pull_merge;    /* pull: Merge instead of replace */
    push_type          ct_push_type;     /* push to remote devices: Do not, validate, or commit */
    actions_type       ct_actions_type;  /* How to trigger service-commit notifications,
                                            and thereby action scripts */
    char              *ct_sourcedb;      /* Source datastore (candidate or running)
                                            as given by rpc controller-commit (stripped prefix) */
    char              *ct_description;   /* Description of transaction */
    char              *ct_origin;        /* Originator of error (if result is != SUCCESS) */
    char              *ct_reason;        /* Reason of error (if result != SUCCESS) */
    char              *ct_warning;       /* Warning, first encountered */
    struct timeval     ct_timestamp0;    /* Timestamp when created */
    struct timeval     ct_timestamp;     /* Timestamp when entering current state */
    cxobj             *ct_devdata;       /* Generic device data, eg CS_RPC_GENERIC */
};
typedef struct controller_transaction_t controller_transaction;

/*! Transaction failed device close parameter
 *
 * If device handle is set, one can ignore, leave or close
 */
enum tr_failed_devclose_t {
    TR_FAILED_DEV_IGNORE,  /* Ignore device just fail transaction, if device already closed */
    TR_FAILED_DEV_LEAVE,   /* Device leaves the transaction */
    TR_FAILED_DEV_CLOSE    /* Close the device and leave the transaction */
};
typedef enum tr_failed_devclose_t tr_failed_devclose;

/*
 * Macros
 */
#define controller_transaction_failed(h, tid, ct, dh, devclose, origin, reason) \
    controller_transaction_failed_fn((h), __func__, __LINE__, (tid), (ct), (dh), (devclose), (origin), (reason))

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif

int   controller_transaction_state_set(controller_transaction *ct, transaction_state state, transaction_result result);
int   transaction_devdata_add(clixon_handle h, controller_transaction *ct, char *name, cxobj *devdata, cbuf **cberr);
int   controller_transaction_notify(clixon_handle h, controller_transaction *ct);
int   controller_transaction_new(clixon_handle h, client_entry *ce, char *username, char *description, controller_transaction **ct, cbuf **cberr);
int   controller_transaction_free(clixon_handle h, controller_transaction *ct);
int   controller_transaction_free_all(clixon_handle h);
int   controller_transaction_done(clixon_handle h, controller_transaction *ct, transaction_result result);
controller_transaction *controller_transaction_find(clixon_handle h, const uint64_t id);
controller_transaction *controller_transaction_find_bystate(clixon_handle h, int neg, transaction_state state);
int   controller_transaction_nr_devices(clixon_handle h, uint64_t tid);
int   controller_transaction_failed_fn(clixon_handle h, const char *func, const int line,
                                       uint64_t tid, controller_transaction *ct, device_handle dh,
                                       tr_failed_devclose devclose, char *origin, char *reason);
int   controller_transaction_wait(clixon_handle h, uint64_t tid);
int   controller_transaction_wait_trigger(clixon_handle h, uint64_t tid, int commit);
int   controller_transaction_statedata(clixon_handle h, cvec *nsc, char *xpath, cxobj *xstate);
int   controller_transaction_periodic(clixon_handle h);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_TRANSACTION_H */
