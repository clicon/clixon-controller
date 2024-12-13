#!/usr/bin/env bash
# Testcases around local device configs (meta) and remote device config push
# Error cases as described in clixon-controller.yang:
# 1) If no devices are selected UNLESS push=NONE.
# 2) If local device fields are changed (except device mount-point - 'config')
# 3) If a matching device is CLOSED UNLESS push=NONE

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

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

# Local change: just change device description
# 1: NAME
# 2: DESC
function local_change()
{
    NAME=$1
    DESC=$2

    new "local change $NAME = $DESC"
    ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <devices xmlns="http://clicon.org/controller">
          <device>
            <name>$NAME</name>
            <description nc:operation="merge">$DESC</description>
          </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
       )
#    echo "$ret"   
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "OK reply"
    fi
}

NAME=${IMG}1

# 1) If no devices are selected UNLESS push=NONE.
new "No devices, expect no selected error"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>COMMIT</push>
    <source>ds:candidate</source>
  </controller-commit>
</rpc>]]>]]>
EOF
   )
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -z "$match" ]; then
    err1 "rpc-error"
fi

match=$(echo $ret | grep --null -Eo "No devices are selected") || true
if [ -z "$match" ]; then
    err1 "No devices are selected"
fi

# Reset controller
. ./reset-controller.sh

# 2) If local device fields are changed (except device mount-point - 'config')
local_change ${NAME} "AAA"

new "commit push, expect local fields change error"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>COMMIT</push>
    <source>ds:candidate</source>
  </controller-commit>
</rpc>]]>]]>
EOF
   )
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -z "$match" ]; then
    err1 "rpc-error"
fi

match=$(echo $ret | grep --null -Eo "local fields are changed") || true
if [ -z "$match" ]; then
    err1 "local fields are changed"
fi

new "local commit"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
)
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "OK reply"
fi

# 3) If a matching device is CLOSED UNLESS push=NONE
new "close $NAME"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <connection-change xmlns="http://clicon.org/controller">
      <devname>$NAME</devname>
      <operation>CLOSE</operation>
   </connection-change>
</rpc>]]>]]>
EOF
)
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "OK reply"
fi

new "commit push, expect device closed error"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>COMMIT</push>
    <source>ds:candidate</source>
  </controller-commit>
</rpc>]]>]]>
EOF
   )
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -z "$match" ]; then
    err1 "rpc-error"
fi

match=$(echo $ret | grep --null -Eo "No changes to push") || true
if [ -z "$match" ]; then
    err1 "Device is closed"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
