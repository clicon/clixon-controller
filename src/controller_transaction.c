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
  * Transactions are created in the following places:
  * 1. In rpc_config_pull
  * 2. In rpc_controller_commit
  * 3. In rpc_connection_change
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
            clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : %s -> %s",
                         ct->ct_id,
                         transaction_state_int2str(ct->ct_state),
                         transaction_state_int2str(state));
        else
            clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : -> %s",
                         ct->ct_id,
                         transaction_state_int2str(state));
        break;
    case TS_ACTIONS:
        clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : %s -> %s",
                     ct->ct_id,
                     transaction_state_int2str(ct->ct_state),
                     transaction_state_int2str(state));
        break;
    case TS_RESOLVED:
        assert(result != -1);
        assert(state != ct->ct_state);
        clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : %s -> %s result: %s",
                     ct->ct_id,
                     transaction_state_int2str(ct->ct_state),
                     transaction_state_int2str(state),
                     transaction_result_int2str(result));
        break;
    case TS_DONE:
        assert(state != ct->ct_state);
        if (result != -1 && result != ct->ct_result)
            clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : %s -> %s result: %s",
                         ct->ct_id,
                         transaction_state_int2str(ct->ct_state),
                         transaction_state_int2str(state),
                         transaction_result_int2str(result));
        else
            clixon_debug(CLIXON_DBG_CTRL, "%" PRIu64 " : %s -> %s",
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

/*! Copy XML to transaction devdata field
 *
 * Add a new device element and children of devdata
 * see notification/device-data
 * @param[in]  ct      Controller transaction
 * @param[in]  name    Device name
 * @param[in]  xmsg    XML tree, copied
 * @param[out] cberr   Error, free with cbuf_err (retval = 0)
 * @retval     1       OK
 * @retval     0       Failed: error in cberr
 * @retval    -1       Error
 * @see  controller_transaction_notify where devdata is sent
 */
int
transaction_devdata_add(clixon_handle           h,
                        controller_transaction *ct,
                        char                   *name,
                        cxobj                  *xmsg,
                        cbuf                  **cberr)
{
    int        retval = -1;
    cxobj     *xdata;
    cxobj     *xc;
    cxobj     *xc1;
    yang_stmt *yspec0;
    yang_stmt *ydevs;
    cxobj     *xerr = NULL;
    cbuf      *cb = NULL;
    cvec      *nsc = NULL;
    int        ret;

    if (ct->ct_devdata == NULL){
        if ((ct->ct_devdata = xml_new("devices", NULL, CX_ELMNT)) == NULL)
            goto done;
        if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
            clixon_err(OE_FATAL, 0, "No DB_SPEC");
            goto done;
        }
        if (yang_abs_schema_nodeid(yspec0, "/ctrl:controller-transaction/devices", &ydevs) < 0)
            goto done;
        if (ydevs == NULL){
            if ((cb = cbuf_new()) == NULL){
                clixon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "No YANG found in rpc controller-transaction/devices");
            if (cberr){
                *cberr = cb;
                cb = NULL;
            }
            goto failed;
        }
        xml_spec_set(ct->ct_devdata, ydevs);
    }
    if ((ret = clixon_xml_parse_va(YB_PARENT, NULL, &ct->ct_devdata, &xerr,
                                   "<devdata xmlns=\"%s\"><name>%s</name><data/></devdata>",
                                   CONTROLLER_NAMESPACE, name)) < 0)
        goto done;
    if (ret == 0){
        if ((cb = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if (netconf_err2cb(h, xml_find_type(xerr, NULL, "rpc-error", CX_ELMNT), cb) < 0)
            goto done;
        if (cberr){
            *cberr = cb;
            cb = NULL;
        }
        goto failed;
    }
    if ((xdata = xpath_first(ct->ct_devdata, NULL, "devdata[name='%s']/data", name)) == NULL){
        clixon_err(OE_XML, 0, "devdata not found");
        goto done;
    }
    xc = NULL;
    while ((xc = xml_child_each(xmsg, xc, CX_ELMNT)) != NULL) {
        /* get all inherited namespaces and add them after dup and add to new parent */
        if (xml_nsctx_node(xc, &nsc) < 0)
            goto done;
        if ((xc1 = xml_dup(xc)) == NULL)
            goto done;
        if ((xml_addsub(xdata, xc1)) < 0)
            goto done;
        if (xmlns_set_all(xc1, nsc) < 0)
            goto done;
        if (nsc){
            cvec_free(nsc);
            nsc = NULL;
        }
    }
    retval = 1;
 done:
    if (nsc)
        cvec_free(nsc);
    if (cb)
        cbuf_free(cb);
    if (xerr)
        xml_free(xerr);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Grouping of handle and NETCONF notification message to use in event callback
 */
struct notify_async {
    clixon_handle na_h;
    cbuf         *na_cb;
};

/*! Send notification asynchronously
 *
 * @param[in]  fd   Dummy
 * @param[in]  arg  Cast to notify_async that holds handle netconf cbuf that needs freeing
 */
static int
transaction_notify_async(int   fd,
                         void *arg)
{
    int                  retval = -1;
    struct notify_async *na = (struct notify_async *)arg;

    if (stream_notify(na->na_h, "controller-transaction", "%s", cbuf_get(na->na_cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (na){
        if (na->na_cb)
            cbuf_free(na->na_cb);
        free(na);
    }
    return retval;
}

/*! A transaction has been completed
 *
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Controller transaction
 * @param[in]  result 0:error, 1:success
 * @retval     0      OK
 * @retval    -1      Error
 * @note  Extra logic to spawn notification message asynchronously
 */
int
controller_transaction_notify(clixon_handle           h,
                              controller_transaction *ct)
{
    int                  retval = -1;
    cbuf                *cb = NULL;
    struct timeval       t;
    struct notify_async *na = NULL;

    if (ct->ct_state == TS_INIT){
        clixon_err(OE_CFG, EINVAL, "Transaction notify sent in state INIT");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "tid:%" PRIu64 ", result:%s origin:%s reason:%s",
                 ct->ct_id,
                 transaction_result_int2str(ct->ct_result),
                 ct->ct_origin?ct->ct_origin:"",
                 ct->ct_reason?ct->ct_reason:"");
    cprintf(cb, "<controller-transaction xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%" PRIu64  "</tid>", ct->ct_id);
    if (ct->ct_username)
        cprintf(cb, "<username>%s</username>", ct->ct_username);
    cprintf(cb, "<result>%s</result>", transaction_result_int2str(ct->ct_result));
    if (ct->ct_origin)
        cprintf(cb, "<origin>%s</origin>", ct->ct_origin);
    if (ct->ct_reason){
        cprintf(cb, "<reason>");
        if (xml_chardata_cbuf_append(cb, 0, ct->ct_reason) < 0)
            goto done;
        cprintf(cb, "</reason>");
    }
    cprintf(cb, "</controller-transaction>");
    /* This is set in from-client rpc call, which means it is synchronous */
    if (clicon_data_int_get(h, "clixon-client-rpc") == 1){
        gettimeofday(&t, NULL);
        if ((na = malloc(sizeof(*na))) == NULL){
            clixon_err(OE_UNIX, errno, "malloc");
            goto done;
        }
        na->na_h = h;
        na->na_cb = cb;
        clixon_event_reg_timeout(t, transaction_notify_async, na, "transaction-notify");
        cb = NULL;
    }
    else if (stream_notify(h, "controller-transaction", "%s", cbuf_get(cb)) < 0)
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
transaction_new_id(clixon_handle h,
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
 * @param[in]   ce          Client/session entry
 * @param[in]   username    Which user created the transaction
 * @param[in]   description Description of transaction
 * @param[out]  ct          Transaction struct (if retval = 1)
 * @param[out]  reason      Reason for failure. Freed by caller
 * @retval      1           OK
 * @retval      0           Failed
 * @retval     -1           Error
 * @see controller_transaction_state_done
 */
int
controller_transaction_new(clixon_handle            h,
                           client_entry            *ce,
                           char                    *username,
                           char                    *description,
                           controller_transaction **ctp,
                           cbuf                   **cberr)
{
    int                     retval = -1;
    controller_transaction *ct = NULL;
    controller_transaction *ct0;
    client_entry           *ce1;
    controller_transaction *ct_list = NULL;
    size_t                  sz;
    uint32_t                iddb;
    uint32_t                lock_id;
    uint32_t                ceid;
    db_elmnt               *de = NULL;
    char                   *db = NULL;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if (ctp == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "ctp is NULL");
        goto done;
    }
    ceid = ce->ce_id;
    if (xmldb_find_create(h, "candidate", ceid, &de, &db) < 0)
        goto done;
    if (de == NULL){
        clixon_err(OE_DB, 0, "No candidate struct");
        goto done;
    }
    iddb = xmldb_islocked(h, db);
    /* If no lock create transaction lock else use existing lock */
    if (iddb == 0)
        lock_id = TRANSACTION_CLIENT_ID;
    else if (iddb != ceid || iddb == TRANSACTION_CLIENT_ID){
        assert(iddb != ceid);
        if ((*cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if (iddb == TRANSACTION_CLIENT_ID){
            if ((ct0 = controller_transaction_find_bystate(h, 1, TS_DONE)) != NULL){
                ce1 = NULL;
                if (ct0->ct_client_id &&
                    (ce1 = backend_client_find(h, ct0->ct_client_id)) != NULL){
                }
                cprintf(*cberr, "Candidate db is locked by transaction \"%s\" initiated by user:%s with client-id:%d",
                        ct0->ct_description, ct0->ct_username, ct0->ct_client_id);
                if (ce1 && ce1->ce_transport)
                    cprintf(*cberr, " using transport:%s", ce1->ce_transport);
            }
            else
                cprintf(*cberr, "Candidate db is locked by unknown transaction");
        }
        else{
            if ((ce1 = backend_client_find(h, iddb)) != NULL){
                cprintf(*cberr, "Candidate db is locked by user:%s with client-id:%u",
                        ce1->ce_username, iddb);
                if (ce1->ce_transport)
                    cprintf(*cberr, " using transport:%s", ce1->ce_transport);
            }
            else{
                cprintf(*cberr, "Candidate db is locked by %u", iddb);
            }
        }
        goto failed;
    }
    else
        lock_id = 0; /* Reuse existing lock */
    /* Exclusive lock of single active transaction */
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            if (ct->ct_state != TS_DONE){
                if ((*cberr = cbuf_new()) == NULL){
                    clixon_err(OE_UNIX, errno, "cbuf_new");
                    goto done;
                }
                cprintf(*cberr, "Transaction %s is ongoing", ct->ct_description);
                ct = NULL;
                goto failed;
            }
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    sz = sizeof(controller_transaction);
    if ((ct = malloc(sz)) == NULL){
        clixon_err(OE_NETCONF, errno, "malloc");
        goto done;
    }
    memset(ct, 0, sz);
    ct->ct_h = h;
    ct->ct_client_id = ceid;
    if (username) {
        if ((ct->ct_username = strdup(username)) == NULL){
            clixon_err(OE_NETCONF, errno, "strdup");
            goto done;
        }
    }
    if (transaction_new_id(h, &ct->ct_id) < 0)
        goto done;
    if (description &&
        (ct->ct_description = strdup(description)) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    (void)clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list);
    ADDQ(ct, ct_list);
    clicon_ptr_set(h, "controller-transaction-list", (void*)ct_list);
    if (lock_id) {
        if (xmldb_lock(h, db, lock_id) < 0)
            goto done;
        /* user callback */
        if (clixon_plugin_lockdb_all(h, db, 1, lock_id) < 0)
            goto done;
    }
    *ctp = ct;
    ct = NULL;
    retval = 1;
 done:
    if (ct)
        free(ct);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Free transaction itself
 */
static int
controller_transaction_free1(controller_transaction *ct)
{
    if (ct->ct_username)
        free(ct->ct_username);
    if (ct->ct_description)
        free(ct->ct_description);
    if (ct->ct_origin)
        free(ct->ct_origin);
    if (ct->ct_reason)
        free(ct->ct_reason);
    if (ct->ct_warning)
        free(ct->ct_warning);
    if (ct->ct_sourcedb)
        free(ct->ct_sourcedb);
    if (ct->ct_devdata)
        xml_free(ct->ct_devdata);
    free(ct);
    return 0;
}

/*! Remove and free single controller transaction
 */
int
controller_transaction_free(clixon_handle           h,
                            controller_transaction *ct)
{
    controller_transaction *ct_list = NULL;

    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0){
        DELQ(ct, ct_list, controller_transaction *);
    }
    return controller_transaction_free1(ct);
}

/*! Free all controller transactions
 *
 * @param[in]  h   Clixon handle
 */
int
controller_transaction_free_all(clixon_handle h)
{
    controller_transaction *ct_list = NULL;
    controller_transaction *ct;

    clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list);
    while ((ct = ct_list) != NULL) {
        DELQ(ct, ct_list, controller_transaction *);
        controller_transaction_free1(ct);
    }
    clicon_ptr_set(h, "controller-transaction-list", (void*)ct_list);
    return 0;
}

/*! Terminate/close transaction, unlock candidate, unmark all devices and notify
 *
 * @param[in]  h      Clixon handle
 * @param[in]  ct     Transaction
 * @param[in]  result Can be -1 for already set
 * @retval     0      OK
 * @retval    -1      Error
 * @see controller_transaction_new
 */
int
controller_transaction_done(clixon_handle           h,
                            controller_transaction *ct,
                            transaction_result      result)
{
    int           retval = -1;
    uint32_t      iddb;
    char         *db;
    device_handle dh;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    controller_transaction_state_set(ct, TS_DONE, result);
    if (xmldb_candidate_find(h, "candidate", ct->ct_client_id, NULL, &db) < 0)
        goto done;
    if (db == NULL){
        clixon_debug(CLIXON_DBG_CTRL, "Candidate not found on close");
    }
    else {
        iddb = xmldb_islocked(h, db);
        if (iddb == TRANSACTION_CLIENT_ID){
            if (xmldb_unlock(h, db) < 0)
                goto done;
            /* user callback */
            if (clixon_plugin_lockdb_all(h, db, 0, TRANSACTION_CLIENT_ID) < 0)
                goto done;
        }
    }
    /* Unmark all devices */
    dh = NULL;
    while ((dh = device_handle_each(h, dh)) != NULL){
        if (device_handle_tid_get(dh) == ct->ct_id)
            device_handle_tid_set(dh, 0);
        device_handle_outmsg_set(dh, 1, NULL);
        device_handle_outmsg_set(dh, 2, NULL);
    }
    /* This should be the only place */
    if (controller_transaction_notify(h, ct) < 0)
        goto done;
#if 0
    /* This will leave devdata in the transaction history.
     * Pros: You can check device rpc results via show state, not only immediate in the
     *       transaction-done notification
     * Cons: Memory consumption, never freed, clutters history
     */
    if (ct->ct_devdata){
        xml_free(ct->ct_devdata); // XXX free at close?
        ct->ct_devdata = NULL;
    }
#endif
    retval = 0;
 done:
    return retval;
}

/*! Find controller transaction given id
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  id    Transaction id
 * @retval     ct    Transaction struct
 * @retval     NULL  Not found
 * @see controller_transaction_find_bystate
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

/*! Find frst controller transaction with given state
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  neg   If set find first that is not in this state
 * @param[in]  state Transaction state
 * @retval     ct    Transaction struct
 * @retval     NULL  Not found
 */
controller_transaction *
controller_transaction_find_bystate(clixon_handle     h,
                                    int               neg,
                                    transaction_state state)
{
    controller_transaction *ct_list = NULL;
    controller_transaction *ct = NULL;

    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            if (neg){
                if (ct->ct_state != state)
                    break;
            }
            else if (ct->ct_state == state)
                break;
            ct = NEXTQ(controller_transaction *, ct);
        } while (ct && ct != ct_list);
    }
    return ct;
}

/*! Return number of devices in a specific transaction
 *
 * @param[in]  h      Clixon handle
 * @param[in]  tid    Transaction id
 * @param[in]  state  In state (or -1 for any)
 * @retval     nr     Number of devices in transaction
 */
int
controller_transaction_nr_devices(clixon_handle h,
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
 * @param[in]  func   Inline function name
 * @param[in]  line   Inline file line number
 * @param[in]  tid    Transaction id
 * @param[in]  dh     Device handler (or NULL)
 * @param[in]  devclose 0: dont close, (If dh is set)
 *                      1: dont close and leave transaction
 *                      2: close device and leave transaction
 * @param[in]  origin Originator of error
 * @param[in]  reason Reason for terminating transaction. If set && !recover -> close device
 * @retval     0      OK
 * @retval    -1      Error
 */
int
controller_transaction_failed_fn(clixon_handle           h,
                                 const char             *func,
                                 const int               line,
                                 uint64_t                tid,
                                 controller_transaction *ct,
                                 device_handle           dh,
                                 tr_failed_devclose      devclose,
                                 char                   *origin,
                                 char                   *reason)
{
    int retval = -1;

    if (ct == NULL){
        device_close_connection(dh, "Device not associated with transaction");
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "%s:%d tid:%" PRIu64 ", ct-state:%s, device:%s, devclose:%d, origin:%s, reason:%s",
                 func, line,
                 tid,
                 transaction_state_int2str(ct->ct_state),
                 dh?device_handle_name_get(dh):"NULL",
                 devclose,
                 origin?origin:"NULL",
                 reason?reason:"NULL");
    if (dh != NULL && devclose != TR_FAILED_DEV_IGNORE){
        if (devclose == TR_FAILED_DEV_CLOSE){
            /* 1.2 The error is not recoverable */
            /* 1.2.1 close the device */
            if (device_close_connection(dh, "%s", reason) < 0)
                goto done;
        }
        /* 1.2.2 Leave transaction */
        device_handle_tid_set(dh, 0);
        /* 1.2.3 If no devices left in transaction, mark it as done */
        if (controller_transaction_nr_devices(h, tid) == 0){
            if (origin && ct->ct_origin == NULL){
                if ((ct->ct_origin = strdup(origin)) == NULL){
                    clixon_err(OE_UNIX, errno, "strdup");
                    goto done;
                }
            }
            if (reason && ct->ct_reason == NULL){
                if ((ct->ct_reason = strdup(reason)) == NULL){
                    clixon_err(OE_UNIX, errno, "strdup");
                    goto done;
                }
            }
            if (controller_transaction_done(h, ct, TR_FAILED) < 0)
                goto done;
        }
    }
    if (ct->ct_state == TS_INIT || ct->ct_state == TS_ACTIONS){
        /* 1.3 The transition is not in an error state
           1.3.1 Set transition in error state */
        controller_transaction_state_set(ct, TS_RESOLVED, TR_FAILED);
        if (origin && ct->ct_origin == NULL) {
            if ((ct->ct_origin = strdup(origin)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
        }
        if (reason && ct->ct_reason == NULL) {
            if ((ct->ct_reason = strdup(reason)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
        }
        /* 1.3.2 For all other devices in WAIT state trigger DISCARD */
        if (controller_transaction_wait_trigger(h, tid, 0) < 0)
            goto done;
    }
    else if (ct->ct_result == TR_SUCCESS){
        clixon_err(OE_XML, 0, "Sanity: may not be in resolved OK state");
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "retval:%d", retval);
    return retval;
}

/*! Check if all devices in a transaction is in wait case
 *
 * @retval  1 All devices are in wait state
 * @retval  0 Not all devices are in wait state
 * @retval -1 Error: inconsistent states
 */
int
controller_transaction_wait(clixon_handle h,
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
        if (
            device_handle_conn_state_get(dh) == CS_PUSH_LOCK ||
            device_handle_conn_state_get(dh) == CS_PUSH_CHECK ||
            device_handle_conn_state_get(dh) == CS_PUSH_EDIT ||
            device_handle_conn_state_get(dh) == CS_PUSH_EDIT2 ||
            device_handle_conn_state_get(dh) == CS_PUSH_VALIDATE)
            notready++;
        else if (device_handle_conn_state_get(dh) == CS_PUSH_WAIT)
            wait++;
        else
            other++;
    }
    if ((notready||wait) && other){
        clixon_err(OE_YANG, 0, "Inconsistent states: (notready||wait) && other");
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
controller_transaction_wait_trigger(clixon_handle h,
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
 * @param[in]    h        Clixon handle
 * @param[in]    nsc      External XML namespace context, or NULL
 * @param[in]    xpath    String with XPath syntax. or NULL for all
 * @param[out]   xstate   XML tree, <config/> on entry.
 * @retval       0        OK
 * @retval      -1        Error
 */
int
controller_transaction_statedata(clixon_handle   h,
                                 cvec           *nsc,
                                 char           *xpath,
                                 cxobj          *xstate)
{
    int                     retval = -1;
    cbuf                   *cb = NULL;
    struct timeval         *tv;
    controller_transaction *ct_list = NULL;
    controller_transaction *ct = NULL;

    clixon_debug(CLIXON_DBG_CTRL|CLIXON_DBG_DETAIL, "");
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<transactions xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        do {
            cprintf(cb, "<transaction>");
            cprintf(cb, "<tid>%" PRIu64  "</tid>", ct->ct_id);
            if (ct->ct_state != TS_INIT)
                cprintf(cb, "<result>%s</result>", transaction_result_int2str(ct->ct_result));
            if (ct->ct_username){
                cprintf(cb, "<username>");
                xml_chardata_cbuf_append(cb, 0, ct->ct_username);
                cprintf(cb, "</username>");
            }
            if (ct->ct_reason){
                cprintf(cb, "<reason>");
                xml_chardata_cbuf_append(cb, 0, ct->ct_reason);
                cprintf(cb, "</reason>");
            }
            if (ct->ct_devdata){
                cprintf(cb, "<devices>");
                if (clixon_xml2cbuf1(cb, ct->ct_devdata, 0, 0, NULL, -1, 1, 0) < 0)
                    goto done;
                cprintf(cb, "</devices>");
            }
            cprintf(cb, "<state>%s</state>", transaction_state_int2str(ct->ct_state));
            if (ct->ct_description)
                cprintf(cb, "<description>%s</description>", ct->ct_description);
            if (ct->ct_origin)
                cprintf(cb, "<origin>%s</origin>", ct->ct_origin);

            if (ct->ct_warning){
                cprintf(cb, "<warning>");
                xml_chardata_cbuf_append(cb, 0, ct->ct_warning);
                cprintf(cb, "</warning>");
            }
            tv = &ct->ct_timestamp;
            if (tv->tv_sec != 0){
                char timestr[28];
                if (time2str(tv, timestr, sizeof(timestr)) < 0)
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
    return retval;
}

/*! Transactions periodic handler,
 *
 * Remove device data after TRANSACTION_DEVICES_TIMEOUT ( these can be very large)
 * Save at most TRANSACTION_MAX_NR
 * @param[in]  h   Clixon  handle
 * @retval     0   OK
 * @retval    -1   Error

 */
int
controller_transaction_periodic(clixon_handle  h)
{
    controller_transaction *ct_list = NULL;
    controller_transaction *ct = NULL;
    controller_transaction *ct1;
    struct timeval          t0;
    struct timeval          t;
    int                     i;

    gettimeofday(&t0, NULL);
    if (clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list) == 0 &&
        (ct = ct_list) != NULL) {
        ct = PREVQ(controller_transaction *, ct);
        i = 0;
        do {
            ct1 = NULL;
            timersub(&t0, &ct->ct_timestamp, &t);
            if (TRANSACTION_MAX_NR && i > TRANSACTION_MAX_NR-1){
                ct1 = ct;  /* delete later */
            }
            else if (TRANSACTION_DEVICES_TIMEOUT && ct->ct_devdata && t.tv_sec > TRANSACTION_DEVICES_TIMEOUT){
                xml_free(ct->ct_devdata);
                ct->ct_devdata = NULL;
            }
            ct = PREVQ(controller_transaction *, ct);
            i++;
            if (ct1){
                controller_transaction_free(h, ct1);
            }
        } while (ct && ct != ct_list);
    }
    return 0;
}
