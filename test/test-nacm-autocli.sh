#!/usr/bin/env bash
# Test NACM-aware autocli for CLI operations crossing device mountpoints.
#
# With CLICON_NACM_AUTOCLI=true, NACM read rules filter CLI tab completion so
# that denied nodes are hidden from 'set ?' expansion.  This test verifies the
# filtering for paths that cross the device mount-point boundary
# (/devices/device/config/<device-yang>).
#
# Tests:
#  1. permit-default + deny /devices/device/config/interfaces:
#       wilma: 'set devices device x config ?' hides interfaces, shows routing-policy
#       admin: sees everything
#  2. permit-default + deny /devices/device/config/routing-policy:
#       wilma: 'set devices device x config ?' shows interfaces, NOT routing-policy
#       admin: sees everything
#  3. CLICON_NACM_AUTOCLI=false: deny rule active but interfaces visible anyway
#
# Note: deny-default (read-default=deny) cannot be tested at the mountpoint
# level because the controller must read device yang-library data (via NACM-gated
# GET operations) to generate the CLI tab-completion tree.  With deny-default, this
# read is denied and no device tree is generated.  The deny-default nacm_autocli
# behaviour is fully tested in the standalone test_nacm_autocli.sh.

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

. ./nacm.sh

# Specialize controller.xml
cat <<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_NACM_CREDENTIALS>none</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <CLICON_NACM_AUTOCLI>true</CLICON_NACM_AUTOCLI>
  <CLICON_XMLDB_DIR>${dir}</CLICON_XMLDB_DIR>
</clixon-config>
EOF

# autocli: grouping-treeref=true is required for NACM autocli to work correctly
# with YANG groupings (used extensively in openconfig models).
# clispec-cache=read means the CLI always calls the backend for user-specific clispec.
cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <clispec-cache>read</clispec-cache>
  </autocli>
</clixon-config>
EOF

# NACM config: Test 1 - permit-default, deny /devices/device/config/interfaces
# nc:operation="replace" on <nacm> ensures only the NACM subtree is replaced (not the full candidate)
NACM_DENY_INTERFACES="<nacm xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-acm\" xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\" nc:operation=\"replace\"><enable-nacm>true</enable-nacm><read-default>permit</read-default><write-default>deny</write-default><exec-default>permit</exec-default>${NGROUPS}${NADMIN}<rule-list><name>limited-acl</name><group>limited</group><rule><name>deny-interfaces</name><module-name>*</module-name><path xmlns:ctrl=\"http://clicon.org/controller\" xmlns:oc-if=\"http://openconfig.net/yang/interfaces\">/ctrl:devices/ctrl:device/ctrl:config/oc-if:interfaces</path><access-operations>read</access-operations><action>deny</action></rule></rule-list></nacm>"

# NACM config: Test 2 - permit-default, deny /devices/device/config/routing-policy
# nc:operation="replace" on <nacm> ensures only the NACM subtree is replaced (not the full candidate)
NACM_DENY_ROUTINGPOLICY="<nacm xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-acm\" xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\" nc:operation=\"replace\"><enable-nacm>true</enable-nacm><read-default>permit</read-default><write-default>deny</write-default><exec-default>permit</exec-default>${NGROUPS}${NADMIN}<rule-list><name>limited-acl</name><group>limited</group><rule><name>deny-routing-policy</name><module-name>*</module-name><path xmlns:ctrl=\"http://clicon.org/controller\" xmlns:oc-rpol=\"http://openconfig.net/yang/routing-policy\">/ctrl:devices/ctrl:device/ctrl:config/oc-rpol:routing-policy</path><access-operations>read</access-operations><action>deny</action></rule></rule-list></nacm>"

new "test params: -f $CFG -E $CFD"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

new "reset controller"
. ./reset-controller.sh

new "Sleep and verify devices are open"
sleep_open "$CFD" ""

# --- Test 1: permit-default, deny /devices/device/config/interfaces ---
new "Test 1: Load NACM read-default=permit, deny /devices/device/config/interfaces"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><edit-config><target><candidate/></target><default-operation>merge</default-operation><config>$NACM_DENY_INTERFACES</config></edit-config></rpc>" "" \
    "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 1 commit"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><commit/></rpc>" "" \
    "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 1a: wilma - 'set ?' shows devices (ancestor not denied)"
expectpart "$(echo 'set ?' | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "devices"

new "Test 1b: wilma - 'set devices device x config ?' shows routing-policy but NOT interfaces"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "routing-policy" --not-- "interfaces"

new "Test 1c: admin - 'set devices device x config ?' shows both interfaces and routing-policy"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U admin 2>&1)" 0 "interfaces" "routing-policy"

new "Test 1d: wilma - edit into device config; 'set ?' shows routing-policy but NOT interfaces"
expectpart "$(printf 'edit devices device %s config\nset ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "routing-policy" --not-- "interfaces"

# --- Test 2: permit-default, deny /devices/device/config/routing-policy ---
new "Test 2: Load NACM read-default=permit, deny /devices/device/config/routing-policy"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><edit-config><target><candidate/></target><default-operation>merge</default-operation><config>$NACM_DENY_ROUTINGPOLICY</config></edit-config></rpc>" "" \
    "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 2 commit"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><commit/></rpc>" "" \
    "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 2a: wilma - 'set ?' shows devices (ancestor not denied)"
expectpart "$(echo 'set ?' | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "devices"

new "Test 2b: wilma - 'set devices device x config ?' shows interfaces but NOT routing-policy"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "interfaces" --not-- "routing-policy"

new "Test 2c: wilma - edit into device config; 'set ?' shows interfaces but NOT routing-policy"
expectpart "$(printf 'edit devices device %s config\nset ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "interfaces" --not-- "routing-policy"

new "Test 2d: admin - 'set ?' shows both devices and services (permit-all)"
expectpart "$(echo 'set ?' | $clixon_cli -f $CFG -E $CFD -m configure -U admin 2>&1)" 0 "devices" "services"

new "Test 2e: admin - 'set devices device x config ?' shows interfaces and routing-policy"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U admin 2>&1)" 0 "interfaces" "routing-policy"

# --- Test 3: CLICON_NACM_AUTOCLI=false disables filtering ---
# NACM_DENY_INTERFACES is still active; with autocli disabled, interfaces must be visible
new "Test 3: Remove NACM"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><edit-config><target><candidate/></target><config><nacm operation=\"nc:delete\" xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-acm\" xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/></config></edit-config></rpc>" ""  "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 3: Load NACM read-default=permit, deny /devices/device/config/interfaces"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><edit-config><target><candidate/></target><default-operation>merge</default-operation><config>$NACM_DENY_INTERFACES</config></edit-config></rpc>" "" \
 "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 3 commit"
expecteof_netconf "$clixon_netconf -qf $CFG -E $CFD" 0 "$DEFAULTHELLO" \
    "<rpc $DEFAULTNS><commit/></rpc>" "" \
    "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

new "Test 3a: wilma - NACM_AUTOCLI enabled (default): interfaces hidden"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -m configure -U wilma 2>&1)" 0 "bfd" --not-- "interfaces"

new "Test 3b: wilma - NACM_AUTOCLI=false: interfaces visible despite deny rule"
expectpart "$(printf 'set devices device %s config ?' ${IMG}1 | $clixon_cli -f $CFG -E $CFD -o CLICON_NACM_AUTOCLI=false -m configure -U wilma 2>&1)" 0 "bfd" "interfaces"

# --- Cleanup ---
if $BE; then
    new "Kill backend"
    stop_backend -f $CFG -E $CFD
fi

sudo rm -rf $dir

new "endtest"
endtest
