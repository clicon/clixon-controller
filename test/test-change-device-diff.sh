#!/usr/bin/env bash
# Assume backend and devics running
# Reset devices and backend
# Commit a change to _devices_ remove x, change y, and add z
# Make a sync-pull dryrun
# Check the diff between controller and devices

set -eux

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Reset devices with initial config
. ./reset-devices.sh

# Check backend is running
wait_backend

# Reset controller 
. ./reset-controller.sh

# Change device configs on devices (not controller)
. ./change-devices.sh

echo "trigger sync-pull dryrun"
ret=$(${PREFIX} ${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <sync-pull xmlns="http://clicon.org/controller">
    <devname>*</devname>
    <dryrun>true</dryrun>
  </sync-pull>
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
    echo "get and check dryrun device db"
    ret=$(${PREFIX} ${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <get-device-config xmlns="http://clicon.org/controller">
    <devname>$NAME</devname>
    <extended>dryrun</extended>
  </get-device-config>
</rpc>]]>]]>
EOF
      )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
    match=$(echo $ret | grep --null -Eo '<rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43"><config xmlns="http://clicon.org/controller"><root><table xmlns="urn:example:clixon"><parameter><name>y</name><value>122</value></parameter><parameter><name>z</name><value>99</value></parameter></table></root></config></rpc-reply>') || true
    if [ -z "$match" ]; then
        echo "netconf rpc get-device-config failed"
        exit 1
    fi
done

echo "device-diff"
echo OK


