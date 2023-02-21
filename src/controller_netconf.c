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

/* Local includes */
#include "controller_netconf.h"

/*
 * Constants
 */
/* Netconf binary default, override with environment variable: CLIXON_NETCONF_BIN 
 * Could try to get path from install/makefile data
 */
#define CLIXON_NETCONF_BIN "/usr/local/bin/clixon_netconf"

/*! Connect to backend using internal netconf
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
    if (clicon_debug_get() != 0)
        nr += 2;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if ((netconf_bin = getenv("CLIXON_NETCONF_BIN")) == NULL)
        netconf_bin = CLIXON_NETCONF_BIN;
    if (stat(netconf_bin, &st) < 0){
        clicon_err(OE_NETCONF, errno, "netconf binary %s. Set with CLIXON_NETCONF_BIN=", 
                   netconf_bin);
        goto done;
    }
    argv[i++] = netconf_bin;
    argv[i++] = "-q";
    argv[i++] = "-f";
    argv[i++] = clicon_option_str(h, "CLICON_CONFIGFILE");
    argv[i++] = "-l"; /* log to syslog */
    argv[i++] = "s";
    if (clicon_debug_get() != 0){
        argv[i++] = "-D";
        snprintf(dbgstr, sizeof(dbgstr)-1, "%d", clicon_debug_get());
        argv[i++] = dbgstr;
    }
    argv[i++] = NULL;
    if (i !=nr){
        clicon_err(OE_NETCONF, 0, "argv mismatch, internal error");
        goto done;
    }
    if (clixon_proc_socket(argv, SOCK_DGRAM, pid, sock) < 0){
        goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Connect to backend using NETCONF over SSH
 */
int
clixon_client_connect_ssh(clixon_handle  h,
                          const char    *dest,
                          pid_t         *pid,
                          int           *sock)
{
    int         retval = -1;
    int         nr;
    int         i;
    char      **argv = NULL;
    char       *ssh_bin = SSH_BIN;
    struct stat st = {0,};

    clicon_debug(1, "%s %s", __FUNCTION__, dest);
    nr = 9;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if (stat(ssh_bin, &st) < 0){
        clicon_err(OE_NETCONF, errno, "ssh binary %s", ssh_bin);
        goto done;
    }
    argv[i++] = ssh_bin;
    argv[i++] = (char*)dest;
    argv[i++] = "-o";
    argv[i++] = "StrictHostKeyChecking=yes"; // dont ask
    argv[i++] = "-o";
    argv[i++] = "PasswordAuthentication=no"; // dont query
    argv[i++] = "-s";
    argv[i++] = "netconf";
    argv[i++] = NULL;
    if (i !=nr){
        clicon_err(OE_NETCONF, 0, "argv mismatch, internal error");
        goto done;
    }
    for (i=0;i<nr;i++)
        clicon_debug(1, "%s: argv[%d]:%s", __FUNCTION__, i, argv[i]);
    if (clixon_proc_socket(argv, SOCK_STREAM, pid, sock) < 0){
        goto done;
    }
    retval = 0;
 done:
    if (argv)
        free(argv);
    return retval;
}

/*! Process incoming frame, ie a char message framed by ]]>]]>
 * Parse string to xml, check only one netconf message within a frame
 * @param[in]   cb    Packet buffer
 * @param[in]   yspec Yang spec
 * @param[out]  xrecv XML packet
 * @retval      1     OK
 * @retval      0     Invalid, parse error, etc
 * @retval     -1     Fatal error
 */
int
netconf_input_frame(cbuf       *cb,
                    yang_stmt  *yspec,
                    cxobj     **xrecv)
{
    int     retval = -1;
    char   *str = NULL;
    cxobj  *xtop = NULL; /* Request (in) */
    cxobj  *xerr = NULL;
    int     ret;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if (xrecv == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xrecv is NULL");
        return -1;
    }
    //    clicon_debug(CLIXON_DBG_MSG, "%s: \"%s\"", __FUNCTION__, cbuf_get(cb));
    if ((str = strdup(cbuf_get(cb))) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }    
    if (strlen(str) != 0){
        if ((ret = clixon_xml_parse_string(str, YB_RPC, yspec, &xtop, &xerr)) < 0){
            goto fail;
        }
#if 0 // XXX only after schema mount and get-schema stuff
        else if (ret == 0){
            clicon_log(LOG_WARNING, "%s: YANG error", __FUNCTION__);
            goto fail;
        }
#endif
        else if (xml_child_nr_type(xtop, CX_ELMNT) == 0){
            clicon_log(LOG_WARNING, "%s: empty frame", __FUNCTION__);
            goto fail;
        }
        else if (xml_child_nr_type(xtop, CX_ELMNT) != 1){
            clicon_log(LOG_WARNING, "%s: multiple message in single frames", __FUNCTION__);
            goto fail;
        }
        else{
            *xrecv = xtop;
            xtop = NULL;
        }
    }
    retval = 1;
 done:
    if (xerr)
        xml_free(xerr);
    if (xtop)
        xml_free(xtop);
    if (str)
        free(str);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get netconf message: detect end-of-msg XXX could be moved to clixon_netconf_lib.c
 *
 * @param[in]     s           Socket where input arrives. Read from this.
 * @param[in,out] frame_state 
 * @param[in,out] frame_size 
 * @param[in,out] cb          Buffer
 * @param[out]    eom         If frame found in cb?
 * @param[out]    eof         socket closed / eof?
 * @retval        0           OK
 * @retval       -1           Error
 * This routine continuously reads until no more data on s. There could
 * be risk of starvation, but the netconf client does little else than
 * read data so I do not see a danger of true starvation here.
 * @note data is saved in clicon-handle at NETCONF_HASH_BUF since there is a potential issue if data
 * is not completely present on the s, ie if eg:
 *   <a>foo ..pause.. </a>]]>]]>
 * then only "</a>" would be delivered to netconf_input_frame().
 */
int
netconf_input_msg(int      s,
                  int      framing,
                  int     *frame_state,
                  size_t  *frame_size,
                  cbuf    *cb,
                  int     *eom,
                  int     *eof)
{
    int            retval = -1;
    unsigned char  buf[BUFSIZ]; /* from stdio.h, typically 8K */
    int            i;
    ssize_t        len;
    int            ret;
    int            found = 0;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    memset(buf, 0, sizeof(buf));
    while (1){
        clicon_debug(CLIXON_DBG_DETAIL, "%s read()", __FUNCTION__);
        if ((len = read(s, buf, sizeof(buf))) < 0){
            if (errno == ECONNRESET)
                len = 0; /* emulate EOF */
            else{
                clicon_log(LOG_ERR, "%s: read: %s", __FUNCTION__, strerror(errno));
                goto done;
            }
        } /* read */
        clicon_debug(CLIXON_DBG_DETAIL, "%s len:%ld", __FUNCTION__, len);
        if (len == 0){  /* EOF */
            clicon_debug(CLIXON_DBG_DETAIL, "%s len==0, closing", __FUNCTION__);
            *eof = 1;
        }
        else
            for (i=0; i<len; i++){
                if (buf[i] == 0)
                    continue; /* Skip NULL chars (eg from terminals) */
                if (framing == NETCONF_SSH_CHUNKED){
                    /* Track chunked framing defined in RFC6242 */
                    if ((ret = netconf_input_chunked_framing(buf[i], frame_state, frame_size)) < 0)
                        goto done;
                    switch (ret){
                    case 1: /* chunk-data */
                        cprintf(cb, "%c", buf[i]);
                        break;
                    case 2: /* end-of-data */
                        /* Somewhat complex error-handling:
                         * Ignore packet errors, UNLESS an explicit termination request (eof)
                         */
                        found++;
                        break;
                    default:
                        break;
                    }
                }
                else{
                    cprintf(cb, "%c", buf[i]);
                    if (detect_endtag("]]>]]>", buf[i], frame_state)){
                        *frame_state = 0;
                        /* OK, we have an xml string from a client */
                        /* Remove trailer */
                        *(((char*)cbuf_get(cb)) + cbuf_len(cb) - strlen("]]>]]>")) = '\0';
                        found++;
                        break;
                    }
                }
            }
#if 1
        break;
#else
        /* This is a way to keep reading, may be better for performance 
         * XXX No, maybe on single socket, otherwise it may starve other activities
         */
        if (found) /* frame found */
            break;
        {
            int poll;
            if ((poll = clixon_event_poll(s)) < 0)
                goto done;
            if (poll == 0){
                clicon_debug(CLIXON_DBG_DETAIL, "%s poll==0: no data on s", __FUNCTION__);
                break; 
            }
        }
#endif

    } /* while */
    *eom = found;
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DETAIL, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

