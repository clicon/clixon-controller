#!/usr/bin/env bash
# Reset running example container devices and initiate with config x=11, y=22
set -u

new "run $0"

# Set if also check, which only works for clixon-example
: ${dir:=/var/tmp}
: ${nrif:=2}

# Netconf monitoring on device config (affects clients not backend)
: ${NETCONF_MONITORING:=true}

# Data

# Initial config: Define two interfaces x and y
REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces">'
CONFIG+='<interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config></interface>'
CONFIG+='<interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface>'

for i in $(seq 3 $nrif); do
    CONFIG+="<interface><name>x$i</name><config><name>x$i</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>"
done
CONFIG+='</interfaces>'

# Reset devices backends with RFC 6022 enabled
cat <<EOF > $dir/extra.xml
<clixon-config xmlns="http://clicon.org/config">
   <CLICON_NETCONF_MONITORING>${NETCONF_MONITORING}</CLICON_NETCONF_MONITORING>
</clixon-config>
EOF

new "reset-devices: Add hostname"
i=1
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    new "set hostname $NAME $ip"
    ret=$(ssh ${SSHID} -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    r=$?
    if [ $r -ne 0 ]; then
        err1 "0" "$r"
        exit 1
    fi
    echo "ret:$ret"
    if [ -z "$ret" ]; then
        err1 "rpc-reply" "No reply (have you started device $NAME?)"
        exit 1
    fi
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf system rpc-error detected"
        echo "$ret"
        exit 1
    fi
    i=$((i+1))
    dockerbin=$(which docker) || true
    if [ -n "$dockerbin" ]; then
        new "Set CLICON_NETCONF_MONITORING"
        sudo docker cp -q $dir/extra.xml $NAME:/usr/local/etc/clixon/openconfig/extra.xml
    fi
done

new "reset-devices: Init config"
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "init config: $NAME"
    ret=$(ssh ${SSHID} -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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

new "reset-devices: Check config"
# Check config
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "Check config $NAME"
    ret=$(ssh ${SSHID} -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    i=$((i+1))
done

new "reset-devices OK"
