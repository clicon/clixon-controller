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
 *
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int tsmap[] = {
    {"INIT",      TS_INIT},
    {"ACTIONS",   TS_ACTIONS},
    {"RESOLVED",  TS_RESOLVED},
    {"DONE",      TS_DONE},
    {NULL,        -1}
};

/*! Mapping between enum transaction_result and yang transaction-result
 *
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int trmap[] = {
    {"INIT",    TR_INIT},
    {"ERROR",   TR_ERROR},
    {"FAILED",  TR_FAILED},
    {"SUCCESS", TR_SUCCESS},
    {NULL,      -1}
};

/*! Mapping between enum device_config_type_t and yang device-config-type
 *
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int dtmap[] = {
    {"RUNNING",   DT_RUNNING},
    {"CANDIDATE", DT_CANDIDATE},
    {"ACTIONS",   DT_ACTIONS},
    {"SYNCED",    DT_SYNCED},
    {"TRANSIENT", DT_TRANSIENT},
    {NULL,       -1}
};

/*! Mapping between enum push_type_t and yang push-type
 *
 * @see clixon-controller@2023-01-01.yang
 */
static const map_str2int ptmap[] = {
    {"NONE",     PT_NONE},
    {"VALIDATE", PT_VALIDATE},
    {"COMMIT",   PT_COMMIT},
    {NULL,       -1}
};

/*! Mapping between enum actions_type_t and yang actions-type
 *
 * @see clixon-controller.yang
 */
static const map_str2int atmap[] = {
    {"NONE",    AT_NONE},
    {"CHANGE",  AT_CHANGE},
    {"FORCE",   AT_FORCE},
    {"DELETE",  AT_DELETE},
    {NULL,     -1}
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

/*! Map actions type from int to string
 *
 * @param[in]  typ    Actions type as int
 * @retval     str    Actions type as string
 */
char *
actions_type_int2str(actions_type t)
{
    return (char*)clicon_int2str(atmap, t);
}

/*! Map actions type from string to int
 *
 * @param[in]  str    Actions type as string
 * @retval     type   Actions type as int
 */
actions_type
actions_type_str2int(char *str)
{
    return clicon_str2int(atmap, str);
}

/*! Given a yang-library/module-set, bind it to yang
 *
 * The RFC 8525 yang-library has several different sources with different XML structure,
 * (top-level is different)
 * in order to bind yang to it, the following must be done (if not already done):
 * - Bind top-level XML to yang-library
 * - Add yang-library namespace to top-lvel
 * @param[in]  h       Clixon handle
 * @param[in]  xylib   yanglib/module-set lib in XML
 * @retval     0       OK
 * @retval    -1       Error
 * @note  the YANG binding and namespace settings are side-effects, should maybe be removed
 * after use, since they could potentially affect other code
 */
int
controller_yang_library_bind(clixon_handle h,
                             cxobj        *xylib)
{
    int        retval = -1;
    yang_stmt *yspec;
    yang_stmt *ylib = NULL;
    cxobj     *xmodset;
    cxobj     *xerr = NULL;
    char      *namespace = NULL;
    cbuf      *cberr = NULL;
    int        ret;

    yspec = clicon_dbspec_yang(h);
    if ((xmodset = xml_find(xylib, "module-set")) == NULL){
        clixon_err(OE_YANG, 0, "No module-set");
        goto done;
    }
    if (xml_spec(xylib) == NULL){
        if (yang_abs_schema_nodeid(yspec, "/yanglib:yang-library", &ylib) < 0)
            goto done;
        if (ylib == NULL){
            clixon_err(OE_YANG, 0, "No yang-library spec");
            goto done;
        }
        xml_spec_set(xylib, ylib);
    }
    if (xml2ns(xylib, NULL, &namespace) < 0)
        goto done;
    if (namespace == NULL)
        xmlns_set(xylib, NULL, "urn:ietf:params:xml:ns:yang:ietf-yang-library");
    if ((ret = xml_bind_yang0(h, xmodset, YB_PARENT, NULL, 0, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if ((cberr = cbuf_new()) == NULL){
            clixon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if (netconf_err2cb(h, xml_find(xerr, "rpc-error"), cberr) < 0)
            goto done;
        clixon_err(OE_YANG, 0, "Bind failed: %s", cbuf_get(cberr));
        goto done;
    }
    retval = 0;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (xerr)
        xml_free(xerr);
    return retval;
}

/*! Translate from RFC 6022 schemalist to RFC8525 yang-library
 *
 * @param[in]  xschemas On the form: <schemas><schema><identifier>clixon-autocli</identifier>...
 * @param[in]  domain   Device domain, used as module-set name
 * @param[out] xyanglib Allocated, xml_free:d by caller
 * @retval     0        OK
 * @retval    -1        Error
 */
int
schema_list2yang_library(clixon_handle h,
                         cxobj        *xschemas,
                         char         *domain,
                         cxobj       **xyanglib)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;
    char  *identifier;
    char  *version;
    char  *format;
    char  *namespace;
    char  *location;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">");
    cprintf(cb, "<module-set>");
    cprintf(cb, "<name>%s</name>", domain);
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            continue;
        if ((identifier = xml_find_body(x, "identifier")) == NULL ||
            (namespace = xml_find_body(x, "namespace")) == NULL ||
            (format = xml_find_body(x, "format")) == NULL)
            continue;
        if (strcmp(format, "yang") != 0)
            continue;
        version = xml_find_body(x, "version");
        location = xml_find_body(x, "location");
        cprintf(cb, "<module>");
        cprintf(cb, "<name>%s</name>", identifier);
        cprintf(cb, "<revision>%s</revision>", version?version:"");
        cprintf(cb, "<namespace>%s</namespace>", namespace);
        if (location){
            cprintf(cb, "<location>%s</location>", location);
        }
        cprintf(cb, "</module>");
    }
    cprintf(cb, "</module-set>");
    cprintf(cb, "</yang-library>");
    /* Need yspec to make YB_MODULE */
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, xyanglib, NULL) < 0)
        goto done;
    if (controller_yang_library_bind(h, xml_find(*xyanglib, "yang-library")) <0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Translate from controller device modules to RFC8525 yang-library
 *
 * @param[in]  xmodset  Device/module-set with potential <module> list
 * @param[out] xyanglib Allocated, xml_free:d by caller
 * @retval     0        OK
 * @retval    -1        Error
 */
int
xdev2yang_library(cxobj  *xmodset,
                  char   *domain,
                  cxobj **xyanglib)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;
    char  *name;
    char  *revision;
    char  *namespace;

    if (domain == NULL){
        clixon_err(OE_YANG, 0, "domain is NULL");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">");
    cprintf(cb, "<module-set>");
    cprintf(cb, "<name>%s</name>", domain);
    x = NULL;
    while ((x = xml_child_each(xmodset, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "module") != 0)
            continue;
        if ((name = xml_find_body(x, "name")) == NULL){
            clixon_debug(CLIXON_DBG_CTRL, "No name in module");
            continue;
        }
        revision = xml_find_body(x, "revision");
        namespace = xml_find_body(x, "namespace");
        cprintf(cb, "<module>");
        cprintf(cb, "<name>%s</name>", name);
        if (revision)
            cprintf(cb, "<revision>%s</revision>", revision);
        if (namespace)
            cprintf(cb, "<namespace>%s</namespace>", namespace);
        cprintf(cb, "</module>");
    }
    cprintf(cb, "</module-set>");
    cprintf(cb, "</yang-library>");
    /* Need yspec to make YB_MODULE */
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, xyanglib, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Get yang of mountpoint
 *
 * @param[in]  h        Clixon handle
 * @param[in]  devname  Name of device
 * @param[out] yspec1   yang spec
 * @retval     0        OK
 * @retval    -1        Error
 */
static int
controller_mount_yang_get(clixon_handle h,
                          yang_stmt   **yu)
{
    int        retval = -1;
    yang_stmt *yspec0;
    yang_stmt *ymod;

    yspec0 = clicon_dbspec_yang(h);
    if ((ymod = yang_find(yspec0, Y_MODULE, "clixon-controller")) == NULL){
        clixon_err(OE_YANG, 0, "module clixon-controller not found");
        goto done;
    }
    if (yang_path_arg(ymod, "/devices/device/config", yu) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Get xpath of mountpoint given device name
 *
 * @param[in]  devname  Name of device
 * @param[out] cbxpath  XPath in cbuf, created by function, free with cbuf_free()
 * @retval     0        OK
 * @retval    -1        Error
 */
int
controller_mount_xpath_get(char  *devname,
                           cbuf **cbxpath)
{
    int        retval = -1;

    if ((*cbxpath = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(*cbxpath, "/ctrl:devices/ctrl:device[ctrl:name='%s']/ctrl:config", devname);
    retval = 0;
 done:
    return retval;
}

/*! Specialized version of yang_mount_get for the controller using YANG
 *
 * @param[in]  h        Clixon handle
 * @param[in]  devname  Name of device
 * @param[out] yspec1   yang spec
 * @retval     0        OK
 * @retval    -1        Error
 * @see yang_mount_get
 */
int
controller_mount_yspec_get(clixon_handle h,
                           char         *devname,
                           yang_stmt   **yspec1)
{
    int        retval = -1;
    yang_stmt *yu;
    cbuf      *cbxpath = NULL;

    *yspec1 = NULL;
    if (controller_mount_yang_get(h, &yu) < 0)
        goto done;
    if (controller_mount_xpath_get(devname, &cbxpath) < 0)
        goto done;
    /* Low-level function */
    if (yang_mount_get(yu, cbuf_get(cbxpath), yspec1) < 0)
        goto done;
    retval = 0;
 done:
    if (cbxpath)
        cbuf_free(cbxpath);
    return retval;
}

/*! Specialized version of yang_mount_set for the controller using YANG
 *
 * @param[in]  h        Clixon handle
 * @param[in]  devname  Name of device
 * @param[in]  yspec1p  yang spec
 * @retval     0        OK
 * @retval    -1        Error
 */
int
controller_mount_yspec_set(clixon_handle h,
                           char         *devname,
                           yang_stmt    *yspec1)
{
    int        retval = -1;
    yang_stmt *yu;
    cbuf      *cbxpath = NULL;

    if (controller_mount_yang_get(h, &yu) < 0)
        goto done;
    if (controller_mount_xpath_get(devname, &cbxpath) < 0)
        goto done;
    /* Low-level function */
    if (yang_mount_set(yu) < 0)
        goto done;
    retval = 0;
 done:
    if (cbxpath)
        cbuf_free(cbxpath);
    return retval;
}

/*! Go through all yspecs and delete if there are no mounts
 *
 * Essentially a garbage collect.
 * It can happen at reconnect that old yangs are left hanging and due to
 * race-conditions you cannot delete them in the connect transaction due to existing
 * yang bindings.
 * see https://github.com/clicon/clixon-controller/issues/169
 * @param[in]  h   Clixon handle
 * @retval     0   OK
 * @retval    -1   Error
 * Only removes first empty in each domain
 */
int
yang_mount_cleanup(clixon_handle h)
{
    int        retval = 1;
    yang_stmt *ymounts;
    yang_stmt *ydomain;
    yang_stmt *yspec = NULL;
    int        inext;
    int        inext2;

    if ((ymounts = clixon_yang_mounts_get(h)) == NULL){
        clixon_err(OE_YANG, ENOENT, "Top-level yang mounts not found");
        goto done;
    }
    inext = 0;
    ydomain = NULL;
    while ((ydomain = yn_iter(ymounts, &inext)) != NULL) {
        inext2 = 0;
        while ((yspec = yn_iter(ydomain, &inext2)) != NULL) {
            if (yang_keyword_get(yspec) == Y_SPEC &&
                yang_cvec_get(yspec) == NULL &&
                yang_flag_get(yspec, YANG_FLAG_SPEC_MOUNT)){
                ys_prune_self(yspec);
                ys_free(yspec);
                break;
            }
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Callback for printing version output and exit
 *
 * A plugin can customize a version (or banner) output on stdout.
 * Several version strings can be printed if there are multiple callbacks.
 * Typically invoked by command-line option -V
 * @param[in]  h   Clixon handle
 * @param[in]  f   Output file
 * @retval     0   OK
 * @retval    -1   Error
 * @see cli_controller_show_version
 */
int
controller_version(clixon_handle h,
                   FILE         *f)
{
    /* Assume clixon version already printed */
    cligen_output(f, "CLIgen: \t%s\n", CLIGEN_VERSION);
    cligen_output(f, "Controller:\t%s\n", CONTROLLER_VERSION);
    cligen_output(f, "Build:\t\t%s\n", CONTROLLER_BUILDSTR);
    return 0;
}
