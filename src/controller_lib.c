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
  * Common functions, for backend, cli, etc 
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

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"

/*! Mapping between enum transaction_state and yang transaction-state
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int tsmap[] = {
    {"INIT",      TS_INIT},
    {"RESOLVED",  TS_RESOLVED},
    {"DONE",      TS_DONE},
    {NULL,        -1}
};

/*! Mapping between enum transaction_result and yang transaction-result
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int trmap[] = {
    {"ERROR",   TR_ERROR},
    {"FAILED",  TR_FAILED},
    {"SUCCESS", TR_SUCCESS},
    {NULL,      -1}
};

/*! Mapping between enum device_config_type_t and yang device-config-type
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int dtmap[] = {
    {"RUNNING",   DT_RUNNING},
    {"CANDIDATE", DT_CANDIDATE},
    {"SYNCED",    DT_SYNCED},
    {"TRANSIENT", DT_TRANSIENT},
    {NULL,        -1}
};

/*! Mapping between enum push_type_t and yang push-type
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int ptmap[] = {
    {"NONE",     PT_NONE},
    {"VALIDATE", PT_VALIDATE},
    {"COMMIT",   PT_COMMIT},
    {NULL,       -1}
};

/*! Map controller transaction state from int to string 
 *
 * @param[in]  state  Transaction state as int
 * @retval     str    Transaction state as string
 */
char *
transaction_state_int2str(transaction_state state)
{
    return (char*)clicon_int2str(tsmap, state);
}

/*! Map controller transaction state from string to int 
 *
 * @param[in]  str    Transaction state as string
 * @retval     state  Transaction state as int
 */
transaction_state
transaction_state_str2int(char *str)
{
    return clicon_str2int(tsmap, str);
}

/*! Map controller transaction result from int to string 
 *
 * @param[in]  result Transaction result as int
 * @retval     str    Transaction result as string
 */
char *
transaction_result_int2str(transaction_result result)
{
    return (char*)clicon_int2str(trmap, result);
}

/*! Map controller transaction result from string to int 
 *
 * @param[in]  str    Transaction result as string
 * @retval     result Transaction result as int
 */
transaction_result
transaction_result_str2int(char *str)
{
    return clicon_str2int(trmap, str);
}

/*! Map device config type from int to string 
 *
 * @param[in]  typ    Device config type as int
 * @retval     str    Device config type as string
 */
char *
device_config_type_int2str(device_config_type t)
{
    return (char*)clicon_int2str(dtmap, t);
}

/*! Map device config type from string to int 
 *
 * @param[in]  str    Device config type as string
 * @retval     type   Device config type as int
 */
device_config_type
device_config_type_str2int(char *str)
{
    return clicon_str2int(dtmap, str);
}

/*! Map device push type from int to string 
 *
 * @param[in]  typ    Push type as int
 * @retval     str    Push type as string
 */
char *
push_type_int2str(push_type t)
{
    return (char*)clicon_int2str(ptmap, t);
}

/*! Map device push type from string to int 
 *
 * @param[in]  str    Push type as string
 * @retval     type   Push type as int
 */
push_type
push_type_str2int(char *str)
{
    return clicon_str2int(ptmap, str);
}

#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
/*! Rewrite of junos YANGs after parsing
 *
 * Add grouping command-forwarding in junos-rpc yangs if not exists
 * tried to make other less intrusive solutions or make a generic way in the
 * original function, but the easiest was just to rewrite the function.
 * @param[in] h       Clicon handle
 * @param[in] yanglib XML tree on the form <yang-lib>...
 * @param[in] yspec   Will be populated with YANGs, is consumed
 * @retval    1       OK
 * @retval    0       Parse error
 * @retval    -1      Error
 * @see yang_lib2yspec  the original function
 */
int
yang_lib2yspec_junos_patch(clicon_handle h,
                           cxobj        *yanglib,
                           yang_stmt    *yspec)
{
    int        retval = -1;
    cxobj     *xi;
    char      *name;
    char      *revision;
    cvec      *nsc = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;
    yang_stmt *ymod;
    yang_stmt *yrev;
    int        modmin = 0;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (xpath_vec(yanglib, nsc, "module-set/module", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xi = vec[i];
        if ((name = xml_find_body(xi, "name")) == NULL)
            continue;
        if ((revision = xml_find_body(xi, "revision")) == NULL)
            continue;
        if ((ymod = yang_find(yspec, Y_MODULE, name)) != NULL ||
            (ymod = yang_find(yspec, Y_SUBMODULE, name)) != NULL){
            /* Skip if matching or no revision 
             * Note this algorithm does not work for multiple revisions
             */
            if ((yrev = yang_find(ymod, Y_REVISION, NULL)) == NULL){
                modmin++;
                continue;
            }
            if (strcmp(yang_argument_get(yrev), revision) == 0){
                modmin++;
                continue;
            }
        }
        if (yang_parse_module(h, name, revision, yspec, NULL) == NULL)
            goto fail;
    }
    /* XXX: Ensure yang-lib is always there otherwise get state dont work for mountpoint */
    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-yang-library")) != NULL &&
        (yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL &&
        strcmp(yang_argument_get(yrev), "2019-01-04") == 0){
        modmin++;
    }
    else if (yang_parse_module(h, "ietf-yang-library", "2019-01-04", yspec, NULL) < 0)
        goto fail;
    clicon_debug(1, "%s yang_parse_post", __FUNCTION__);
    if (yang_parse_post(h, yspec, modmin) < 0)
        goto done;
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}
#endif  /* CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING */
