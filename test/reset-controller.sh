#!/usr/bin/env bash
# Reset controller by initiating with clixon/openconfig devices and a pull
# Only use netconf (not cli)

set -u

echo "reset-controller"

# Set if delete old config
: ${delete:=true}

# Set if also check, which only works for clixon-example
: ${check:=true}

# Default values for controller device settings
: ${description:="Clixon example container"}
: ${yang_config:=VALIDATE}
: ${USER:=root}
REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'
# see reset-devices
CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces>'

# Send edit-config to controller with initial device meta-config
function init_device_config()
{
    NAME=$1
    ip=$2

    echo "Init device $NAME edit-config"
    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
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
	  <description>$description</description>
	  <conn-type>NETCONF_SSH</conn-type>
	  <user>$USER</user>
	  <addr>$ip</addr>
	  <yang-config>${yang_config}</yang-config>
	  <config/>
	</device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
       )
}

if $delete ; then
    echo "Delete device config"
    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
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
fi # delete

i=1
# Loop adding top-level device meta info
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    i=$((i+1))
    jmax=5
    for j in $(seq 1 $jmax); do
	init_device_config $NAME $ip
#	echo "$ret"
	match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
	if [ -z "$match" ]; then
	    break
	fi
	match=$(echo "$ret" | grep --null -Eo "<error-tag>lock-denied</error-tag") || true
	if [ -z "$match" ]; then
	    echo "netconf rpc-error detected"
	    exit 1
	fi
	echo "retry after sleep"
	sleep $sleep
    done
done

echo "Controller commit"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
      )
#echo "$ret"

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

echo "Send rpc connection-change OPEN"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <connection-change xmlns="http://clicon.org/controller">
      <devname>*</devname>
      <operation>OPEN</operation>
   </connection-change>
</rpc>]]>]]>
EOF
   )

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf open rpc-error detected"
    exit 1
fi

sleep $sleep
jmax=5
for j in $(seq 1 $jmax); do
    new "Verify open devices 1"
    ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <get cl:content="all" xmlns:cl="http://clicon.org/lib">
      <nc:filter nc:type="xpath" nc:select="co:devices/co:device/co:conn-state" xmlns:co="http://clicon.org/controller"/>
   </get>
</rpc>]]>]]>
EOF
   )
    #echo "$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "Error: $ret"
    fi

    res=$(echo "$ret" | sed 's/OPEN/OPEN\n/g' | grep "$IMG" | grep -c "OPEN") || true
    if [ "$res" != "$nr" ]; then
        echo "retry after sleep"
        sleep $sleep
    fi
    break

done # verify open
if [ $j -eq $jmax ]; then
    err "$nr devices open" "$res devices open"
fi

echo "Netconf pull"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
	<config-pull xmlns="http://clicon.org/controller">
	 <devname>*</devname>
   </config-pull>
</rpc>]]>]]>
EOF
   )
#echo "ret:$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "Error: $ret"
    exit -1;
fi

echo "Verify open devices 2"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <get cl:content="all" xmlns:cl="http://clicon.org/lib">
      <nc:filter nc:type="xpath" nc:select="co:devices/co:device/co:conn-state" xmlns:co="http://clicon.org/controller"/>
   </get>
</rpc>]]>]]>
EOF
   )
#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "Error: $res"
    exit -1;
fi

res=$(echo "$ret" | sed 's/OPEN/OPEN\n/g' | grep "$IMG" | grep -c "OPEN") || true
if [ "$res" != "$nr" ]; then
    echo "Error: $res"
    exit -1;
fi
# Early exit point, do not check pulled config
if ! $check ; then
    echo "reset-controller early exit: do not check result"
    echo OK
    exit 0
fi

i=1
# Only works for openconfig, others should set check=false
echo "check config"
for ip in $CONTAINERS; do
    echo "Check config on device$i"
    NAME=$IMG$i
    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
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
	  <name>$NAME</name>
	  <config>
            ${REQ}
	  </config>
	</device>
      </devices>
    </filter>
      </get-config>
</rpc>]]>]]>
EOF
       )
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	echo "netconf rpc-error detected"
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

echo "reset-controller OK"
