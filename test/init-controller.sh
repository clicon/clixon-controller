#!/usr/bin/env bash
# Init the controller
# Prereq: run start-devices.sh with the same <nr>

set -eux

# Number of devices to add config to
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

# If set to 0, override starting of clixon_backend in test (you bring your own) 
: ${BE:=1}

IMG=clixon-example

CFG=/usr/local/etc/controller.xml

if [ $BE -ne 0 ]; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend"
    sudo clixon_backend -s init  -f $CFG -D0

    sleep $sleep
fi

# Startup contains x and y
# This script adds deletes x, modifies y, and adds z
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    
    echo "Init config for device$i edit-config"
    ret=$(clixon_netconf -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller">
        <device nc:operation="create">
          <name>$NAME</name>
          <enabled>true</enabled>
          <description>Clixon example container</description>
          <conn-type>NETCONF_SSH</conn-type>
          <user>root</user>
          <addr>$ip</addr>
          <yang-config>VALIDATE</yang-config>
          <root/>
        </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
       )

    echo "$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
done

echo "controller commit"
clixon_netconf -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF

sleep $sleep

res=$(clixon_cli -1f $CFG show devices | grep OPEN | wc -l)
if [ "$res" != "$nr" ]; then
   echo "Error: $res"
   exit -1;
fi
echo OK
