#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
# Start backend
# Commit a change to _devices_ remove x, change y, and add z
# Make a sync-pull dryrun
# Check the diff between controller and devices

set -eux

# Number of devices to add config to
: ${nr:=2}

# Set if also sync push, not only change
: ${push:=true}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

CFG=/usr/local/etc/controller.xml

# If set to 0, override starting of clixon_backend in test (you bring your own) 
: ${BE:=true}

# If set to false, dont start containers and controller, they are assumed to be already running
: ${INIT:=true}

if $INIT; then
    # Start devices
    nr=$nr ./stop-devices.sh
    sleep $sleep
    nr=$nr ./start-devices.sh
fi

# Start backend
BE=$BE nr=$nr ./init-controller.sh

# Add parameters x and y directly on devices
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    ret=$(sudo ssh $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
      <table xmlns="urn:example:clixon">
        <parameter nc:operation="remove">
          <name>x</name>
        </parameter>
        <parameter>
          <name>y</name>
          <value nc:operation="replace">122</value>
        </parameter>
        <parameter nc:operation="merge">>
          <name>z</name>
          <value>99</value>
        </parameter>
      </table>
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><commit/></rpc>]]>]]>
EOF
       )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
done

echo "trigger sync-pull dryrun"
ret=$(clixon_netconf -q0 -f $CFG <<EOF
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
    ret=$(clixon_netconf -q0 -f $CFG <<EOF
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


