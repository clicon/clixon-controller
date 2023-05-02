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

  ***** END LICENSE BLOCK *****
  *
  * Meta-transaction for client/controller/device commit and rollback
  * The state transition is in the base case:
  *   OPEN->EDIT->VALIDATE->WAIT*->COMMIT->OPEN
  * where all devices must enter WAIT before any can proceed to COMMIT
  *
  * This leads to the following algorithm of when a device enters a EDIT/VALIDATE state:
  * 1. The device has failed
  * 1.1 The error is "recoverable" (eg validate fail) 
  * --> 1.1.1 Trigger DISCARD of the device
  * 1.2 The error is not recoverable,
  * --> 1.2.1 close the device
  * --> 1.2.2 Leave transaction
  * --> 1.2.3 If no devices left in transaction, mark as OK
  * 1.3 Further, if the transition is not in an error state
  * --> 1.3.1 Set transition in error state
  * If >= WAIT state
  * --> 1.3.2 For all other devices in WAIT state trigger DISCARD 
  *
  * 2. The device is OK (so far)
  * 2.1 The transaction is in an error state
  * If >= WAIT
  * --> 2.1.1 Trigger DISCARD of the device
  * 2.2 The transaction is OK
  * 2.2.1 IF IN VALIDATE:
  * 2.2.1.1 All devices are in WAIT (none are in EDIT/VALIDATE)
  * --> 2.2.1.1.1 Trigger COMMIT of all devices
  * 2.2.1.2 Some device are in EDIT/VALIDATE
  * --> 2.2.1.2.1 device proceed to next step
  * 2.2.2 IF ENTERING OK
  * --> 2.2.2.1 Leave transaction
  * --> 2.2.2.2 If no devices left in transaction, close transaction (if OK set result=true)
  *
  * There are also unrecoverable errors, such as errors in discard, or commit.
  * 1. A device fails in COMMIT/DISCARD
  *  --> the transition is set in an (unrecoverable) error state 
  *  --> All devices are closed?
  * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <assert.h>
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_device_state.h"
#include "controller_device_send.h"
#include "controller_device_handle.h"
#include "controller_transaction.h"

/*! Set new transaction state and timestamp
 *
 * @param[in]  ct     Transaction
 * @param[in]  state  New state
 * @param[in]  result New result (-1 is dont care, dont set)
 * @retval     0      OK
 */
int
controller_transaction_state_set(controller_transaction *ct,
                                 transaction_state       state,
                                 transaction_result      result)
{
    switch (state) {
    case TS_INIT:
        assert(ct->ct_state != TS_DONE);
        if (ct->ct_state != TS_INIT)
            clicon_debug(1, "%s %" PRIu64 " : %s -> %s",
                         __FUNCTION__,
                         ct->ct_id,
                         transaction_state_int2str(ct->ct_state),
                         transaction_state_int2str(state));
        else
            clicon_debug(1, "%s %" PRIu64 " : -> %s",
                         __FUNCTION__,
                         ct->ct_id,
                         transaction_state_int2str(state));
        break;
    case TS_ACTIONS:
        clicon_debug(1, "%s %" PRIu64 " : %s -> %s",
                     __FUNCTION__,
                     ct->ct_id,
                     transaction_state_int2str(ct->ct_state),
                     transaction_state_int2str(state));
        break;
    case TS_RESOLVED:
        assert(result != -1);
        assert(state != ct->ct_state);
        clicon_debug(1, "%s %" PRIu64 " : %s -> %s result: %s",
                     __FUNCTION__,
                     ct->ct_id,
                     transaction_state_int2str(ct->ct_state),
                     transaction_state_int2str(state),
                     transaction_result_int2str(result));
        break;
    case TS_DONE:
        assert(state != ct->ct_state);
        if (result != -1 && result != ct->ct_result)
            clicon_debug(1, "%s %" PRIu64 " : %s -> %s result: %s",
                         __FUNCTION__,
                         ct->ct_id,
                         transaction_state_int2str(ct->ct_state),
                         transaction_state_int2str(state),
                         transaction_result_int2str(result));
        else
            clicon_debug(1, "%s %" PRIu64 " : %s -> %s",
                         __FUNCTION__,
                         ct->ct_id,
                         transaction_state_int2str(ct->ct_state),
                         transaction_state_int2str(state));
    }
    ct->ct_state = state;
    if (result != -1 &&
        (state == TS_RESOLVED || state == TS_DONE))
        ct->ct_result = result;
    gettimeofday(&ct->ct_timestamp, NULL);
    return 0;
}

/*! A transaction has been completed
 *
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Controller transaction
 * @param[in]  result 0:error, 1:success
 */
int
controller_transaction_notify(clixon_handle           h,
                              controller_transaction *ct)
{
    int   retval = -1;
    cbuf *cb = NULL;

    clicon_debug(1, "%s %" PRIu64, __FUNCTION__, ct->ct_id);
    if (ct->ct_state == TS_INIT){
        clicon_err(OE_CFG, EINVAL, "Transaction notify sent in state INIT");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<controller-transaction xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%" PRIu64  "</tid>", ct->ct_id);
    cprintf(cb, "<result>%s</result>", transaction_result_int2str(ct->ct_result));
    if (ct->ct_origin)
        cprintf(cb, "<origin>%s</origin>", ct->ct_origin);
    if (ct->ct_reason){
        cprintf(cb, "<reason>");
        if (xml_chardata_cbuf_append(cb, ct->ct_reason) < 0)
            goto done;
        cprintf(cb, "</reason>");
    }
    cprintf(cb, "</controller-transaction>");
    if (stream_notify(h, "controller-transaction", "%s", cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Create new transaction id
 */
static int
transaction_new_id(clicon_handle h,
                   uint64_t     *idp)
{
    int      retval = -1;
    uint64_t id = 0;
    char    *idstr;
    char     idstr0[128];
    
    if (clicon_data_get(h, "controller-transaction-id", &idstr) < 0){
        id = 1; /* Dont start with 0, since 0 could mean unassigned */
    }
    else {
        if (parse_uint64(idstr, &id, NULL) <= 0)
            goto done;
    }
    if (idp)
        *idp = id;
    /* Increment for next access */
    id++;
    snprintf(idstr0, 128, "%" PRIu64, id);
    if (clicon_data_set(h, "controller-transaction-id", idstr0) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Create a new controller-transaction, with a new id and locl candidate
 *
 * Failure to create a transaction include:
 * - Candidate is locked
 * - Ongoing transaction, only one active transaction is allowed. 
 *   Actually should always be a sub-condition of the former condition
 * @param[in]   h           Clixon handle
 * @param[in]   description Description of transaction
 * @param[out]  ct          Transaction struct (if retval = 1)
 * @param[out]  reason      Reason for failure. Freed by caller
 * @retval      1           OK
 * @retval      0           Failed
 * @retval     -1           Error
 * @see controller_transaction_state_done
 */
int
controller_transaction_new(clicon_handle            h,
                           char                    *description,
                           controller_transaction **ctp,
                           cbuf                   **cberr)

{
    int                     retval = -1;
    controller_transaction *ct = NULL;
    controller_transaction *ct_list = NULL;
    size_t                  sz;
    uint32_t                iddb;
    char                   *db = "candidate";
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((iddb = xmldb_islocked(h, db)) != 0){
        if (cberr){
            if ((*cberr = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(*cberr, "Candidate db is locked by %u", iddb);
        }
        goto failed;
    }
    /* Exclusive lock of single active transaction */
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            if (ct->ct_state != TS_DONE){
                if (cberr){
                    if ((*cberr = cbuf_new()) == NULL){
                        clicon_err(OE_UNIX, errno, "cbuf_new");
                        goto done;
                    }
                    cprintf(*cberr, "Transaction %s is ongoing", ct->ct_description);
                }
                goto failed;
            }
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    sz = sizeof(controller_transaction);
    if ((ct = malloc(sz)) == NULL){
        clicon_err(OE_NETCONF, errno, "malloc");
        goto done;
    }
    memset(ct, 0, sz);
    ct->ct_h = h;
    if (transaction_new_id(h, &ct->ct_id) < 0)
        goto done;
    if (description &&
        (ct->ct_description = strdup(description)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    
    (void)clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list);
    ADDQ(ct, ct_list);
    clicon_ptr_set(h, "controller-transaction-list", (void*)ct_list);
    if (xmldb_lock(h, db, TRANSACTION_CLIENT_ID) < 0)
        goto done;
     /* user callback */
    if (clixon_plugin_lockdb_all(h, db, 1, TRANSACTION_CLIENT_ID) < 0)
        goto done;
    if (ctp)
        *ctp = ct;
    retval = 1;
 done:
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Remove and free single controller transaction
 */
int
controller_transaction_free(clicon_handle           h,
                            controller_transaction *ct)
{
    controller_transaction *ct_list = NULL;

    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0){
        DELQ(ct, ct_list, controller_transaction *);
    }
    if (ct->ct_description)
        free(ct->ct_description);
    if (ct->ct_origin)
        free(ct->ct_origin);
    if (ct->ct_reason)
        free(ct->ct_reason);
    if (ct->ct_sourcedb)
        free(ct->ct_sourcedb);
    free(ct);
    return 0;
}

/*! Terminate/close transaction and unlock candidate
 *
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Transaction
 * @retval     0      OK
 * @retval    -1      Error
 * @see controller_transaction_new
 */
int
controller_transaction_done(clicon_handle           h,
                            controller_transaction *ct,
                            transaction_result      result)
{
    int      retval = -1;
    uint32_t iddb;
    char    *db = "candidate";

    controller_transaction_state_set(ct, TS_DONE, result);
    iddb = xmldb_islocked(h, db);
    if (iddb != TRANSACTION_CLIENT_ID){
        clicon_err(OE_NETCONF, 0, "Unlock failed, not locked by transaction");
        goto done;
    }
    if (xmldb_unlock(h, db) < 0)
        goto done;
     /* user callback */
    if (clixon_plugin_lockdb_all(h, db, 0, TRANSACTION_CLIENT_ID) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Find clixon-client given name
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  id    Transaction id
 * @retval     ct    Transaction struct
 */
controller_transaction *
controller_transaction_find(clixon_handle  h,
                            const uint64_t id)
{
    controller_transaction *ct_list = NULL;
    controller_transaction *ct = NULL;
    
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            if (ct->ct_id == id)
                return ct;
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    return NULL;
}

/*! Return number of devices in a specific transaction
 *
 * @param[in]  h      Clixon handle
 * @param[in]  tid    Transaction id
 * @param[in]  state  In state (or -1 for any)
 * @retval     nr     Number of devices in transaction
 */
int
controller_transaction_devices(clicon_handle h,
                               uint64_t      tid)
{
    device_handle dh = NULL;
    int           nr = 0;

    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) == tid)
            nr++;
    }
    return nr;
}

/*! A controller transaction (device) has failed
 *
 * This device failed, ie validation has failed, the device lost connection, etc
  * 1.1 The error is "recoverable" (eg validate fail) 
  * --> 1.1.1 Trigger DISCARD of the device
  * 1.2 The error is not recoverable,
  * --> 1.2.1 close the device
  * --> 1.2.2 Leave transaction
  * --> 1.2.3 If no devices left in transaction, mark as OK
  * 1.3 Further, if the transition is not in an error state
  * --> 1.3.1 Set transition in error state
  * If >= WAIT state
  * --> 1.3.2 For all other devices in WAIT state trigger DISCARD 
 * @param[in]  h      Clixon handle
 * @param[in]  tid    Transaction id
 * @param[in]  dh     Device handler (or NULL)
 * @param[in]  devclose 0: dont close, 
 *                      1: dont close and leave transaction
 *                      2: close device and leave transaction
 * @param[in]  origin Originator of error
 * @param[in]  reason Reason for terminating transaction. If set && !recover -> close device
 * @retval     0      OK
 * @retval    -1      Error
 * @note devclose=0 means either that the device is already closed or is handled by the caller
 *       It also means that the caller must ensure the device leaves the transaction and marks it done if last
 */
int
controller_transaction_failed(clicon_handle           h,
                              uint64_t                tid,
                              controller_transaction *ct,                              
                              device_handle           dh,
                              int                     devclose,
                              char                   *origin,
                              char                   *reason)
{
    int retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    if (dh != NULL && devclose){
        if (devclose == 2){
            /* 1.2 The error is not recoverable */
            /* 1.2.1 close the device */
            if (device_close_connection(dh, "%s", reason) < 0)
                goto done;
        }
        /* 1.2.2 Leave transaction */
        device_handle_tid_set(dh, 0);
        /* 1.2.3 If no devices left in transaction, mark it as done */
        if (controller_transaction_devices(h, tid) == 0){
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
            if (origin && (ct->ct_origin = strdup(origin)) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (reason && (ct->ct_reason = strdup(reason)) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if (controller_transaction_notify(h, ct) < 0)
                goto done;
        }
    }
    if (ct->ct_state == TS_INIT || ct->ct_state == TS_ACTIONS){
        /* 1.3 The transition is not in an error state 
           1.3.1 Set transition in error state */
        controller_transaction_state_set(ct, TS_RESOLVED, TR_FAILED);
        if (origin && (ct->ct_origin = strdup(origin)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        if (reason && (ct->ct_reason = strdup(reason)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        if (controller_transaction_notify(h, ct) < 0)
            goto done;
        /* 1.3.2 For all other devices in WAIT state trigger DISCARD */
        if (controller_transaction_wait_trigger(h, tid, 0) < 0)
            goto done;
    }
    else if (ct->ct_result == TR_SUCCESS){
        clicon_err(OE_XML, 0, "Sanity: may not be in resolved OK state");
    }
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);    
    return retval;
}

/*! Check if all devices in a transaction is in wait case
 * 
 * @retval  1 All devices are in wait state
 * @retval  0 Not all devices are in wait state
 * @retval -1 Error: inconsistent states
 */
int
controller_transaction_wait(clicon_handle h,
                            uint64_t      tid)
{
    int           retval = -1;
    device_handle dh;
    int           notready = 0;
    int           wait = 0;
    int           other = 0;
    
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) != tid)
            continue;
        if (device_handle_conn_state_get(dh) == CS_PUSH_CHECK ||
            device_handle_conn_state_get(dh) == CS_PUSH_EDIT ||
            device_handle_conn_state_get(dh) == CS_PUSH_VALIDATE)
            notready++;
        if (device_handle_conn_state_get(dh) == CS_PUSH_WAIT)
            wait++;
        if (device_handle_conn_state_get(dh) != CS_PUSH_CHECK &&
            device_handle_conn_state_get(dh) != CS_PUSH_EDIT &&
            device_handle_conn_state_get(dh) != CS_PUSH_VALIDATE &&
            device_handle_conn_state_get(dh) != CS_PUSH_WAIT)
            other++;
    }
    if ((notready||wait) && other){
        clicon_err(OE_YANG, 0, "Inconsistent states: (notready||wait) && other");
        goto done;
    }
    if (wait && notready==0)
        retval = 1;
    else
        retval = 0;
 done:
    return retval;
}

/*! For all devices in WAIT state trigger commit or discard 
 *
 * @param[in]  h      Clixon handle
 * @param[in]  tid    Transaction id
 * @param[in]  commit 0: discard, 1: commit
 * @retval     0      OK
 * @retval    -1      Error
 */
int
controller_transaction_wait_trigger(clicon_handle h,
                                    uint64_t      tid,
                                    int           commit)
{
    int           retval = -1;
    device_handle dh = NULL;

    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) != tid)
            continue;
        if (device_handle_conn_state_get(dh) != CS_PUSH_WAIT)
            continue;
        if (commit){
            if (device_send_commit(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_COMMIT) < 0)
                goto done;
        }
        else{
            if (device_send_discard_changes(h, dh) < 0)
                goto done;
            if (device_state_set(dh, CS_PUSH_DISCARD) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Get transactions statedata
 *
 * @param[in]    h        Clicon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPATH syntax. or NULL for all
 * @param[out]   xstate   XML tree, <config/> on entry. 
 * @retval       0        OK
 * @retval      -1        Error
 */
int 
controller_transactions_statedata(clixon_handle   h, 
                                  cvec           *nsc,
                                  char           *xpath,
                                  cxobj          *xstate)
{
    int            retval = -1;
    cxobj        **vec = NULL;
    cxobj         *xret = NULL;
    cbuf          *cb = NULL;
    struct timeval *tv;
    controller_transaction *ct_list = NULL;
    controller_transaction *ct = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<transactions xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            cprintf(cb, "<transaction>");
            cprintf(cb, "<tid>%" PRIu64  "</tid>", ct->ct_id);
            cprintf(cb, "<state>%s</state>", transaction_state_int2str(ct->ct_state));
            if (ct->ct_description)
                cprintf(cb, "<description>%s</description>", ct->ct_description);
            if (ct->ct_origin)
                cprintf(cb, "<origin>%s</origin>", ct->ct_origin);
            if (ct->ct_reason){
                cprintf(cb, "<reason>");
                xml_chardata_cbuf_append(cb, ct->ct_reason);
                cprintf(cb, "</reason>");
            }
            if (ct->ct_state != TS_INIT)
                cprintf(cb, "<result>%s</result>", transaction_result_int2str(ct->ct_result));
            tv = &ct->ct_timestamp;
            if (tv->tv_sec != 0){
                char timestr[28];            
                if (time2str(*tv, timestr, sizeof(timestr)) < 0)
                    goto done;
                cprintf(cb, "<timestamp>%s</timestamp>", timestr);
            }
            cprintf(cb, "</transaction>");
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    cprintf(cb, "</transactions>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xstate, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}
