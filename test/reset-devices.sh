#!/usr/bin/env bash
# Reset running example container devices and initiate with config x=11, y=22
set -u

echo "reset-devices"

# Set if also check, which only works for clixon-example
: ${check:=true}
: ${dir:=/var/tmp}

# Netconf monitoring on device config (affects clients not backend)
: ${NETCONF_MONITORING:=true}

# Data

# Initial config: Define two interfaces x and y
REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces>'
CHECK='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces>'

# Reset devices backends with RFC 6022 enabled
cat <<EOF > $dir/extra.xml
<clixon-config xmlns="http://clicon.org/config">
   <CLICON_NETCONF_MONITORING>${NETCONF_MONITORING}</CLICON_NETCONF_MONITORING>
</clixon-config>
EOF

# Add hostname
i=1
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    echo "set hostname $NAME $ip"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    <default-operation>replace</default-operation>
    <config>
       <system xmlns="http://openconfig.net/yang/system">
          <config>
             <hostname>${NAME}</hostname>
          </config>
       </system>
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
        echo "netconf system rpc-error detected"
        echo "$ret"
        exit 1
    fi
    i=$((i+1))
done

# Early exit point, do not check pulled config
if ! $check ; then
    echo "reset-controller early exit: do not check result"
    echo OK
    exit 0
fi

i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    echo "init config: $NAME"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    <default-operation>merge</default-operation>
    <config>
      $CONFIG
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
        echo "netconf interfaces rpc-error detected"
        exit 1
    fi
    i=$((i+1))
done

# Check config
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    echo "Check config $NAME"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
  message-id="42">
  <get-config>
    <source>
      <running/>
    </source>
    <filter type='subtree'>
      ${REQ}
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#    echo "ret:$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	echo "netconf rpc-error detected"
        echo "$ret"
	exit 1
    fi
    match=$(echo "$ret" | grep --null -Eo "$CONFIG") || true
    if [ -z "$match" ]; then
	echo "Config of device $i not matching:"
	echo "$ret"
        echo "Expected:"
        echo "$CONFIG"
	exit 1
    fi
    i=$((i+1))
done

echo "reset-devices OK"
