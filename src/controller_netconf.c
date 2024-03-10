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
  * try to move to clixon lib, not controller depends
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
#include <sys/stat.h>
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* Local includes (no controller dependencies) */
#include "controller_netconf.h"

/*
 * Constants
 */
/* Netconf binary default, override with environment variable: CLIXON_NETCONF_BIN
 * Could try to get path from install/makefile data
 */
#define CLIXON_NETCONF_BIN "/usr/local/bin/clixon_netconf"

/*! Connect using internal netconf
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  name  Client name
 * @retval     ch    Client handle
 */
int
clixon_client_connect_netconf(clixon_handle  h,
                              pid_t         *pid,
                              int           *sock)
{
    int         retval = -1;
    int         nr;
    int         i;
    char      **argv = NULL;
    char       *netconf_bin = NULL;
    struct stat st = {0,};
    char        dbgstr[8];

    nr = 7;
    if (clixon_debug_get() != 0)
        nr += 2;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if ((netconf_bin = getenv("CLIXON_NETCONF_BIN")) == NULL)
        netconf_bin = CLIXON_NETCONF_BIN;
    if (stat(netconf_bin, &st) < 0){
        clixon_err(OE_NETCONF, errno, "netconf binary %s. Set with CLIXON_NETCONF_BIN=",
                   netconf_bin);
        goto done;
    }
    argv[i++] = netconf_bin;
    argv[i++] = "-q";
    argv[i++] = "-f";
    argv[i++] = clicon_option_str(h, "CLICON_CONFIGFILE");
    argv[i++] = "-l"; /* log to syslog */
    argv[i++] = "s";
    if (clixon_debug_get() != 0){
        argv[i++] = "-D";
        snprintf(dbgstr, sizeof(dbgstr)-1, "%d", clixon_debug_get());
        argv[i++] = dbgstr;
    }
    argv[i++] = NULL;
    if (i !=nr){
        clixon_err(OE_NETCONF, 0, "argv mismatch, internal error");
        goto done;
    }
    if (clixon_proc_socket(h, argv, SOCK_DGRAM, pid, sock, NULL) < 0){
        goto done;
    }
    retval = 0;
 done:
    if (argv)
        free(argv);
    return retval;
}

/*! Connect using NETCONF over SSH
 *
 * @param[in]  h             Clixon  handle
 * @param[in]  dest          SSH destination
 * @param[in]  stricthostkey If set ensure strict hostkey checking. Only for ssh connections
 * @param[out] pid           Sub-process-id
 * @param[out] sock          Stdin/stdout socket
 * @param[out] sockerr       Stderr socket
 * @retval     0             OK
 * @retval    -1             Error
 */
int
clixon_client_connect_ssh(clixon_handle h,
                          const char   *dest,
                          int           stricthostkey,
                          pid_t        *pid,
                          int          *sock,
                          int          *sockerr)
{
    int         retval = -1;
    int         nr;
    int         i;
    char      **argv = NULL;
    char       *ssh_bin = SSH_BIN;
    struct stat st = {0,};

    clixon_debug(CLIXON_DBG_MSG|CLIXON_DBG_DETAIL, "%s %s", __FUNCTION__, dest);
    nr = 12;  /* NOTE this is hardcoded */
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if (stat(ssh_bin, &st) < 0){
        clixon_err(OE_NETCONF, errno, "ssh binary %s", ssh_bin);
        goto done;
    }
    argv[i++] = ssh_bin;
    argv[i++] = (char*)dest;
    argv[i++] = "-T"; /* Disable pseudo-terminal allocation. */
    argv[i++] = "-o";
    if (stricthostkey)
        argv[i++] = "StrictHostKeyChecking=yes";
    else
        argv[i++] = "StrictHostKeyChecking=no";
    argv[i++] = "-o";
    argv[i++] = "PasswordAuthentication=no"; // dont query
    argv[i++] = "-o";
    argv[i++] = "BatchMode=yes"; // user interaction disabled
    argv[i++] = "-s";
    argv[i++] = "netconf";
    argv[i++] = NULL;
    if (i !=nr){
        clixon_err(OE_NETCONF, 0, "argv mismatch, internal error");
        goto done;
    }
    for (i=0;i<nr;i++)
        clixon_debug(CLIXON_DBG_MSG|CLIXON_DBG_DETAIL, "%s: argv[%d]:%s", __FUNCTION__, i, argv[i]);
    if (clixon_proc_socket(h, argv, SOCK_STREAM, pid, sock, sockerr) < 0){
        goto done;
    }
    retval = 0;
 done:
    if (argv)
        free(argv);
    return retval;
}
