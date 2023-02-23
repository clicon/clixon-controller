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
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller.h"
#include "controller_transaction.h"

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
        id = 0;
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

/*! Create a new controller-transaction, with a new id
 *
 * @param[in]   h   Clixon handle
 * @param[out]  ct  Transaction struct
 */
int
controller_transaction_new(clicon_handle            h,
                           controller_transaction **ctp)

{
    int                     retval = -1;
    controller_transaction *ct = NULL;
    controller_transaction *ct_list = NULL;
    size_t                  sz;
    
    clicon_debug(1, "%s", __FUNCTION__);
    sz = sizeof(controller_transaction);
    if ((ct = malloc(sz)) == NULL){
        clicon_err(OE_NETCONF, errno, "malloc");
        goto done;
    }
    memset(ct, 0, sz);
    if (transaction_new_id(h, &ct->ct_id) < 0)
        goto done;
    (void)clicon_ptr_get(h, "controller-transaction-list", (void**)&ct_list);
    ADDQ(ct, ct_list);
    clicon_ptr_set(h, "controller-transaction-list", (void*)ct_list);
    if (ctp)
        *ctp = ct;
    retval = 0;
 done:
    return retval;
}

#ifdef NOTYET
/*! Free controller transaction itself
 */
static int
controller_transaction_free(controller_transaction *ct)
{
    if (th->th_origin)
        free(th->th_origin);
    free(th);
    return 0;
}
#endif
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
