/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2025 Olof Hagsand

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
  * Main backend plugin file
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

/*! Called when application is "started", (almost) all initialization is complete
 *
 * Create a global transaction notification handler and socket
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int
controller_restconf_start(clixon_handle h)
{
    return 0;
}

/*! Called just before plugin unloaded.
 *
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int
controller_restconf_exit(clixon_handle h)
{
    return 0;
}

/*! Callback for printing version output and exit
 *
 * XXX unsure if this is ever called for restconf?
 */
int
controller_restconf_version(clixon_handle h,
                            FILE         *f)
{
    /* Assume clixon version already printed */
    fprintf(f, "Controller:\t%s\n", CONTROLLER_VERSION);
    fprintf(f, "Build:\t\t%s\n", CONTROLLER_BUILDSTR);
    return 0;
}

/*! Check device open, if not, yanglib data may not be available
 *
 * @param[in] h    Clixon handle
 * @param[in] name Device name
 * @retval    1    Device is open
 * @retval    0    Device is not open, or not found
 * @retval   -1    Error
 */
static int
device_check_open(clixon_handle h,
                  const char   *name)
{
    int    retval = -1;
    cvec  *nsc = NULL;
    cxobj *xret = NULL;
    cxobj *xerr;
    cxobj *xconn;

    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h,
                       "co:devices/co:device/co:name | co:devices/co:device/co:conn-state",
                       nsc, CONTENT_ALL, -1, "explicit", &xret) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get devices");
        goto done;
    }
    if ((xconn = xpath_first(xret, NULL, "devices/device[name='%s']/conn-state", name)) != NULL){
        if (strcmp(xml_body(xconn), "OPEN") == 0)
            goto open;
    }
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (nsc)
        cvec_free(nsc);
    return retval;
 open:
    retval = 1;
    goto done;
}

/*! Get yanglib from xpath and nsc of mountpoint
 *
 * @param[in]  h       Clixon handle
 * @param[in]  xpath   XPath of mountpoint
 * @param[in]  nsc     Namespace context of xpath
 * @param[out] xylib   New XML tree on the form <yang-library><module-set><module>*, caller frees
 * @retval     1       OK
 * @retval     0       No yanglib returned
 * @retval    -1       Error
 */
static int
controller_xpath2yanglib(clixon_handle h,
                         char         *xpath,
                         cvec         *nsc,
                         cxobj       **xylibp)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;
    cxobj *xerr;
    cxobj *xylib = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s", xpath);
    /* xpath to mount-point (to get config) */
    /* XXX: why not use /yanglib:yang-library directly ?
     * A: because you cannot get state-only (across mount-point?)
     */
    if (clixon_rpc_get1(h, cbuf_get(cb), nsc, CONTENT_ALL, -1, "explicit", YB_NONE, &xt) < 0)
        goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "clicon_rpc_get");
        goto done;
    }
    /* xpath to module-set */
    if (xml_nsctx_add(nsc, "yanglib", "urn:ietf:params:xml:ns:yang:ietf-yang-library") < 0)
        goto done;
    /* Append yang-library cb */
    cprintf(cb, "/yanglib:yang-library");
    if ((xylib = xpath_first(xt, nsc, "%s", cbuf_get(cb))) == NULL)
        goto skip;
    xml_rm(xylib);
    if (controller_yang_library_bind(h, xylib) < 0)
        goto done;
    *xylibp = xylib;
    xylib = NULL;
    retval = 1;
 done:
    if (xylib)
        xml_free(xylib);
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
    return retval;
 skip:
    retval = 0;
    goto done;
}

/*! YANG schema mount, query backend of yangs
 *
 * Given an XML mount-point xt, return XML yang-lib modules-set
 * Return yanglib as XML tree on the RFC8525 form:
 *   <yang-library>
 *      <module-set>
 *         <module>...</module>
 *         ...
 *      </module-set>
 *   </yang-library>
 * Get the schema-list for this device from the backend
 * @param[in]  h       Clixon handle
 * @param[in]  xmt     XML mount-point in XML tree
 * @param[out] config  If '0' all data nodes in the mounted schema are read-only
 * @param[out] vallevel Do or dont do full RFC 7950 validation
 * @param[out] yanglib XML yang-lib module-set tree. Freed by caller.
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC 8528 (schema-mount) and RFC 8525 (yang-lib)
 * @see device_send_get_schema_list/device_recv_schema_list Backend fns for send/rcv
 */
int
controller_restconf_yang_mount(clixon_handle   h,
                               cxobj          *xmt,
                               int            *config,
                               validate_level *vl,
                               cxobj         **xyanglib)
{
    int    retval = -1;
    char  *xpath = NULL;
    cvec  *nsc = NULL;
    cxobj *xname;
    char  *name;
    int    ret;

    if (xml_nsctx_node(xmt, &nsc) < 0)
        goto done;
    if (xml2xpath(xmt, nsc, 1, 1, &xpath) < 0)
        goto done;
    /* get modset */
    ret = controller_xpath2yanglib(h, xpath, nsc, xyanglib);
    if (ret < 0)
        goto done;
    if (ret == 0){ /* No xylib, give a reasonable error msg */
        if ((xname = xpath_first(xmt, NULL, "/devices/device/name")) != NULL ||
            (xname = xpath_first(xmt, NULL, "../name")) != NULL){
            name = xml_body(xname);
            if ((ret = device_check_open(h, name)) < 0)
                goto done;
            if (ret == 0){
                clixon_err(OE_YANG, 0, "Mountpoint operation on closed device %s", name);
            }
            else{
                clixon_err(OE_YANG, 0, "No yanglib from open device %s", name);
            }
        }
        else
            clixon_err(OE_YANG, 0, "No yanglib from device, unknown");
        goto done;
    }
    retval = 0;
 done:
    if (xpath)
        free(xpath);
    if (nsc)
        xml_nsctx_free(nsc);
    return retval;
}

/*! Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "controller restconf",
    .ca_start        = controller_restconf_start,
    .ca_exit         = controller_restconf_exit,
    .ca_yang_mount   = controller_restconf_yang_mount,
    .ca_version      = controller_restconf_version,
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    if (!clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        clixon_err(OE_YANG, 0, "The clixon controller requires CLICON_YANG_SCHEMA_MOUNT set to true");
        goto done;
    }
    return &api;
 done:
    return NULL;
}
