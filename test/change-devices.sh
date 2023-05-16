#!/usr/bin/env bash
# Change device config: Remove x, change y, and add z
set -eu

echo "change-devices"

: ${PREFIX:=}

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

: ${SSHKEY:=/root/.ssh/id_rsa.pub}

${PREFIX} test -f $SSHKEY || ${PREFIX} ssh-keygen -t rsa -N "" -f /root/.ssh/id_rsa

# Remove x, change y, and add z directly on devices
for ip in $CONTAINERS; do
    ret=$(${PREFIX} ssh $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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

echo "change-devices OK"
