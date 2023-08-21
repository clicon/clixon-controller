#!/usr/bin/env bash
# Reset devices and backend
# Commit a change to _devices_ remove x, change y, and add z
# Make a pull transient
# Check the diff between controller and devices

set -eu

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend"
    sudo clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller 
. ./reset-controller.sh

# Change device configs on devices (not controller)
. ./change-devices.sh

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface><interface><name>z</name><config><name>z</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:usb</type></config></interface></interfaces><system xmlns="http://openconfig.net/yang/system">'

new "trigger pull transient"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <config-pull xmlns="http://clicon.org/controller">
    <devname>*</devname>
    <transient>true</transient>
  </config-pull>
</rpc>]]>]]>
EOF
      )
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    # verify controller 
    new "get and check transient device db"
    ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <get-device-config xmlns="http://clicon.org/controller">
    <devname>$NAME</devname>
    <config-type>TRANSIENT</config-type>
  </get-device-config>
</rpc>]]>]]>
EOF
      )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
    # XXX double <config><config>
    #    match=$(echo $ret | grep --null -Eo "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"43\"><config xmlns=\"http://clicon.org/controller\">$CONFIG</config></rpc-reply>") || true
    match=$(echo $ret | grep --null -Eo "$CONFIG") || true
    if [ -z "$match" ]; then
        echo "netconf rpc get-device-config failed"
        echo "$ret"
        exit 1
    fi
done

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

echo "test-device-diff"
echo OK
