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
  * Backend rpc callbacks of standard RPCs
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
#include <assert.h>
#include <sys/time.h>

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
#include "controller_rpc_std.h"

/*! Given an attribute name and its expected namespace, find its value
 *
 * An attribute may have a prefix(or NULL). The routine finds the associated
 * xmlns binding to find the namespace: <namespace>:<name>.
 * If such an attribute is not found, failure is returned with cbret set,
 * If such an attribute is found, its string value is returned and removed from XML
 * @param[in]  x         XML node (where to look for attribute)
 * @param[in]  name      Attribute name
 * @param[in]  ns        (Expected) Namespace of attribute
 * @param[out] cbret     Error message (if retval=0)
 * @param[out] valp      Malloced value (if retval=1)
 * @retval     1         OK
 * @retval     0         Failed (cbret set)
 * @retval    -1         Error
 * @note as a side.effect the attribute is removed
 * @note slightly modified from clixon_datastore_write.c
 */
static int
attr_ns_value(cxobj      *x,
              const char *name,
              const char *ns,
              char      **valp)
{
    int    retval = -1;
    cxobj *xa;
    char  *ans = NULL; /* attribute namespace */
    char  *val = NULL;

    /* prefix=NULL since we do not know the prefix */
    if ((xa = xml_find_type(x, NULL, name, CX_ATTR)) != NULL){
        if (xml2ns(xa, xml_prefix(xa), &ans) < 0)
            goto done;
        if (ans == NULL){ /* the attribute exists, but no namespace */
            goto fail;
        }
        /* the attribute exists, but not w expected namespace */
        if (ns == NULL ||
            strcmp(ans, ns) == 0){
            if ((val = strdup(xml_value(xa))) == NULL){
                clixon_err(OE_UNIX, errno, "malloc");
                goto done;
            }
            xml_purge(xa);
        }
    }
    *valp = val;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Look for creator attributes in edit-config, remove it and create entry in service instance
 *
 * Callback function type for xml_apply
 * @param[in]  x    XML node
 * @param[in]  arg  General-purpose argument
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
creator_applyfn(cxobj *x,
                void  *arg)
{
    int        retval = -1;
    cxobj     *xserv = (cxobj*)arg;
    char      *creator = NULL;
    char      *service = NULL;
    cvec      *nsc = 0;
    char      *xpath = NULL;
    cxobj     *xi;
    cxobj     *xc;
    char      *instance;
    char      *p;
    char       q;
    yang_stmt *yserv;
    yang_stmt *yi;
    char      *ns;
    cvec      *cvk;
    char      *key;
    char      *ykey;
    int        ret;

    /* Special clixon-lib attribute for keeping track of creator of objects */
    if ((ret = attr_ns_value(x, "creator", CLIXON_LIB_NS, &creator)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (creator != NULL){
        if (xml2xpath(x, nsc, 0, 0, &xpath) < 0)
            goto done;
        /* Find existing entry in xserv, if not found create it */
        if ((xi = xpath_first(xserv, NULL, "%s", creator)) != NULL){
            if ((xc = xml_find_type(xi, NULL, "created", CX_ELMNT)) == NULL)
                goto ok;
            if (xpath_first(xc, 0, "path[.='%s']", xpath) != NULL)
                goto ok; /* duplicate: silently drop */
            clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "Created path: %s %s", xpath, creator);
            if ((ret = clixon_xml_parse_va(YB_PARENT, NULL, &xc, NULL, "<path>%s</path>", xpath)) < 0)
                goto done;
            if (ret == 0)
                goto ok;
        }
        else {
            /* split creator into service, key and instance, assuming creator is on the form:
             * service[key='myname']
             */
            if ((service = strdup(creator)) == NULL){
                clixon_err(OE_UNIX, errno, "strdup");
                goto done;
            }
            if ((p = index(service, '[')) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no instance: [] in %s", creator);
                goto done;
            }
            *p++ = '\0';
            key = p;
            if ((p = index(p, '=')) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no instance = in %s", creator);
                goto done;
            }
            *p++ = '\0';
            q = *p++; /* assume quote */
            instance = p;
            if ((p = index(p, q)) == NULL){
                clixon_err(OE_YANG, 0, "Creator attribute, no quote in %s", creator);
                goto done;
            }
            *p = '\0';
            yserv = xml_spec(xserv);
            if ((yi = yang_find(yserv, Y_LIST, service)) == NULL){
                clixon_err(OE_YANG, 0, "Invalid creator service name in %s", creator);
                goto done;
            }
            if ((cvk = yang_cvec_get(yi)) == NULL)
                goto ok;
            if ((ykey = cvec_i_str(cvk, 0)) == NULL)
                goto ok;
            if (strcmp(key, ykey) != 0){
                clixon_err(OE_YANG, 0, "Creator tag: \"%s\": Invalid key: \"%s\", expected: \"%s\"", creator, key, ykey);
                goto done;
            }
            if ((ns = yang_find_mynamespace(yi)) == NULL)
                goto ok;
            clixon_debug(CLIXON_DBG_CTRL | CLIXON_DBG_DETAIL, "Created path: %s %s", xpath, creator);
            if ((ret = clixon_xml_parse_va(YB_PARENT, NULL, &xserv, NULL,
                                           "<%s xmlns=\"%s\"><%s>%s</%s>"
                                           "<created nc:operation=\"merge\">"
                                           "<path>%s</path></created></%s>",
                                           service,
                                           ns,
                                           ykey,
                                           instance,
                                           ykey,
                                           xpath,
                                           service)) < 0)
                goto done;
            if (ret == 0)
                goto ok;
        }
    }
 ok:
    retval = 0;
 done:
    if (creator)
        free(creator);
    if (service)
        free(service);
    if (xpath)
        free(xpath);
    return retval;
 fail:
    retval = 1;
    goto done;
}

/*! Intercept services-commit create-subscription and deny if there is already one
 *
 * The registration should be made from plugin-init to ensure the check is made before
 * the regular from_client_create_subscription callback
 * @param[in]  h       Clixon handle
 * @param[in]  xe      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see clixon-controller.yang notification services-commit
 */
int
check_services_commit_subscription(clixon_handle h,
                                   cxobj        *xe,
                                   cbuf         *cbret,
                                   void         *arg,
                                   void         *regarg)
{
    int                  retval = -1;
    char                *stream = "NETCONF";
    cxobj               *x; /* Generic xml tree */
    cvec                *nsc = NULL;
    event_stream_t      *es;
    struct stream_subscription *ss;
    int                         i;

    clixon_debug(CLIXON_DBG_CTRL, "");
    /* XXX should use prefix cf edit_config */
    if ((nsc = xml_nsctx_init(NULL, EVENT_RFC5277_NAMESPACE)) == NULL)
        goto done;
    if ((x = xpath_first(xe, nsc, "//stream")) == NULL ||
        (stream = xml_find_value(x, "body")) == NULL ||
        (es = stream_find(h, stream)) == NULL)
        goto ok;
    if (strcmp(stream, "services-commit") != 0)
        goto ok;
    if ((ss = es->es_subscription) != NULL){
        i = 0;
        do {
            ss = NEXTQ(struct stream_subscription *, ss);
            i++;
        } while (ss && ss != es->es_subscription);
        if (i>0){
            cbuf_reset(cbret);
            if (netconf_operation_failed(cbret, "application", "services-commit client already registered")< 0)
                goto done;
        }
    }
 ok:
    retval = 0;
 done:
    if (nsc)
        xml_nsctx_free(nsc);
    return retval;
}

/*! Controller wrapper of edit-config
 *
 * Find and remove creator attributes and create services/../created structures.
 * Ignore all semantic errors, trust base function error-handling
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see from_client_edit_config
 */
int
controller_edit_config(clixon_handle h,
                       cxobj        *xe,
                       cbuf         *cbret,
                       void         *arg,
                       void         *regarg)
{
    int        retval = -1;
    cxobj     *xc;
    cvec      *nsc = NULL;
    char      *target;
    yang_stmt *yspec;
    cxobj     *xconfig = NULL;
    cxobj     *xserv;
    int        ret;

    clixon_debug(CLIXON_DBG_CTRL, "Find and remove creator attributes and create services/../created structures");
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_YANG, ENOENT, "No yang spec9");
        goto done;
    }
    if (xml_nsctx_node(xe, &nsc) < 0)
        goto done;
    if ((target = netconf_db_find(xe, "target")) == NULL)
        goto ok;
    /* Get config element */
    if ((xc = xpath_first(xe, nsc, "%s", NETCONF_INPUT_CONFIG)) == NULL){
        goto ok;
    }
    if ((ret = xml_bind_yang(h, xc, YB_MODULE, yspec, 0, NULL)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
    if ((xconfig = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
        goto done;
    if (clixon_xml_parse_va(YB_NONE, NULL, &xconfig, NULL,
                            "<services xmlns=\"%s\" xmlns:nc=\"%s\"/>",
                            CONTROLLER_NAMESPACE,
                            NETCONF_BASE_NAMESPACE) < 0){
        goto ok;
    }
    if ((xserv = xml_find_type(xconfig, NULL, "services", CX_ELMNT)) == NULL)
        goto ok;
    if ((ret = xml_bind_yang0(h, xserv, YB_MODULE, yspec, 0, 0, NULL)) < 0)
        goto done;
    if (ret == 0)
        goto ok;
    if (xml_spec(xserv) == NULL)
        goto ok;
    if ((ret = xml_apply(xc, CX_ELMNT, creator_applyfn, xserv)) < 0)
        goto done;
    if (ret == 1){
        if (netconf_operation_failed(cbret, "application", "Translation for creator attributes to created tag")< 0)
            goto done;
        goto ok;
    }
    if (xml_child_nr_type(xserv, CX_ELMNT) == 0)
        goto ok;
    clixon_debug_xml(CLIXON_DBG_CTRL2, xserv, "Objects created in %s-db", target);
    if ((ret = xmldb_put(h, target, OP_NONE, xconfig, NULL, cbret)) < 0){
        if (netconf_operation_failed(cbret, "protocol", "%s", clixon_err_reason())< 0)
            goto done;
        goto ok;
    }
 ok:
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xconfig)
        xml_free(xconfig);
    return retval;
}

/*! Controller wrapper of clixon-lib stats RPC
 *
 * This takes over / overrides the clixon callback
 * @param[in]  h       Clixon handle
 * @param[in]  xn      Request: <rpc><xn></rpc>
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error..
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register()
 * @retval     0       OK
 * @retval    -1       Error
 * @see from_client_stats
 */
int
controller_clixon_stats(clixon_handle h,
                        cxobj        *xe,
                        cbuf         *cbret,
                        void         *arg,
                        void         *regarg)
{
    int            retval = -1;
    char          *str;
    int            modules = 0;
    xml_stats_enum xml_type = XML_STATS_ALL;
    uint64_t       nr;
    size_t         sz;

    if ((str = xml_find_body(xe, "modules")) != NULL)
        modules = strcmp(str, "true") == 0;
    if ((str = xml_find_body(xe, "xml-type")) != NULL)
        xml_type = xml_stats_str2type(str);
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    if (clixon_backend_stats(h, modules, xml_type, cbret) < 0)
        goto done;
    if (xml_type == XML_STATS_ALL){
        nr = 0;
        sz = 0;
        if (controller_transaction_stats(h, xml_type, &nr, &sz) < 0)
            goto done;
        if (nr){
            cprintf(cbret, "<transactions xmlns=\"%s\">", CONTROLLER_NAMESPACE);
            cprintf(cbret, "<nr>%" PRIu64 "</nr>", nr);
            cprintf(cbret, "<size>%" PRIu64 "</size>", sz);
            cprintf(cbret, "</transactions>");
        }
        nr = 0;
        sz = 0;
        if (device_handle_stats(h, &nr, &sz) < 0)
            goto done;
        if (nr){
            cprintf(cbret, "<devices xmlns=\"%s\">", CONTROLLER_NAMESPACE);
            cprintf(cbret, "<nr>%" PRIu64 "</nr>", nr);
            cprintf(cbret, "<size>%" PRIu64 "</size>", sz);
            cprintf(cbret, "</devices>");
        }
    }
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    return retval;
}

/*! Register callback for rpc calls
 */
int
controller_rpc_std_init(clixon_handle h)
{
    int retval = -1;

    /* Check that services subscriptions is just done once */
    if (rpc_callback_register(h, check_services_commit_subscription,
                              NULL,
                              EVENT_RFC5277_NAMESPACE, "create-subscription") < 0)
        goto done;
    /* Wrapper of standard RPCs */
    if (rpc_callback_register(h, controller_edit_config,
                              NULL,
                              NETCONF_BASE_NAMESPACE,
                              "edit-config"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, controller_clixon_stats,
                              NULL,
                              CLIXON_LIB_NS,
                              "stats"
                              ) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
