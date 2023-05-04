#!/usr/bin/env bash
# Reset running example container devices and initiate with config x=11, y=22
set -e

echo "reset-devices"

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

: ${SSHKEY:=/root/.ssh/id_rsa.pub}

test -f $SSHKEY || ssh-keygen -t rsa -N "" -f /root/.ssh/id_rsa

# Add parameters x and y
for ip in $CONTAINERS; do
    ret=$(ssh $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    <target>
      <candidate/>
    </target>
    <default-operation>none</default-operation>
    <config>
      <table xmlns="urn:example:clixon" nc:operation="replace">
        <parameter>
          <name>x</name>
          <value>11</value>
        </parameter>
        <parameter>
          <name>y</name>
          <value>22</value>
        </parameter>
      </table>
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <commit/>
</rpc>]]>]]>
EOF
       )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        exit 1
    fi
done

echo "reset-devices OK"
