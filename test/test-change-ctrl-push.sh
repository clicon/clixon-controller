#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
# Start backend
# Commit a change to devices (on controller): remove x, change y, and add z
# Push to devices
# Check the change on the devices

set -eux

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also sync push, not only change
: ${push:=true}

# Reset devices with initial config
. ./reset-devices.sh

# Reset backend with 
. ./reset-backend.sh

# Remove x, change y, and add z
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    
    ret=$(clixon_netconf -0 -f $CFG <<EOF
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
          <root>
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
          </root>
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
done

echo "local commit"
clixon_netconf -0 -f $CFG <<EOF
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
    echo "Stop after change, no push"
    echo OK
    exit 0
fi

echo "push sync"
ret=$(clixon_netconf -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <sync-push xmlns="http://clicon.org/controller">
  </sync-push>
</rpc>]]>]]>
EOF
      )

echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

# XXX get transaction-id from ret and wait for that?

sleep $sleep

# Verify controller
res=$(clixon_cli -1f $CFG show devices | grep OPEN | wc -l)
if [ "$res" = "$nr" ]; then
   echo OK
else
   echo Error
   exit -1;
fi

# Verify containers
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    ret=$(clixon_netconf -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
message-id="42">
  <get-config>
    <source><running/></source>
  </get-config>
</rpc>]]>]]>
EOF
       )
    echo "ret:$ret"
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
    match=$(echo $ret | grep --null -Eo '<root><table xmlns="urn:example:clixon"><parameter><name>y</name><value>122</value></parameter><parameter><name>z</name><value>99</value></parameter></table></root></device><device><name>clixon-example2</name><description>Clixon example container</description><enabled>true</enabled><conn-type>NETCONF_SSH</conn-type><user>root</user><addr>172.17.0.3</addr><yang-config>VALIDATE</yang-config><root><table xmlns="urn:example:clixon"><parameter><name>y</name><value>122</value></parameter><parameter><name>z</name><value>99</value></parameter></table></root>') || true
    if [ -z "$match" ]; then
        echo "netconf rpc get-config failed"
        exit 1
    fi
done
echo OK
