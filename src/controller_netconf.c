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
    if (clixon_proc_socket(h, argv, SOCK_DGRAM, pid, sock) < 0){
        goto done;
    }
    retval = 0;
 done:
    return retval;
}

static void
clixon_proc_sigint(int sig)
{
    /* XXX does nothing */
}

/*! Fork a child, exec a child and setup socket to child and return to caller 
 *
 * Derived from clixon_proc_socket with added stderr
 * @param[in]  h          Clixon handle
 * @param[in]  argv       NULL-terminated Argument vector
 * @param[in]  sock_flags Socket type/flags, typically SOCK_DGRAM or SOCK_STREAM, see
 * @param[out] pid        Process-id of child
 * @param[out] sock       Socket for stdin+stdout
 * @param[out] sockerr    Socket for stderr
 * @retval     O          OK
 * @retval    -1          Error.
 * @see clixon_proc_socket_close  close sockets, kill child and wait for child termination
 * @see for flags usage see man sockerpair(2)
 */
static int
clixon_proc_socket_stderr(clixon_handle h,
                          char        **argv,
                          int           sock_flags,
                          pid_t        *pid,
                          int          *sock,
                          int          *sockerr)
{
    int      retval = -1;
    int      sp[2] = {-1, -1};
    int      sperr[2] = {-1, -1};
    pid_t    child;
    sigfn_t  oldhandler = NULL;
    sigset_t oset;
    int      sig = 0;
    unsigned argc;
    char    *flattened;

    if (argv == NULL){
        clixon_err(OE_UNIX, EINVAL, "argv is NULL");
        goto done;
    }
    if (argv[0] == NULL){
        clixon_err(OE_UNIX, EINVAL, "argv[0] is NULL");
	goto done;
    }
    clixon_debug(CLIXON_DBG_PROC, "%s...", argv[0]);
    for (argc = 0; argv[argc] != NULL; ++argc)
         ;
    if ((flattened = clicon_strjoin(argc, argv, "', '")) == NULL){
        clixon_err(OE_UNIX, ENOMEM, "clicon_strjoin");
        goto done;
    }
    clixon_log(h, LOG_INFO, "%s '%s'", __FUNCTION__, flattened);
    free(flattened);

    if (socketpair(AF_UNIX, sock_flags, 0, sp) < 0){
        clixon_err(OE_UNIX, errno, "socketpair");
        goto done;
    }
    if (sockerr)
        if (socketpair(AF_UNIX, sock_flags, 0, sperr) < 0){
            clixon_err(OE_UNIX, errno, "socketpair");
            goto done;
        }

    sigprocmask(0, NULL, &oset);
    set_signal(SIGINT, clixon_proc_sigint, &oldhandler);
    sig++;
    if ((child = fork()) < 0) {
        clixon_err(OE_UNIX, errno, "fork");
        goto done;
    }
    if (child == 0) {   /* Child */
        /* Unblock all signals except TSTP */
        clicon_signal_unblock(0);
        signal(SIGTSTP, SIG_IGN);

        close(sp[0]);
        close(0);
        if (dup2(sp[1], STDIN_FILENO) < 0){
            clixon_err(OE_UNIX, errno, "dup2(STDIN)");
            return -1;
        }
        close(1);
        if (dup2(sp[1], STDOUT_FILENO) < 0){
            clixon_err(OE_UNIX, errno, "dup2(STDOUT)");
            return -1;
        }
        close(sp[1]);
        if (sockerr){
            close(2);
            if (dup2(sperr[1], STDERR_FILENO) < 0){
                clixon_err(OE_UNIX, errno, "dup2(STDERR)");
                return -1;
            }
            close(sperr[1]);
        }
        if (execvp(argv[0], argv) < 0){
            clixon_err(OE_UNIX, errno, "execvp(%s)", argv[0]);
            return -1;
        }
        exit(-1);        /* Shouldnt reach here */
    }

    clixon_debug(CLIXON_DBG_PROC | CLIXON_DBG_DETAIL, "child %u sock %d", child, sp[0]);
    /* Parent */
    close(sp[1]);
    *pid = child;
    *sock = sp[0];
    if (sockerr)
        *sockerr = sperr[0];
    retval = 0;
 done:
    if (sig){   /* Restore sigmask and fn */
        sigprocmask(SIG_SETMASK, &oset, NULL);
        set_signal(SIGINT, oldhandler, NULL);
    }
    clixon_debug(CLIXON_DBG_PROC, "retval:%d", retval);
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

    clixon_debug(1, "%s %s", __FUNCTION__, dest);
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
        clixon_debug(1, "%s: argv[%d]:%s", __FUNCTION__, i, argv[i]);
    if (clixon_proc_socket_stderr(h, argv, SOCK_STREAM, pid, sock, sockerr) < 0){
        goto done;
    }
    retval = 0;
 done:
    if (argv)
        free(argv);
    return retval;
}
