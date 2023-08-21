#!/usr/bin/env bash
# Assume backend and devices running
# Reset devices and backend
# Commit a change to controller device config: remove x, change y, and add z
# Commit a change to _devices_ remove x, change y, and add z
# Push validate to devices which should fail
# Push commit to devices which should fail
# make a cli show devices check and diff

set -eu

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend"
    sudo clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller 
new "reset controller"
. ./reset-controller.sh

# Change device configs on devices (not controller)
new "change devices"
. ./change-devices.sh

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface nc:operation="remove"><name>x</name></interface><interface><name>y</name><config><type nc:operation="replace" xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:v35</type></config></interface><interface nc:operation="merge"><name>z</name><config><name>z</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:tunnel</type></config></interface></interfaces>'

i=1
# Change device out-of-band directly: Remove x, change y=v35, and add z=tunnel
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "edit device $NAME directly"
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
            ${CONFIG}
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
        echo "$ret"
        exit 1
    fi

    i=$((i+1))  
done

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
    echo "netconf rpc-error detected"
    exit 1
fi

if ! $push ; then
    echo "Stop after changes, no push"
    echo OK
    exit 0
fi

# Wanted to remove cli, but it is difficult since we have to wait for notification,
# rpc-reply is not enough. the cli commands are more convenient
new "push validate"
ret=$(${clixon_cli} -1f $CFG push validate 2>&1)
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "failed Device") || true
if [ -z "$match" ]; then
    echo "Error msg not detected"
    exit 1
fi

NAME=${IMG}1
new "check if in sync (should not be)"
ret=$(${clixon_cli} -1f $CFG show devices $NAME check 2>&1)
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "out-of-sync") || true
if [ -z "$match" ]; then
    echo "Is in sync but should not be"
    exit 1
fi

new "check device diff"
ret=$(${clixon_cli} -1f $CFG show devices $NAME diff 2>&1)
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "+ <type>ianaift:v35</type>") || true
if [ -z "$match" ]; then
    echo "show devices diff does not match"
    exit 1
fi
match=$(echo $ret | grep --null -Eo "\- <type>ianaift:atm</type>") || true
if [ -z "$match" ]; then
    echo "show devices diff does not match"
    exit 1
fi

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

unset push

echo "test-change-both OK"

