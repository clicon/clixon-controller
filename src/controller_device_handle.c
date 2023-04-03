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
  *
  * Device handle, hidden C struct and accessor functions
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
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"

/*
 * Constants
 */
#define CLIXON_CLIENT_MAGIC 0x54fe649a

#define devhandle(dh) (assert(device_handle_check(dh)==0),(struct controller_device_handle *)(dh))

/*! Internal structure of clixon controller device handle. 
 */
struct controller_device_handle{
    qelem_t            cdh_qelem;      /* List header */
    uint32_t           cdh_magic;      /* Magic number */
    char              *cdh_name;       /* Connection name */
    yang_config_t      cdh_yang_config; /* Yang config (shadow of config) */
    conn_state         cdh_conn_state; /* Connection state */
    struct timeval     cdh_conn_time;  /* Time when entering last connection state */
    clixon_handle      cdh_h;          /* Clixon handle */ 
    clixon_client_type cdh_type;       /* Clixon socket type */
    int                cdh_socket;     /* Input/output socket, -1 is closed */
    uint64_t           cdh_msg_id;     /* Client message-id to device */
    int                cdh_pid;        /* Sub-process-id Only applies for NETCONF/SSH */
    uint64_t           cdh_tid;        /* if >0, dev is part of transaction, 0 means unassigned */
    cbuf              *cdh_frame_buf;  /* Remaining expecting chunk bytes */
    int                cdh_frame_state;/* Framing state for detecting EOM */
    size_t             cdh_frame_size; /* Remaining expecting chunk bytes */
    cxobj             *cdh_xcaps;      /* Capabilities as XML tree */
    cxobj             *cdh_yang_lib;   /* RFC 8525 yang-library module list */
    struct timeval     cdh_sync_time;  /* Time when last sync (0 if unsynched) */
    yang_stmt         *cdh_yspec;      /* Top-level yang spec of device */
    int                cdh_nr_schemas; /* How many schemas from this device */
    char              *cdh_schema_name; /* Pending schema name */
    char              *cdh_schema_rev;  /* Pending schema revision */
    char              *cdh_logmsg;      /* Error log message / reason of failed open */
    cbuf              *cdh_outmsg;      /* Pending outgoing netconf message for delayed output */
};

/*! Check struct magic number for sanity checks
 * @param[in]  dh  Device handle
 * @retval     0   Sanity check OK
 * @retval    -1   Sanity check failed
 */
static int
device_handle_check(device_handle dh)
{
    /* Dont use handle macro to avoid recursion */
    struct controller_device_handle *cdh = (struct controller_device_handle *)(dh);

    return cdh->cdh_magic == CLIXON_CLIENT_MAGIC ? 0 : -1;
}

/*! Create new controller device handle given clixon handle and add it to global list
 * @param[in]  h    Clixon  handle
 * @retval     dh   Controller device handle
 */
device_handle
device_handle_new(clixon_handle h,
                  const char   *name)
{
    struct controller_device_handle *cdh = NULL;
    struct controller_device_handle *cdh_list = NULL;
    size_t                           sz;

    clicon_debug(1, "%s", __FUNCTION__);
    sz = sizeof(struct controller_device_handle);
    if ((cdh = malloc(sz)) == NULL){
        clicon_err(OE_NETCONF, errno, "malloc");
        return NULL;
    }
    memset(cdh, 0, sz);
    cdh->cdh_magic = CLIXON_CLIENT_MAGIC;
    cdh->cdh_h = h;
    cdh->cdh_socket = -1;
    cdh->cdh_conn_state = CS_CLOSED;
    if ((cdh->cdh_name = strdup(name)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        return NULL;
    }
    if ((cdh->cdh_frame_buf = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        return NULL;
    }
    (void)clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    ADDQ(cdh, cdh_list);
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return cdh;
}

/*! Free handle itself
 */
static int
device_handle_handle_free(struct controller_device_handle *cdh)
{
    if (cdh->cdh_name)
        free(cdh->cdh_name);
    if (cdh->cdh_frame_buf)
        cbuf_free(cdh->cdh_frame_buf);
    if (cdh->cdh_xcaps)
        xml_free(cdh->cdh_xcaps);
    if (cdh->cdh_yang_lib)
        xml_free(cdh->cdh_yang_lib);
#if 0 // XXX see xml_yang_mount_freeall
    if (cdh->cdh_yspec)
        ys_free(cdh->cdh_yspec);
#endif
    if (cdh->cdh_logmsg)
        free(cdh->cdh_logmsg);
    if (cdh->cdh_schema_name)
        free(cdh->cdh_schema_name);
    if (cdh->cdh_schema_rev)
        free(cdh->cdh_schema_rev);
    if (cdh->cdh_outmsg)
        cbuf_free(cdh->cdh_outmsg);
    free(cdh);
    return 0;
}

/*! Free controller device handle
 *
 * @param[in]  dh   Device handle
 * @retval     0    OK
 */
int
device_handle_free(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c;
    clixon_handle                    h;

    h = (clixon_handle)cdh->cdh_h;
    clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    if ((c = cdh_list) != NULL) {
        do {
            if (cdh == c) {
                DELQ(c, cdh_list, struct controller_device_handle *);
                device_handle_handle_free(c);
                break;
            }
            c = NEXTQ(struct controller_device_handle *, c);
        } while (c && c != cdh_list);
    }
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return 0;
}

/*! Free all controller's device handles
 * @param[in]  h   Clixon handle
 */
int
device_handle_free_all(clixon_handle h)
{
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c;
    
    clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    while ((c = cdh_list) != NULL) {
        DELQ(c, cdh_list, struct controller_device_handle *);
        device_handle_handle_free(c);
    }
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return 0;
}

/*! Find clixon-client given name
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  name  Client name
 * @retval     dh    Device handle
 */
device_handle 
device_handle_find(clixon_handle h,
                   const char   *name)
{
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c = NULL;
    
    if (clicon_ptr_get(h, "client-list", (void**)&cdh_list) == 0 &&
        (c = cdh_list) != NULL) {
        do {
            if (strcmp(c->cdh_name, name) == 0)
                return c;
            c = NEXTQ(struct controller_device_handle *, c);
        } while (c && c != cdh_list);
    }
    return NULL;
}

/*! Iterator over device-handles
 * @code
 *    device_handle dh = NULL;
 *    while ((dh = device_handle_each(h, dh)) != NULL){
 *       dh...
 * @endcode
 */
device_handle 
device_handle_each(clixon_handle h,
                   device_handle dhprev)
{
    struct controller_device_handle *cdh = (struct controller_device_handle *)dhprev;
    struct controller_device_handle *cdh0 = NULL;

    clicon_ptr_get(h, "client-list", (void**)&cdh0);
    if (cdh == NULL)
        return cdh0;
    cdh = NEXTQ(struct controller_device_handle *, cdh);
    if (cdh == cdh0)
        return NULL;
    else
        return cdh;
}

/*! Connect client to clixon backend according to config and return a socket
 * @param[in]  h        Clixon handle
 * @param[in]  socktype Type of socket, internal/external/netconf/ssh
 * @param[in]  dest     Destination for some types
 * @retval     dh       Clixon session handler
 * @retval     NULL     Error
 * @see clixon_client_disconnect  Close the socket returned here
 */
int
device_handle_connect(device_handle      dh,
                      clixon_client_type socktype,
                      const char        *dest)
{
    int                          retval = -1;
    struct controller_device_handle *cdh = (struct controller_device_handle *)dh;
    clixon_handle                h;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (cdh == NULL){
        clicon_err(OE_XML, EINVAL, "dh is NULL");
        goto done;
    }
    h = cdh->cdh_h;
    cdh->cdh_type = socktype;
    switch (socktype){
    case CLIXON_CLIENT_IPC:
        if (clicon_rpc_connect(h, &cdh->cdh_socket) < 0)
            goto err;
        break;
    case CLIXON_CLIENT_NETCONF:
        if (clixon_client_connect_netconf(h, &cdh->cdh_pid, &cdh->cdh_socket) < 0)
            goto err;
        break;
#ifdef SSH_BIN
    case CLIXON_CLIENT_SSH:
        if (clixon_client_connect_ssh(h, dest, &cdh->cdh_pid, &cdh->cdh_socket) < 0)
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
    if (cdh)
        clixon_client_disconnect(cdh);
    goto done;
}

/*! Connect client to clixon backend according to config and return a socket
 * @param[in]    dh        Clixon client session handle
 * @see clixon_client_connect where the handle is created
 * The handle is deallocated
 */
int
device_handle_disconnect(device_handle dh)
{
    int                              retval = -1;
    struct controller_device_handle *cdh = devhandle(dh);
    
    clicon_debug(1, "%s %s", __FUNCTION__, cdh->cdh_name);
    if (cdh == NULL){
        clicon_err(OE_XML, EINVAL, "Expected cdh handle");
        goto done;
    }
    switch(cdh->cdh_type){
    case CLIXON_CLIENT_IPC:
        close(cdh->cdh_socket);
        cdh->cdh_socket = -1;
        break;
    case CLIXON_CLIENT_SSH:
    case CLIXON_CLIENT_NETCONF:
        assert(cdh->cdh_pid && cdh->cdh_socket != -1);
        if (clixon_proc_socket_close(cdh->cdh_pid,
                                     cdh->cdh_socket) < 0)
            goto done;
        cdh->cdh_pid = 0;
        cdh->cdh_socket = -1;
        break;
    }
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}


/* Accessor functions ------------------------------
 */
/*! Get name of connection, allocated at creation time
 */
char*
device_handle_name_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_name;
}

/*! get socket
 * @param[in]  dh     Device handle
 * @retval     s      Open socket
 * @retval    -1      No/closed socket
 */
int
device_handle_socket_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_socket;
}

/*! Get msg-id and increment
 *
 * @param[in]  dh     Device handle
 * @retval     msgid
 */
uint64_t
device_handle_msg_id_getinc(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_msg_id++;
}

/*! Get transaction id
 *
 * @param[in]  dh     Device handle
 * @retval     tid    Transaction-id (0 means unassigned)
 */
uint64_t
device_handle_tid_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_tid;
}

/*! Get transaction id
 *
 * @param[in]  dh     Device handle
 * @param[in]  tid    Transaction-id (0 means unassigned)
 */
int
device_handle_tid_set(device_handle dh,
                      uint64_t      tid)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_tid = tid;
    return 0;
}

clixon_handle
device_handle_handle_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_h;
}

/*! Get yang config
 * @param[in]  dh          Device handle
 * @retval     yang-config How to bind device configuration to YANG
 * @note mirror of config
 */
yang_config_t
device_handle_yang_config_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_yang_config;
}

/*! Set yang config
 * @param[in]  dh     Device handle
 * @param[in]  yfstr  Yang config setting as string
 * @retval     0      OK
 * @note mirror of config, only commit callback code should set this value
 */
int
device_handle_yang_config_set(device_handle dh,
                              char         *yfstr)
{
    struct controller_device_handle *cdh = devhandle(dh);
    yang_config_t                    yf;

    yf = yang_config_str2int(yfstr);
    cdh->cdh_yang_config = yf;
    return 0;
}

/*! Get connection state
 * @param[in]  dh     Device handle
 * @retval     state
 */
conn_state
device_handle_conn_state_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_conn_state;
}

/*! Set connection state also timestamp
 * @param[in]  dh     Device handle
 * @param[in]  state  State
 * @retval     0      OK
 */
int
device_handle_conn_state_set(device_handle dh,
                             conn_state    state)
{
    struct controller_device_handle *cdh = devhandle(dh);

    assert(device_state_int2str(state)!=NULL);
    clicon_debug(1, "%s %s: %s -> %s",
                 __FUNCTION__,
                 device_handle_name_get(dh),
                 device_state_int2str(cdh->cdh_conn_state),
                 device_state_int2str(state));
    /* Free logmsg if leaving closed */
    if (cdh->cdh_conn_state == CS_CLOSED &&
        cdh->cdh_logmsg){
        free(cdh->cdh_logmsg);
        cdh->cdh_logmsg = NULL;
    }
    cdh->cdh_conn_state = state;
    device_handle_conn_time_set(dh, NULL);
    return 0;
}

/*! Get connection timestamp
 * @param[in]  dh     Device handle
 * @param[out] t      Connection timestamp
 */
int
device_handle_conn_time_get(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    *t = cdh->cdh_conn_time;
    return 0;
}

/*! Set connection timestamp
 * @param[in]  dh     Device handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
device_handle_conn_time_set(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (t == NULL)
        gettimeofday(&cdh->cdh_conn_time, NULL);
    else
        cdh->cdh_conn_time = *t;
    return 0;
}

/*! Access frame state get 
 * @param[in]  dh     Device handle
 * @retval     state
 */
int
device_handle_frame_state_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_state;
}

/*! Access state get 
 * @param[in]  dh     Device handle
 * @retval     state  State
 * @retval     0      OK
 */
int
device_handle_frame_state_set(device_handle dh,
                              int           state)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_frame_state = state;
    return 0;
}

/*!
 * @param[in]  dh     Device handle
 */
size_t
device_handle_frame_size_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_size;
}

/*!
 * @param[in]  dh     Device handle
 */
int
device_handle_frame_size_set(device_handle dh,
                             size_t        size)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_frame_size = size;
    return 0;
}

/*!
 * @param[in]  dh     Device handle
 */
cbuf *
device_handle_frame_buf_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_buf;
}

/*! Get capabilities as xml tree
 * @param[in]  dh     Device handle
 * @retval     xcaps  XML tree
 */
cxobj *
device_handle_capabilities_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_xcaps;
}

/*! Set capabilities as xml tree
 * @param[in]  dh     Device handle
 * @param[in]  xcaps  XML tree, is consumed
 * @retval     0      OK
 */
int
device_handle_capabilities_set(device_handle dh,
                               cxobj        *xcaps)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_xcaps != NULL)
        xml_free(cdh->cdh_xcaps);
    cdh->cdh_xcaps = xcaps;
    return 0;
}

/*! Query if capabaility exists on device
 *
 * @param[in]  dh    Device handle
 * @param[in]  name  Capability name
 * @retval     1     Yes, capability exists
 * @retval     0     No, capabilty does not exist
 */
int
device_handle_capabilities_find(clixon_handle dh,
                                const char   *name)
{
    struct controller_device_handle *cdh = devhandle(dh);
    cxobj                        *xcaps = NULL;
    cxobj                        *x = NULL;

    xcaps = cdh->cdh_xcaps;
    while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
        if (strcmp(name, xml_body(x)) == 0)
            break;
    }
    return x?1:0;
}

/*! Get RFC 8525 yang-lib as xml tree
 * @param[in]  dh     Device handle
 * @retval     yang_lib  XML tree
 * On the form: yang-library/module-set/name=<name>/module/name,revision,namespace  RFC 8525
 */
cxobj *
device_handle_yang_lib_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_yang_lib;
}

/*! Set RFC 8525 yang library as xml tree
 * @param[in]  dh      Device handle
 * @param[in]  yanglib XML tree, is consumed
 * @retval     0       OK
 */
int
device_handle_yang_lib_set(device_handle dh,
                           cxobj        *yang_lib)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_yang_lib != NULL)
        xml_free(cdh->cdh_yang_lib);
    cdh->cdh_yang_lib = yang_lib;
    return 0;
}

/*! Get sync timestamp
 * @param[in]  dh     Device handle
 * @param[out] t      Sync timestamp (=0 if uninitialized)
 */
int
device_handle_sync_time_get(device_handle    dh,
                             struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    *t = cdh->cdh_sync_time;
    return 0;
}

/*! Set sync timestamp
 * @param[in]  dh     Device handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
device_handle_sync_time_set(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);
    
    if (t == NULL)
        gettimeofday(&cdh->cdh_sync_time, NULL);
    else
        cdh->cdh_sync_time = *t;
    return 0;
}

/*! Get device-specific top-level yang spec
 * @param[in]  dh     Device handle
 * @retval     yspec
 * @retval     NULL
 */
yang_stmt *
device_handle_yspec_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_yspec;
}

/*! Set device-specific top-level yang spec
 * @param[in]  dh     Device handle
 * @param[in]  yspec
 */
int
device_handle_yspec_set(device_handle dh,
                        yang_stmt    *yspec)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_yspec)
        ys_free(cdh->cdh_yspec);
    cdh->cdh_yspec = yspec;
    return 0;
}

/*! Get nr of schemas
 * @param[in]  dh     Device handle
 * @retval     nr
 */
int
device_handle_nr_schemas_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_nr_schemas;
}

/*! Set nr of schemas
 * @param[in]  dh   Device handle
 * @param[in]  nr   Number of schemas  
 */
int
device_handle_nr_schemas_set(device_handle dh,
                             int           nr)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_nr_schemas = nr;
    return 0;
}

/*! Get pending schema name, strdup
 * @param[in]  dh     Device handle
 * @retval     schema-name
 * @retval     NULL
 */
char*
device_handle_schema_name_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_schema_name;
}

/*! Set pending schema name, strdup
 * @param[in]  dh     Device handle
 * @param[in]  schema-name Is copied
 */
int
device_handle_schema_name_set(device_handle dh,
                              char        *schema_name)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_schema_name)
        free(cdh->cdh_schema_name);
    cdh->cdh_schema_name = strdup(schema_name);
    return 0;
}

/*! Get pending schema rev, strdup
 * @param[in]  dh     Device handle
 * @retval     schema-rev
 * @retval     NULL
 */
char*
device_handle_schema_rev_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_schema_rev;
}

/*! Set pending schema rev, strdup
 * @param[in]  dh     Device handle
 * @param[in]  schema-rev Is copied
 */
int
device_handle_schema_rev_set(device_handle dh,
                              char        *schema_rev)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_schema_rev)
        free(cdh->cdh_schema_rev);
    cdh->cdh_schema_rev = strdup(schema_rev);
    return 0;
}

/*! Get logmsg, direct pointer into struct
 * @param[in]  dh     Device handle
 * @retval     logmsg
 * @retval     NULL
 */
char*
device_handle_logmsg_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_logmsg;
}

/*! Set logmsg, consume string
 * @param[in]  dh     Device handle
 * @param[in]  logmsg Logmsg (is consumed)
 */
int
device_handle_logmsg_set(device_handle dh,
                         char        *logmsg)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_logmsg)
        free(cdh->cdh_logmsg);
    cdh->cdh_logmsg = logmsg;
    return 0;
}

/*! Get pending netconf outmsg
 *
 * @param[in]  dh     Device handle
 * @retval     msg
 * @retval     NULL
 */
cbuf*
device_handle_outmsg_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_outmsg;
}

/*! Set pending netconf outmsg
 *
 * @param[in]  dh   Device handle
 * @param[in]  cb   Netconf msg
 */
int
device_handle_outmsg_set(device_handle dh,
                         cbuf         *cb)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_outmsg){
        cbuf_free(cdh->cdh_outmsg);
        cdh->cdh_outmsg = NULL;
    }
    cdh->cdh_outmsg = cb;
    return 0;
}


