#!/usr/bin/env bash
# Test get-device-schema RPC
# Retrieve YANG schemas stored on the controller for a device
# Tests:
# 1. List all schemas for a device (no identifier)
# 2. Get specific schema by identifier
# 3. Get specific schema by identifier and revision
# 4. Non-existing identifier returns empty list
# 5. Device-group input

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller (connects devices, pulls schemas)
new "reset controller"
(. ./reset-controller.sh)

if true; then # XXX
# 1. List all schemas for a device (no name)
new "get-device-schema: list all schemas for openconfig1"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc ${DEFAULTNS}>
   <get-device-schema ${CONTROLLERNS}>
      <device>openconfig1</device>
   </get-device-schema>
</rpc>]]>]]>
EOF
)
new "Check no rpc-error"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "no rpc-error" "$ret"
fi
new "expect at least openconfig-interfaces"
expect="<schema xmlns=\"http://clicon.org/controller\"><name>openconfig-interfaces</name><revision>.*</revision>"
match=$(echo $ret | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi
new "no data in listing mode"
match=$(echo $ret | grep --null -Eo "<data>") || true
if [ -n "$match" ]; then
    err "not <data>" "$ret"
fi

# 2. Get specific schema by name
new "get-device-schema: get openconfig-interfaces by name"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc ${DEFAULTNS}>
   <get-device-schema ${CONTROLLERNS}>
      <device>openconfig1</device>
      <name>openconfig-interfaces</name>
   </get-device-schema>
</rpc>]]>]]>
EOF
)
new "expect at least openconfig-interfaces"
expect="<schema xmlns=\"http://clicon.org/controller\"><name>openconfig-interfaces</name><revision>.*</revision>"
match=$(echo $ret | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi
new "no data in listing mode"
match=$(echo $ret | grep --null -Eo "<data>") || true
if [ -n "$match" ]; then
    err "not <data>" "$ret"
fi
new "no openconfig-system"
match=$(echo $ret | grep --null -Eo "<openconfig-system") || true
if [ -n "$match" ]; then
    err "not <data>" "$ret"
fi

# 3. Get schema by identifier and revision
# First find a revision from the listing
rev=$(echo $ret | grep --null -Eo "<revision>[0-9-]+</revision>" | head -1 | sed 's/<[^>]*>//g')
if [ -n "$rev" ]; then
    new "get-device-schema: get openconfig-interfaces by revision $rev and detail"
    ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc ${DEFAULTNS}>
   <get-device-schema ${CONTROLLERNS}>
      <device>openconfig1</device>
      <name>openconfig-interfaces</name>
      <revision>$rev</revision>
      <detail>true</detail>
   </get-device-schema>
</rpc>]]>]]>
EOF
    )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err "no rpc-error" "$ret"
    fi
    new "get by id+rev: expect matching revision"
    match=$(echo $ret | grep --null -Eo "<revision>$rev</revision>") || true
    if [ -z "$match" ]; then
        err "<revision>$rev</revision>" "$ret"
    fi
    new "get by id+rev: expect data"
    match=$(echo $ret | grep --null -Eo "<data><!\[CDATA\[module openconfig-interfaces") || true
    if [ -z "$match" ]; then
        err "<data><![CDATA[module openconfig-interfaces" "$ret"
    fi
fi

# 4. Non-existing identifier returns empty result (no schema entries)
new "get-device-schema: non-existing module"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc ${DEFAULTNS}>
   <get-device-schema ${CONTROLLERNS}>
      <device>openconfig1</device>
      <name>no-such-module-xyz</name>
   </get-device-schema>
</rpc>]]>]]>
EOF
)
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "no rpc-error" "$ret"
fi
new "non-existing: no schema entry"
match=$(echo $ret | grep --null -Eo "<schema") || true
if [ -n "$match" ]; then
    err "not <schema" "$ret"
fi

# 5. Device-group input
new "get-device-schema: list schemas via device-group"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc ${DEFAULTNS}>
   <get-device-schema ${CONTROLLERNS}>
      <device>openconfig*</device>
   </get-device-schema>
</rpc>]]>]]>
EOF
)
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "no rpc-error" "$ret"
fi
new "wildcard: expect schema entries"
match=$(echo $ret | grep --null -Eo "<schema") || true
if [ -z "$match" ]; then
    err "<schema" "$ret"
fi

fi # XXX

# 6. CLI: list yang names for a device
new "CLI: show devices openconfig1 schema"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 yang 2>&1)" 0 "openconfig-interfaces"

# 7. CLI: get specific yang infot
new "CLI: show devices openconfig1 yang openconfig-interfaces"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 yang openconfig-interfaces 2>&1)" 0 "openconfig-interfaces" "http://openconfig.net/yang/interfaces" "$rev"

# 8. CLI: non-existing module returns empty
new "CLI: show devices openconfig1 yang no-such-module"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 yang no-such-module-xyz 2>&1)" 0 --not-- "no-such-module"

# 9. CLI:
new "CLI: show device yang schema"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 yang schema 2>&1)" 0 "module openconfig-interfaces" "module openconfig-system"

new "CLI: show device yang schema of specific module"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 yang openconfig-interfaces schema 2>&1)" 0 "module openconfig-interfaces" --not-- "module openconfig-aaa"

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -f $CFG -z
fi

endtest
