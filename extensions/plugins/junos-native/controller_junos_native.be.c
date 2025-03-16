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
  * Junos native, ie without rfc and yang compliant setting
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

/*! The device has checked for junos native */
static uint32_t DH_FLAG_JUNOS_NATIVE_CHECK = 0;

/*! The device is junos native, ie have no rfc/yang-compliant nobs */
static uint32_t DH_FLAG_JUNOS_NATIVE_ANY   = 0;

/*! The device is junos native AND is also a QFX */
static uint32_t DH_FLAG_JUNOS_NATIVE_QFX   = 0;

/*! Remove element
 */
static int
junos_native_remove(cxobj *xdata,
                    char  *xpath)
{
    int     retval = -1;
    cxobj  *xn;
    cxobj **vec0 = NULL;
    size_t  vec0len;
    int     i;

    if (xpath_vec(xdata, NULL, "%s", &vec0, &vec0len, xpath) < 0)
        goto done;
    for (i=0; i<vec0len; i++){
        xn = vec0[i];
        xml_purge(xn);
        clixon_debug(CLIXON_DBG_CTRL, "Removing %s", xpath);
    }
    retval = 0;
 done:
    if (vec0)
        free(vec0);
    return retval;
}

static int
junos_native_add1(cxobj *xdata,
                  char  *prefix,
                  char  *module,
                  char  *namespace)
{
    int     retval = -1;
    cxobj **vec = NULL;
    size_t  veclen;
    cxobj  *xn;
    int     i;

    if (xpath_vec(xdata, NULL, "%s/%s", &vec, &veclen, prefix, module) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if (xml_add_attr(xn, "xmlns", namespace, NULL, NULL) == NULL)
            goto done;
        clixon_debug(CLIXON_DBG_CTRL, "Patching %s/%s", prefix, module);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Add xmlns attribute
 */
static int
junos_native_add_xmlns(cxobj *xdata,
                       int    qfx,
                       char  *module)
{
    int     retval = -1;
    cbuf   *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "http://yang.juniper.net/junos%s/conf/%s", qfx?"-qfx":"", module);
    if (junos_native_add1(xdata, "configuration", module, cbuf_get(cb)) < 0)
        goto done;
    if (junos_native_add1(xdata, "configuration/groups", module, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Remove xmlns attribute n base and group
 */
static int
junos_native_rm_xmlns(cxobj *xdata,
                      char  *xpath)
{
    int     retval = -1;
    cxobj  *xn;
    cxobj **vec = NULL;
    cxobj  *xa;
    size_t  veclen;
    int     i;

    if (xpath_vec(xdata, NULL, "%s", &vec, &veclen, xpath) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((xa = xml_find_type(xn, NULL, "xmlns", CX_ATTR)) != NULL)
            xml_purge(xa);
        clixon_debug(CLIXON_DBG_CTRL, "Patching %s", xpath);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Add new body object and insert at position
 */
static int
new_body_pos(char  *name,
             cxobj *xp,
             char  *val,
             int    pos)
{
    int    retval = -1;
    cxobj *xn;
    cxobj *xb;

    if (name == NULL){
        clixon_err(OE_XML, EINVAL, "name is NULL");
        goto done;
    }
    if ((xn = xml_new(name, NULL, CX_ELMNT)) == NULL)
        goto done;
    if ((xb = xml_new("body", xn, CX_BODY)) == NULL){
        xml_free(xn);
        goto done;
    }
    if (val && xml_value_set(xb, val) < 0){
        xml_free(xn);
        xml_free(xb);
        goto done;
    }
    xml_type_set(xb, CX_BODY);
    if (xml_child_insert_pos(xp, xn, pos) < 0)
        goto done;
    xml_parent_set(xn, xp);
    retval = 0;
 done:
    return retval;
}

/*! Expand an XML enum from a single to a double choice-id + value
 *
 * 1. Find the enum
 * 2. Create new id body and insert after enum
 * 3. Create new val body and insert after id
 * 4. Remove enum
 * Example:
 * from:
 *    <prefix-length-range>/19-/29</prefix-length-range>
 * to:
 *    <choice-ident>prefix-length-range</choice-ident>
 *    <choice-value>/19-/29</choice-value>
 * @see junos_native_enum_reduce
 */
static int
junos_native_enum_expand(cxobj *xdata,
                         char  *xpath,
                         char  *idtag,
                         char  *valtag)
{
    int     retval = -1;
    cxobj  *xn;
    cxobj  *xp;
    cxobj **vec = NULL;
    size_t  veclen;
    char   *id;
    char   *val;
    int     pos;
    int     i;

    if (idtag == NULL || valtag == NULL){
        clixon_err(OE_XML, EINVAL, "idtag or valtag is NULL");
        goto done;
    }
    if (xpath_vec(xdata, NULL, "%s", &vec, &veclen, xpath) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        xp = xml_parent(xn);
        xml_enumerate_children(xp);
        pos = xml_enumerate_get(xn);
        if ((id = xml_name(xn)) == NULL)
            continue;
        if (new_body_pos(idtag, xp, id, pos++) < 0)
            goto done;
        val = xml_body(xn);
        if (new_body_pos(valtag, xp, val, pos++) < 0)
            goto done;
        xml_purge(xn);
        clixon_debug(CLIXON_DBG_CTRL, "Replace %s enum->choice", xpath);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Reduce an XML enum frm a double choice-id + value to a single enum
 *
 * 1. Find the enum
 * XXX 2. Create new id body and insert after enum
 * XXX 3. Create new val body and insert after id
 * XXX 4. Remove enum
 * Example:
 * from:
 *    <choice-ident>prefix-length-range</choice-ident>
 *    <choice-value>/19-/29</choice-value>
 * to:
 *    <prefix-length-range>/19-/29</prefix-length-range>
 * @see junos_native_enum_expand
 */
static int
junos_native_enum_reduce(cxobj *xdata,
                         char  *xpath,
                         char  *idtag,
                         char  *valtag)
{
    int     retval = -1;
    cxobj  *xn1;
    cxobj  *xn2;
    cxobj  *xp;
    cxobj **vec = NULL;
    size_t  veclen;
    char   *id;
    char   *val1;
    char   *val2;
    int     pos;
    int     i;

    if (idtag == NULL || valtag == NULL){
        clixon_err(OE_XML, EINVAL, "idtag or valtag is NULL");
        goto done;
    }
    if (xpath_vec(xdata, NULL, "%s", &vec, &veclen, xpath) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xn1 = vec[i];
        xp = xml_parent(xn1);
        xml_enumerate_children(xp);
        pos = xml_enumerate_get(xn1);
        if ((xn2 = xml_child_i(xp, pos+1)) == NULL)
            continue;
        if ((id = xml_name(xn1)) == NULL)
            continue;
        if (strcmp(id, idtag) != 0)
            continue;
        if ((id = xml_name(xn2)) == NULL)
            continue;
        if (strcmp(id, valtag) != 0)
            continue;
        val1 = xml_body(xn1);
        val2 = xml_body(xn2);
        if (new_body_pos(val1, xp, val2, pos) < 0)
            goto done;
        xml_purge(xn1);
        xml_purge(xn2);
        clixon_debug(CLIXON_DBG_CTRL, "Replace %s enum->choice", xpath);
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Modify incoming XML config to match YANGs
 *
 * This is for junos native, ie without rfc and yang compliant setting
 * @param[in]  dh      Device handle
 * @param[in]  xdata   Incoming XML config data
 * @retval     0       OK
 * @retval    -1       Error
 * @see junos_native_modify_send
 */
int
junos_native_modify_recv(device_handle dh,
                         cxobj        *xdata)
{
    int    retval = -1;
    cxobj *xn;
    cxobj *xa;
    cxobj *xylib;
    int    qfx = 0;

    /* Algorithm:
     * 1. incoming config has no rfc-compliant or yang-compliant -> DH_FLAG_JUNOS_NATIVE
     * 2. If (1) and qfc-specific yang is announced -> DH_FLAG_JUNOS_NATIVE
     */
    if (device_handle_flag_get(dh, DH_FLAG_JUNOS_NATIVE_CHECK) == 0){
        if (xpath_first(xdata, NULL, "configuration/system/services/netconf/rfc-compliant") == NULL ||
            xpath_first(xdata, NULL, "configuration/system/services/netconf/yang-compliant") == NULL){
            if ((xylib = device_handle_yang_lib_get(dh)) != NULL &&
                xpath_first(xylib, NULL, "module-set/module[name='junos-qfx-conf-root']") != NULL){
                device_handle_flag_set(dh, DH_FLAG_JUNOS_NATIVE_QFX);
            }
            else{
                device_handle_flag_set(dh, DH_FLAG_JUNOS_NATIVE_ANY);
            }
        }
        device_handle_flag_set(dh, DH_FLAG_JUNOS_NATIVE_CHECK);
    }
    /* Check junos-native flags */
    if (device_handle_flag_get(dh, DH_FLAG_JUNOS_NATIVE_ANY) == 0 &&
        (qfx = device_handle_flag_get(dh, DH_FLAG_JUNOS_NATIVE_QFX)) == 0)
        goto ok;
    /* Switch top-level namespace */
    if ((xn = xpath_first(xdata, NULL, "configuration")) != NULL &&
        (xa = xml_find_type(xn, NULL, "xmlns", CX_ATTR)) != NULL){
        if (qfx){
            if (xml_value_set(xa, "http://yang.juniper.net/junos-qfx/conf/root") < 0)
                goto done;
        }
        else
            if (xml_value_set(xa, "http://yang.juniper.net/junos/conf/root") < 0)
                goto done;
        clixon_debug(CLIXON_DBG_CTRL, "Patching configuration");
    }
    /* Remove elements */
    if (junos_native_remove(xdata, "//junos:comment") < 0)
        goto done;
    if (junos_native_remove(xdata, "//undocumented") < 0)
        goto done;

    /* Add namespace */
    /* multicast-snooping */
    if (junos_native_add_xmlns(xdata, qfx, "multicast-snooping-options") < 0)
        goto done;

    /* event-options */
    if (junos_native_add_xmlns(xdata, qfx, "event-options") < 0)
        goto done;
    /* multi-chassis */
    if (junos_native_add_xmlns(xdata, qfx, "multi-chassis") < 0)
        goto done;
    /* forwarding-options */
    if (junos_native_add_xmlns(xdata, qfx, "forwarding-options") < 0)
        goto done;
    /* policy-options */
    if (junos_native_add_xmlns(xdata, qfx, "policy-options") < 0)
        goto done;
    /* protocols */
    if (junos_native_add_xmlns(xdata, qfx, "protocols") < 0)
        goto done;
    /* logical-systems */
    if (junos_native_add_xmlns(xdata, qfx, "logical-systems") < 0)
        goto done;
    /* routing-instances */
    if (junos_native_add_xmlns(xdata, qfx, "routing-instances") < 0)
        goto done;
    /* access-profile */
    if (junos_native_add_xmlns(xdata, qfx, "access-profile") < 0)
        goto done;
    /* diameter */
    if (junos_native_add_xmlns(xdata, qfx, "diameter") < 0)
        goto done;
    /* access */
    if (junos_native_add_xmlns(xdata, qfx, "access") < 0)
        goto done;
    /* unified-edge */
    if (junos_native_add_xmlns(xdata, qfx, "unified-edge") < 0)
        goto done;
    /* interfaces */
    if (junos_native_add_xmlns(xdata, qfx, "interfaces") < 0)
        goto done;
    /* chassis */
    if (junos_native_add_xmlns(xdata, qfx, "chassis") < 0)
        goto done;
    /* snmp */
    if (junos_native_add_xmlns(xdata, qfx, "snmp") < 0)
        goto done;
    /* fabric */
    if (junos_native_add_xmlns(xdata, qfx, "fabric") < 0)
        goto done;
    /* switch-options */
    if (junos_native_add_xmlns(xdata, qfx, "switch-options") < 0)
        goto done;
    /* accounting-options */
    if (junos_native_add_xmlns(xdata, qfx, "accounting-options") < 0)
        goto done;
    /* dynamic-profiles */
    if (junos_native_add_xmlns(xdata, qfx, "dynamic-profiles") < 0)
        goto done;
    /* vmhost */
    if (junos_native_add_xmlns(xdata, qfx, "vmhost") < 0)
        goto done;
    /* firewall */
    if (junos_native_add_xmlns(xdata, qfx, "firewall") < 0)
        goto done;
    /* class-of-service */
    if (junos_native_add_xmlns(xdata, qfx, "class-of-service") < 0)
        goto done;
    /* jsrc-partition */
    if (junos_native_add_xmlns(xdata, qfx, "jsrc-partition") < 0)
        goto done;
    /* routing-options */
    if (junos_native_add_xmlns(xdata, qfx, "routing-options") < 0)
        goto done;
    /* session-limit-group */
    if (junos_native_add_xmlns(xdata, qfx, "session-limit-group") < 0)
        goto done;
    /* vlans */
    if (junos_native_add_xmlns(xdata, qfx, "vlans") < 0)
        goto done;
    /* bridge-domains */
    if (junos_native_add_xmlns(xdata, qfx, "bridge-domains") < 0)
        goto done;
    /* services */
    if (junos_native_add_xmlns(xdata, qfx, "services") < 0)
        goto done;
    /* system */
    if (junos_native_add_xmlns(xdata, qfx, "system") < 0)
        goto done;
    /* jsrc */
    if (junos_native_add_xmlns(xdata, qfx, "jsrc") < 0)
        goto done;
    /* security */
    if (junos_native_add_xmlns(xdata, qfx, "security") < 0)
        goto done;
    /* apply-macro */
    if (junos_native_add_xmlns(xdata, qfx, "apply-macro") < 0)
        goto done;
    /* applications */
    if (junos_native_add_xmlns(xdata, qfx, "applications") < 0)
        goto done;
    /* poe */
    if (junos_native_add_xmlns(xdata, qfx, "poe") < 0)
        goto done;
    /* tenants */
    if (junos_native_add_xmlns(xdata, qfx, "tenants") < 0)
        goto done;

    /* Replace enums */
    /* junos-conf-policy-options.yang: grouping route_filter_list_items */
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/exact", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/longer", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/orlonger", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/upto", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/through", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/prefix-length-range", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/route-filter-list/rf_list/adress-mask", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: grouping control_prefix_list_filter_type */
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/exact", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/longer", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/orlonger", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: list community */
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/equal-literal", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/set", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/plus-literal", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/add", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/minus-literal", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/then/community/delete", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: grouping control_route_filter_type */
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/exact", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/longer", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/orlonger", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/upto", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/through", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/prefix-length-range", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_expand(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/address-mask", "choice-ident", "choice-value") < 0)
        goto done;
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Modify outgoing XML config to match YANGs
 *
 * This is for junos native, ie without rfc and yang compliant setting
 * @param[in]  dh      Device handle
 * @param[in]  xdata   Outgoing XML config data
 * @retval     0       OK
 * @retval    -1       Error
 * @see junos_native_modify_recv
 */
int
junos_native_modify_send(device_handle dh,
                         cxobj        *xdata)
{
    int    retval = -1;
    cxobj *xn;
    cxobj *xa;
    int    qfx = 0;

    if (device_handle_flag_get(dh, DH_FLAG_JUNOS_NATIVE_ANY) == 0 &&
        (qfx = device_handle_flag_get(dh, DH_FLAG_JUNOS_NATIVE_QFX)) == 0)
        goto ok;
    /* Switch top-level namespace */
    if ((xn = xpath_first(xdata, NULL, "configuration")) != NULL &&
        (xa = xml_find_type(xn, NULL, "xmlns", CX_ATTR)) != NULL){
        if (xml_value_set(xa,"http://xml.juniper.net/xnm/1.1/xnm") < 0)
            goto done;
        clixon_debug(CLIXON_DBG_CTRL, "Patching configuration");
    }
    /* Remove namespace */
    /* multicast-snooping */
    if (junos_native_rm_xmlns(xdata, "configuration/multicast-snooping") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/multicast-snooping") < 0)
        goto done;
    /* event-options */
    if (junos_native_rm_xmlns(xdata, "configuration/event-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/event-options") < 0)
        goto done;
    /* multi-chassis */
    if (junos_native_rm_xmlns(xdata, "configuration/multi-chassis") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/multi-chassis") < 0)
        goto done;
    /* forwarding-options */
    if (junos_native_rm_xmlns(xdata, "configuration/forwarding-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/forwarding-options") < 0)
        goto done;
    /* policy-options */
    if (junos_native_rm_xmlns(xdata, "configuration/policy-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/policy-options") < 0)
        goto done;
    /* protocols */
    if (junos_native_rm_xmlns(xdata, "configuration/protocols") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/protocols") < 0)
        goto done;
    /* logical-systems */
    if (junos_native_rm_xmlns(xdata, "configuration/logical-systems") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/logical-systems") < 0)
        goto done;
    /* routing-instances */
    if (junos_native_rm_xmlns(xdata, "configuration/routing-instances") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/routing-instances") < 0)
        goto done;
    /* access-profile */
    if (junos_native_rm_xmlns(xdata, "configuration/access-profile") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/access-profile") < 0)
        goto done;
    /* diameter */
    if (junos_native_rm_xmlns(xdata, "configuration/diameter") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/diameter") < 0)
        goto done;
    /* access */
    if (junos_native_rm_xmlns(xdata, "configuration/access") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/access") < 0)
        goto done;
    /* unified-edge */
    if (junos_native_rm_xmlns(xdata, "configuration/unified-edge") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/unified-edge") < 0)
        goto done;
    /* interfaces */
    if (junos_native_rm_xmlns(xdata, "configuration/interfaces") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/interfaces") < 0)
        goto done;
    /* chassis */
    if (junos_native_rm_xmlns(xdata, "configuration/chassis") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/chassis") < 0)
        goto done;
    /* snmp */
    if (junos_native_rm_xmlns(xdata, "configuration/snmp") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/snmp") < 0)
        goto done;
    /* fabric */
    if (junos_native_rm_xmlns(xdata, "configuration/fabric") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/fabric") < 0)
        goto done;
    /* switch-options */
    if (junos_native_rm_xmlns(xdata, "configuration/switch-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/switch-options") < 0)
        goto done;
    /* accounting-options */
    if (junos_native_rm_xmlns(xdata, "configuration/accounting-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/accounting-options") < 0)
        goto done;
    /* dynamic-profiles */
    if (junos_native_rm_xmlns(xdata, "configuration/dynamic-profiles") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/dynamic-profiles") < 0)
        goto done;
    /* vmhost */
    if (junos_native_rm_xmlns(xdata, "configuration/vmhost") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/vmhost") < 0)
        goto done;
    /* firewall */
    if (junos_native_rm_xmlns(xdata, "configuration/firewall") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/firewall") < 0)
        goto done;
    /* class-of-service */
    if (junos_native_rm_xmlns(xdata, "configuration/class-of-service") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/class-of-service") < 0)
        goto done;
    /* jsrc-partition */
    if (junos_native_rm_xmlns(xdata, "configuration/jsrc-partition") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/jsrc-partition") < 0)
        goto done;
    /* routing-options */
    if (junos_native_rm_xmlns(xdata, "configuration/routing-options") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/routing-options") < 0)
        goto done;
    /* session-limit-group */
    if (junos_native_rm_xmlns(xdata, "configuration/session-limit-group") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/session-limit-group") < 0)
        goto done;
    /* vlans */
    if (junos_native_rm_xmlns(xdata, "configuration/vlans") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/vlans") < 0)
        goto done;
    /* bridge-domains */
    if (junos_native_rm_xmlns(xdata, "configuration/bridge-domains") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/bridge-domains") < 0)
        goto done;
    /* services */
    if (junos_native_rm_xmlns(xdata, "configuration/services") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/services") < 0)
        goto done;
    /* system */
    if (junos_native_rm_xmlns(xdata, "configuration/system") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/system") < 0)
        goto done;
    /* jsrc */
    if (junos_native_rm_xmlns(xdata, "configuration/jsrc") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/jsrc") < 0)
        goto done;
    /* security */
    if (junos_native_rm_xmlns(xdata, "configuration/security") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/security") < 0)
        goto done;
    /* apply-macro */
    if (junos_native_rm_xmlns(xdata, "configuration/apply-macro") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/apply-macro") < 0)
        goto done;
    /* applications */
    if (junos_native_rm_xmlns(xdata, "configuration/applications") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/applications") < 0)
        goto done;
    /* poe */
    if (junos_native_rm_xmlns(xdata, "configuration/poe") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/poe") < 0)
        goto done;
    /* tenants */
    if (junos_native_rm_xmlns(xdata, "configuration/tenants") < 0)
        goto done;
    if (junos_native_rm_xmlns(xdata, "configuration/groups/tenants") < 0)
        goto done;

    /* Replace enums */
    /* junos-conf-policy-options.yang: grouping route_filter_list_items */
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='exact']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='longer']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='orlonger']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='upto']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='through']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='prefix-length-range']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/route-filter-list/rf_list/choice-ident[.='adress-mask']", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: grouping control_prefix_list_filter_type */
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/choice-ident[.='exact']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/choice-ident[.='longer']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/prefix-list-filter/choice-ident[.='orlonger']", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: list community */
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='equal-literal']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='set']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='plus-literal']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='add']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='minus-literal']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/then/community/choice-ident[.='delete']", "choice-ident", "choice-value") < 0)
        goto done;

    /* junos-conf-policy-options.yang: grouping control_route_filter_type */
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='exact']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='longer']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='orlonger']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='upto']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='through']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='prefix-length-range']", "choice-ident", "choice-value") < 0)
        goto done;
    if (junos_native_enum_reduce(xdata, "configuration/policy-options/policy-statement/term/from/route-filter/choice-ident[.='address-mask']", "choice-ident", "choice-value") < 0)
        goto done;
 ok:
    retval = 0;
 done:
    return retval;
}

#ifdef CLIXON_PLUGIN_USERDEF
/*! Called when application is "started", (almost) all initialization is complete
 *
 * Backend: daemon is in the background. If daemon privileges are dropped
 *          this callback is called *before* privileges are dropped.
 * @param[in] h    Clixon handle
 * @see junos_native_modify_recv  where the flags are set from incoming XML config
 */
static int
junos_native_start(clixon_handle h)
{
    int      retval = -1;

    if (device_handle_allocate_flag(h, &DH_FLAG_JUNOS_NATIVE_CHECK) < 0)
        goto done;
    if (device_handle_allocate_flag(h, &DH_FLAG_JUNOS_NATIVE_ANY) < 0)
        goto done;
    if (device_handle_allocate_flag(h, &DH_FLAG_JUNOS_NATIVE_QFX) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Callback for generic user-defined semantics
 *
 * An application may define a user-defined callback which is called from _within_
 * an application callback. That is, not from within the Clixon base code.
 * This means that the application needs to define semantics based on where and when the
 * callback is called, and also the semantics of the arguments to the callback.
 * @param[in]  h    Clixon handle
 * @param[in]  type User-defined type
 * @param[in]  arg  User-defined argument
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
junos_native_userdef(clixon_handle h,
                     int           type,
                     cxobj        *xn,
                     void         *arg)
{
    int           retval = -1;
    device_handle dh = (device_handle)arg;

    if (xn == NULL || dh == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "xn or dh is NULL");
        goto done;
    }
    switch (type){
    case CTRL_NX_RECV:
        if (junos_native_modify_recv(dh, xn) < 0)
            goto done;
        break;
    case CTRL_NX_SEND:
        if (junos_native_modify_send(dh, xn) < 0)
            goto done;
        break;
    default:
        break;
    }
    retval = 0;
 done:
    return retval;
}
#endif /* CLIXON_PLUGIN_USERDEF */

/*! Forward declaration */
clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    "junos-native",
#ifdef CLIXON_PLUGIN_USERDEF
    .ca_start        = junos_native_start,
    .ca_userdef      = junos_native_userdef,
#endif
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    return &api;
}
