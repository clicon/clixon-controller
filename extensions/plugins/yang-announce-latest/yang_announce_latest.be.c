/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2026 Olof Hagsand

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
  * Add option for selecting which announced yang to choose
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
#include <sys/stat.h>

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
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_rpc.h"

/*! Transaction commit
 */
int
yang_announce_commit(clixon_handle    h,
                     transaction_data td)
{
    int            retval = -1;
    cxobj         *target;
    cxobj        **vec0 = NULL;
    size_t         veclen0;
    int            i;
    cxobj         *x;
    char          *devname;
    device_handle *dh;

    clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "");
    target = transaction_target(td); /* wanted XML tree */
    if (xpath_vec_flag(target, NULL, "devices/device/yang-announce-latest",
                       XML_FLAG_CHANGE,
                       &vec0, &veclen0) < 0)
        goto done;
    for (i=0; i<veclen0; i++){
        x = vec0[i];
        if ((devname = xml_find_body(xml_parent(x), "name")) != NULL){
            if ((dh = device_handle_find(h, devname)) != NULL){
                clixon_debug(CLIXON_DBG_CTRL, "device %s change yang-announce-latest flag to: %s", devname, xml_body(x));
                if (strcmp(xml_body(x), "true") == 0)
                    device_handle_flag_set(dh, DH_FLAG_YANG_ANNOUNCE_LATEST);
                else
                    device_handle_flag_reset(dh, DH_FLAG_YANG_ANNOUNCE_LATEST);
            }
        }
    }
    retval = 0;
 done:
    if (vec0)
        free(vec0);
    return retval;
}

/*! Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "yang-announce-latest",
#ifdef CLIXON_PLUGIN_USERDEF
    .ca_trans_commit = yang_announce_commit,
#endif
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    return &api;
}
