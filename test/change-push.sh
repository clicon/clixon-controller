#!/usr/bin/env bash
# Commit a change to devices: remove x, change y, and add z and push to devices
set -eux

# Number of devices to add config to
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

CFG=/usr/local/etc/controller.xml

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    
    ret=$(clixon_netconf -q0 -f $CFG <<EOF
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
    echo "match:<$match>"
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
done

echo "local commit"
clixon_netconf -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF

echo "push sync"
clixon_netconf -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <sync-push xmlns="http://clicon.org/controller"/>
</rpc>]]>]]>
EOF

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
