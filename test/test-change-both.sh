#!/usr/bin/env bash
# Assume backend and devics running
# Reset devices and backend
# Commit a change to controller device config: remove x, change y, and add z
# Commit a change to _devices_ remove x, change y, and add z
# Push validate to devices which should fail
# Push commit to devices which should fail

set -eu

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    ${PREFIX} clixon_backend -s init -f $CFG -z

    echo "Start new backend"
    ${PREFIX} clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller 
. ./reset-controller.sh

# Change device configs on devices (not controller)
. ./change-devices.sh
i=1
# Change device in controller: Remove x, change y=322, and add z=399
for ip in $CONTAINERS; do
    NAME=$IMG$i
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
          <config>
            <table xmlns="urn:example:clixon">
              <parameter nc:operation="remove">
                <name>x</name>
              </parameter>
              <parameter>
                <name>y</name>
                <value nc:operation="replace">322</value>
               </parameter>
               <parameter nc:operation="merge">>
                 <name>z</name>
                 <value>399</value>
               </parameter>
            </table>
          </config>
        </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
          )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi

    i=$((i+1))  
done

new "local commit"
${PREFIX} ${clixon_netconf} -0 -f $CFG <<EOF
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

if ! $push ; then
    echo "Stop after changes, no push"
    echo OK
    exit 0
fi

new "push validate"
ret=$(${PREFIX} ${clixon_cli} -1f $CFG push validate 2>&1)
echo "ret:$ret"

match=$(echo $ret | grep --null -Eo "failed Device changed config") || true
if [ -z "$match" ]; then
    echo "Error msg not detected"
    exit 1
fi

if $BE; then
    echo "Kill old backend"
    ${PREFIX} clixon_backend -s init -f $CFG -z
fi

unset push

echo "test-change-both OK"

