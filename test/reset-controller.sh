#!/usr/bin/env bash
# Reset controller by initiating with clixon/openconfig devices and a pull
# Only use netconf (not cli)

set -u

# Set if delete old config
: ${delete:=true}

# Default values for controller device settings
: ${description:="Clixon example container"}
: ${yang_config:=VALIDATE}
: ${USER:=root}
: ${nrif:=2}

: ${EXTRA:=} # Extra top-level device config

REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'
# see reset-devices
CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces">'
CONFIG+='<interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config></interface>'
CONFIG+='<interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface>'

for i in $(seq 3 $nrif); do
    CONFIG+="<interface><name>x$i</name><config><name>x$i</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type><description></description></config></interface>"
done
CONFIG+='</interfaces>'

# Send edit-config to controller with initial device meta-config
function init_device_config()
{
    NAME=$1
    ip=$2

    new "reset-controller: Init device $NAME edit-config"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
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
	  <private-candidate>false</private-candidate>
          ${EXTRA}
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
    new "reset-controller: Delete device config"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
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
    # echo "ret: $ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	err "netconf delete" "$ret"
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
	# echo "$ret"
	match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
	if [ -z "$match" ]; then
	    break
	fi
	match=$(echo "$ret" | grep --null -Eo "<error-tag>lock-denied</error-tag") || true
	if [ -z "$match" ]; then
	    err1 "netconf rpc-error detected"
	fi
	new "reset-controller: retry after sleep"
	sleep $sleep
    done
done

new "reset-controller: Controller commit"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
      )
#echo "$ret"

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "OK" "$ret"
fi

new "reset-controller: Send rpc connection-change OPEN"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <connection-change xmlns="http://clicon.org/controller">
      <device>*</device>
      <operation>OPEN</operation>
   </connection-change>
</rpc>]]>]]>
EOF
   )
#echo "ret:$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "OK" "$ret"
fi

sleep $sleep
jmax=5
for j in $(seq 1 $jmax); do
    new "reset-controller: Verify open devices 1"
    ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
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
        new "reset-controller: retry after sleep"
        sleep $sleep
        continue
    fi
    break
done # verify open
if [ $j -eq $jmax ]; then
    err "$nr devices open" "$res devices open"
fi

new "reset-controller: Netconf pull"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
	<config-pull xmlns="http://clicon.org/controller">
	 <device>*</device>
   </config-pull>
</rpc>]]>]]>
EOF
   )
#echo "ret:$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "Error: $ret"
fi

new "reset-controller: Verify open devices 2"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
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
    err1 "Error: $res"
fi

res=$(echo "$ret" | sed 's/OPEN/OPEN\n/g' | grep "$IMG" | grep -c "OPEN") || true
if [ "$res" != "$nr" ]; then
    err1 "Error: $res"
fi

new "reset-controller 4"
i=1
# Only works for openconfig, others should set check=false
for ip in $CONTAINERS; do
    new "reset-controller: Check config on device$i"
    NAME=$IMG$i
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
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
	err1 "netconf rpc-error detected"
    fi
    match=$(echo "$ret" | grep --null -Eo "$CONFIG") || true
    if [ -z "$match" ]; then
	err  "$CONFIG" "$ret"
    fi

    i=$((i+1))
done

echo "reset-controller OK"
