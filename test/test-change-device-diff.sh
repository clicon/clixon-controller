#!/usr/bin/env bash
# Assume backend and devics running
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

echo "trigger pull transient"
ret=$(${PREFIX} ${clixon_netconf} -q0 -f $CFG <<EOF
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
    echo "get and check transient device db"
    ret=$(${PREFIX} ${clixon_netconf} -q0 -f $CFG <<EOF
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
    # XXX should it really be double: config/config?
    match=$(echo $ret | grep --null -Eo '<rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43"><config xmlns="http://clicon.org/controller"><config><table xmlns="urn:example:clixon"><parameter><name>y</name><value>122</value></parameter><parameter><name>z</name><value>99</value></parameter></table></config></config></rpc-reply>') || true
    if [ -z "$match" ]; then
        echo "netconf rpc get-device-config failed"
        exit 1
    fi
done

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

echo "test-device-diff"
echo OK
