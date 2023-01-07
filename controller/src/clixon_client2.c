/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  *
  */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/stat.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

/* Controller includes */
#include "controller_custom.h"
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "clixon_client2.h"

/*
 * Constants
 */
#define CLIXON_CLIENT_MAGIC 0x54fe649a

#define chandle(ch) (assert(clixon_client_handle_check(ch)==0),(struct clixon_client2_handle *)(ch))

/*! Internal structure of clixon controller device handle. 
 */
struct clixon_client2_handle{
    qelem_t            ch_qelem;  /* List header */
    uint32_t           ch_magic;  /* Magic number */
    char              *ch_name;   /* Connection name */
    conn_state_t       ch_conn_state;  /* Connection state */
    struct timeval     ch_conn_time;  /* Time when entering last connection state */
    clicon_handle      ch_h;      /* Clixon handle */ 
    clixon_client_type ch_type;   /* Clixon socket type */
    int                ch_socket; /* Input/output socket, -1 is closed */
    int                ch_pid;    /* Sub-process-id Only applies for NETCONF/SSH */
    cbuf              *ch_frame_buf; /* Remaining expecting chunk bytes */
    int                ch_frame_state; /* Framing state for detecting EOM */
    size_t             ch_frame_size; /* Remaining expecting chunk bytes */
    cxobj             *ch_xcaps;      /* Capabilities as XML tree */
    struct timeval     ch_sync_time;  /* Time when last sync (0 if unsynched) */
    yang_stmt         *ch_yspec;      /* Top-level yang spec of device */
    int                ch_nr_schemas; /* How many schemas frm this device */
    char              *ch_logmsg;     /* Error log message / reason of failed open */
};

/*! Check struct magic number for sanity checks
 * @param[in]  h   Clicon client handle
 * @retval     0   Sanity check OK
 * @retval    -1   Sanity check failed
 */
static int
clixon_client_handle_check(clixon_client_handle ch)
{
    /* Dont use handle macro to avoid recursion */
    struct clixon_client2_handle *cch = (struct clixon_client2_handle *)(ch);

    return cch->ch_magic == CLIXON_CLIENT_MAGIC ? 0 : -1;
}

/*! Create client handle from clicon handle and add it to global list
 * @param[in]  h    Clixon  handle
 * @retval     ch   Client handle
 */
clixon_client_handle
clixon_client2_new(clicon_handle h,
                   const char   *name)
{
    struct clixon_client2_handle *cch = NULL;
    size_t                       sz = sizeof(struct clixon_client2_handle);
    struct clixon_client2_handle *ch_list = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((cch = malloc(sz)) == NULL){
        clicon_err(OE_NETCONF, errno, "malloc");
        return NULL;
    }
    memset(cch, 0, sz);
    cch->ch_magic = CLIXON_CLIENT_MAGIC;
    cch->ch_h = h;
    cch->ch_socket = -1;
    cch->ch_conn_state = CS_CLOSED;
    if ((cch->ch_name = strdup(name)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        return NULL;
    }
    if ((cch->ch_frame_buf = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        return NULL;
    }
    (void)clicon_ptr_get(h, "client-list", (void**)&ch_list);
    ADDQ(cch, ch_list);
    clicon_ptr_set(h, "client-list", (void*)ch_list);
    return cch;
}

/*! Free handle itself
 */
static int
clixon_client2_handle_free(struct clixon_client2_handle *ch)
{
    if (ch->ch_name)
        free(ch->ch_name);
    if (ch->ch_logmsg)
        free(ch->ch_logmsg);
    if (ch->ch_frame_buf)
        cbuf_free(ch->ch_frame_buf);
    if (ch->ch_xcaps)
        xml_free(ch->ch_xcaps);
    if (ch->ch_yspec)
        ys_free(ch->ch_yspec);
    free(ch);
    return 0;
}

/*! Free clixon client handle
 * @param[in]  ch   Client handle
 * @retval     0    OK
 */
int
clixon_client2_free(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);
    struct clixon_client2_handle *ch_list = NULL;
    struct clixon_client2_handle *c;
    clicon_handle                 h;

    h = (clicon_handle)cch->ch_h;
    clicon_ptr_get(h, "client-list", (void**)&ch_list);
    if ((c = ch_list) != NULL) {
        do {
            if (cch == c) {
                DELQ(c, ch_list, struct clixon_client2_handle *);
                clixon_client2_handle_free(c);
                break;
            }
            c = NEXTQ(struct clixon_client2_handle *, c);
        } while (c && c != ch_list);
    }
    clicon_ptr_set(h, "client-list", (void*)ch_list);
    return 0;
}

/*! Free all clixon client handles
 * @param[in]  h   Clixon handle
 */
int
clixon_client2_free_all(clixon_handle h)
{
    struct clixon_client2_handle *ch_list = NULL;
    struct clixon_client2_handle *c;
    
    clicon_ptr_get(h, "client-list", (void**)&ch_list);
    while ((c = ch_list) != NULL) {
        DELQ(c, ch_list, struct clixon_client2_handle *);
        clixon_client2_handle_free(c);
    }
    clicon_ptr_set(h, "client-list", (void*)ch_list);
    return 0;
}

/*! Find clixon-client given name
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  name  Client name
 * @retval     ch    Client handle
 */
clixon_client_handle 
clixon_client2_find(clicon_handle h,
                    const char   *name)
{
    struct clixon_client2_handle *ch_list = NULL;
    struct clixon_client2_handle *c = NULL;
    
    if (clicon_ptr_get(h, "client-list", (void**)&ch_list) == 0 &&
        (c = ch_list) != NULL) {
        do {
            if (strcmp(c->ch_name, name) == 0)
                return c;
            c = NEXTQ(struct clixon_client2_handle *, c);
        } while (c && c != ch_list);
    }
    return NULL;
}

/*! Connect client to clixon backend according to config and return a socket
 * @param[in]  h        Clixon handle
 * @param[in]  socktype Type of socket, internal/external/netconf/ssh
 * @param[in]  dest     Destination for some types
 * @retval     ch       Clixon session handler
 * @retval     NULL     Error
 * @see clixon_client_disconnect  Close the socket returned here
 */
int
clixon_client2_connect(clixon_client_handle ch,
                       clixon_client_type   socktype,
                       const char          *dest)
{
    int                          retval = -1;
    struct clixon_client2_handle *cch = (struct clixon_client2_handle *)ch;
    clicon_handle                h;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (cch == NULL){
        clicon_err(OE_XML, EINVAL, "ch is NULL");
        goto done;
    }
    h = cch->ch_h;
    cch->ch_type = socktype;
    switch (socktype){
    case CLIXON_CLIENT_IPC:
        if (clicon_rpc_connect(h, &cch->ch_socket) < 0)
            goto err;
        break;
    case CLIXON_CLIENT_NETCONF:
        if (clixon_client_connect_netconf(h, &cch->ch_pid, &cch->ch_socket) < 0)
            goto err;
        break;
#ifdef SSH_BIN
    case CLIXON_CLIENT_SSH:
        if (clixon_client_connect_ssh(h, dest, &cch->ch_pid, &cch->ch_socket) < 0)
            goto err;
#else
        clicon_err(OE_UNIX, 0, "No ssh bin");
        goto done;
#endif
        break;
    } /* switch */
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
 err:
    if (cch)
        clixon_client_disconnect(cch);
    goto done;
}

/*! Connect client to clixon backend according to config and return a socket
 * @param[in]    ch        Clixon client session handle
 * @see clixon_client_connect where the handle is created
 * The handle is deallocated
 */
int
clixon_client2_disconnect(clixon_client_handle ch)
{
    int   retval = -1;
    struct clixon_client2_handle *cch = chandle(ch);
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (cch == NULL){
        clicon_err(OE_XML, EINVAL, "Expected cch handle");
        goto done;
    }
    switch(cch->ch_type){
    case CLIXON_CLIENT_IPC:
        close(cch->ch_socket);
        cch->ch_socket = -1;
        break;
    case CLIXON_CLIENT_SSH:
    case CLIXON_CLIENT_NETCONF:
        if (clixon_proc_socket_close(cch->ch_pid,
                                     cch->ch_socket) < 0)
            goto done;
        cch->ch_pid = 0;
        cch->ch_socket = -1;
        break;
    }
    retval = 0;
 done:
    return retval;
}


/* Accessor functions ------------------------------
 */
/*! Get name of connection, allocated at creation time
 */
char*
clixon_client2_name_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_name;
}

/*! get socket
 * @param[in]  ch     Clixon client handle
 * @retval     s      Open socket
 * @retval    -1      No/closed socket
 */
int
clixon_client2_socket_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_socket;
}

clicon_handle
clixon_client2_handle_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_h;
}

/*! Get connection state
 * @param[in]  ch     Clixon client handle
 * @retval     state
 */
conn_state_t
clixon_client2_conn_state_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_conn_state;
}

/*! Set connection state also timestamp
 * @param[in]  ch     Clixon client handle
 * @retval     state  State
 * @retval     0      OK
 */
int
clixon_client2_conn_state_set(clixon_client_handle ch,
                              conn_state_t         state)
{
    struct clixon_client2_handle *cch = chandle(ch);

    clicon_debug(1, "%s %s: %s -> %s",
                 __FUNCTION__, cch->ch_name,
                 controller_state_int2str(cch->ch_conn_state),
                 controller_state_int2str(state));
    fprintf(stderr, "%s: %s -> %s\n",
            cch->ch_name,
            controller_state_int2str(cch->ch_conn_state),
            controller_state_int2str(state));

    cch->ch_conn_state = state;
    clixon_client2_conn_time_set(ch, NULL);
    return 0;
}

/*! Get connection timestamp
 * @param[in]  ch     Clixon client handle
 * @param[out] t      Connection timestamp
 */
int
clixon_client2_conn_time_get(clixon_client_handle ch,
                             struct timeval      *t)
{
    struct clixon_client2_handle *cch = chandle(ch);

    *t = cch->ch_conn_time;
    return 0;
}

/*! Set connection timestamp
 * @param[in]  ch     Clixon client handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
clixon_client2_conn_time_set(clixon_client_handle ch,
                             struct timeval      *t)
{
    struct clixon_client2_handle *cch = chandle(ch);
    if (t == NULL)
        gettimeofday(&cch->ch_conn_time, NULL);
    else
        cch->ch_conn_time = *t;
    return 0;
}

/*! Access frame state get 
 * @param[in]  ch     Clixon client handle
 * @retval     state
 */
int
clixon_client2_frame_state_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_frame_state;
}

/*! Access state get 
 * @param[in]  ch     Clixon client handle
 * @retval     state  State
 * @retval     0      OK
 */
int
clixon_client2_frame_state_set(clixon_client_handle ch,
                               int                  state)
{
    struct clixon_client2_handle *cch = chandle(ch);

    cch->ch_frame_state = state;
    return 0;
}

/*!
 * @param[in]  ch     Clixon client handle
 */
size_t
clixon_client2_frame_size_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_frame_size;
}

/*!
 * @param[in]  ch     Clixon client handle
 */
int
clixon_client2_frame_size_set(clixon_client_handle ch,
                              size_t               size)
{
    struct clixon_client2_handle *cch = chandle(ch);

    cch->ch_frame_size = size;
    return 0;
}

/*!
 * @param[in]  ch     Clixon client handle
 */
cbuf *
clixon_client2_frame_buf_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_frame_buf;
}

/*! Get capabilities as xml tree
 * @param[in]  ch     Clixon client handle
 * @retval     xcaps  XML tree
 */
cxobj *
clixon_client2_capabilities_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_xcaps;
}

/*! Set capabilities as xml tree
 * @param[in]  ch     Clixon client handle
 * @param[in]  xcaps  XML tree, is consumed
 * @retval     0      OK
 */
int
clixon_client2_capabilities_set(clixon_client_handle ch,
                                cxobj               *xcaps)
{
    struct clixon_client2_handle *cch = chandle(ch);

    if (cch->ch_xcaps != NULL)
        xml_free(cch->ch_xcaps);
    cch->ch_xcaps = xcaps;
    return 0;
}

/*! Query if capabaility exists on device
 *
 * @param[in]  ch    Clixon client handle
 * @param[in]  name  Capability name
 * @retval     1     Yes, capability exists
 * @retval     0     No, capabilty does not exist
 */
int
clixon_client2_capabilities_find(clicon_handle ch,
                                 const char   *name)
{
    struct clixon_client2_handle *cch = chandle(ch);
    cxobj                        *xcaps = NULL;
    cxobj                        *x = NULL;

    xcaps = cch->ch_xcaps;
    while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
        if (strcmp(name, xml_body(x)) == 0)
            break;
    }
    return x?1:0;
}

/*! Get sync timestamp
 * @param[in]  ch     Clixon client handle
 * @param[out] t      Sync timestamp (=0 if uninitialized)
 */
int
clixon_client2_sync_time_get(clixon_client_handle ch,
                             struct timeval      *t)
{
    struct clixon_client2_handle *cch = chandle(ch);

    *t = cch->ch_sync_time;
    return 0;
}

/*! Set sync timestamp
 * @param[in]  ch     Clixon client handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
clixon_client2_sync_time_set(clixon_client_handle ch,
                             struct timeval      *t)
{
    struct clixon_client2_handle *cch = chandle(ch);
    
    if (t == NULL)
        gettimeofday(&cch->ch_sync_time, NULL);
    else
        cch->ch_sync_time = *t;
    return 0;
}

/*! Get device-specific top-level yang spec
 * @param[in]  ch     Clixon client handle
 * @retval     yspec
 * @retval     NULL
 */
yang_stmt *
clixon_client2_yspec_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_yspec;
}

/*! Set device-specific top-level yang spec
 * @param[in]  ch     Clixon client handle
 * @param[in]  yspec
 */
int
clixon_client2_yspec_set(clixon_client_handle ch,
                          yang_stmt           *yspec)
{
    struct clixon_client2_handle *cch = chandle(ch);

    if (cch->ch_yspec)
        ys_free(cch->ch_yspec);
    cch->ch_yspec = yspec;
    return 0;
}

/*! Get nr of schemas
 * @param[in]  ch     Clixon client handle
 * @retval     nr
 */
int
clixon_client2_nr_schemas_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_nr_schemas;
}

/*! Set nr of schemas
 * @param[in]  ch   Clixon client handle
 * @param[in]  nr   Number of schemas  
 */
int
clixon_client2_nr_schemas_set(clixon_client_handle ch,
                              int                  nr)
{
    struct clixon_client2_handle *cch = chandle(ch);

    cch->ch_nr_schemas = nr;
    return 0;
}

/*! Get logmsg, direct pointer into struct
 * @param[in]  ch     Clixon client handle
 * @retval     logmsg
 * @retval     NULL
 */
char*
clixon_client2_logmsg_get(clixon_client_handle ch)
{
    struct clixon_client2_handle *cch = chandle(ch);

    return cch->ch_logmsg;
}

/*! Set logmsg, consume string
 * @param[in]  ch     Clixon client handle
 * @param[in]  logmsg Logmsg (is consumed)
 */
int
clixon_client2_logmsg_set(clixon_client_handle ch,
                          char                *logmsg)
{
    struct clixon_client2_handle *cch = chandle(ch);

    if (cch->ch_logmsg)
        free(cch->ch_logmsg);
    cch->ch_logmsg = logmsg;
    return 0;
}

