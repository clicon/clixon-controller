#!/usr/bin/env bash
# Reset controller by initiaiting with clixon-example devices and a sync pull

set -eux

echo "reset-controller"

# Controller config file
: ${CFG:=/usr/local/etc/controller.xml}

# Number of devices to add config to
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

: ${clixon_cli:=clixon_cli}

: ${clixon_netconf:=$(which clixon_netconf)}

# Prefix to add in front of all client commands.
# Eg to force all client to run as root if there is problem with group assignment (see github actions)
: ${PREFIX:=}

echo "Delete device config"
ret=$(${PREFIX} ${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
  message-id="42">
  <edit-config>
    <target>
      <candidate/>
    </target>
    <default-operation>none</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller" nc:operation="remove">
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi      

# This script adds deletes x, modifies y, and adds z
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    
    echo "Init config for device$i edit-config"
    ret=$(${PREFIX} ${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
  message-id="42">
  <edit-config>
    <target>
      <candidate/>
    </target>
    <default-operation>none</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller">
        <device nc:operation="replace">
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
ret=$(${PREFIX} ${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
      )
echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi
sleep $sleep

echo "sync pull"
${PREFIX} ${clixon_cli} -1f $CFG sync pull

sleep $sleep

echo "check open"
res=$(${PREFIX} ${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)
if [ "$res" != "$nr" ]; then
   echo "Error: $res"
   exit -1;
fi

echo "check config"
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    
    echo "Check config on device$i"
    ret=$(${PREFIX} ${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
  message-id="42">
  <get-config>
    <source>
      <candidate/>
    </source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
        <device>
          <name>clixon-example1</name>
          <root>
            <table xmlns="urn:example:clixon"/>
          </root>
        </device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
    echo "$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
    match=$(echo "$ret" | grep --null -Eo "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>") || true
    if [ -z "$match" ]; then
        echo "Config of device $i not matching"
        exit 1
    fi
done

echo "reset-controller OK"
