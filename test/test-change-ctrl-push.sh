#!/usr/bin/env bash
# Reset devices and backend
# Commit a change to controller device config: remove x, change y, and add z
# Push validate to devices
# Push to devices

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

# Set if also push commit, not only push validate
: ${commit:=true}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

# Data 
REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'
CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface nc:operation="remove"><name>x</name></interface><interface><name>y</name><config><type nc:operation="replace" xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:x213</type></config></interface><interface nc:operation="merge"><name>z</name><config><name>z</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:vdsl</type></config></interface></interfaces>'

# XXX xmlns iana ns decl is on config once and on type the other
CHECK='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>y</name><config xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type"><name>y</name><type>ianaift:x213</type></config></interface><interface><name>z</name><config><name>z</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:vdsl</type></config></interface></interfaces>'

i=1
# Change device in controller: Remove x, change y=x213, and add z=vdsl
for x in $CONTAINERS; do
    NAME=$IMG$i
    ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
	  <config>
            ${CONFIG}
	  </config>
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

    i=$((i+1))
done

new "local commit"
${clixon_netconf} -0 -f $CFG > /dev/null <<EOF
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

new "push validate"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>VALIDATE</push>
    <source>ds:running</source>
  </controller-commit>
</rpc>]]>]]>
EOF
   )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

# XXX get transaction-id from ret and wait for that?

sleep $sleep

new "push commit"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>COMMIT</push>
    <source>ds:running</source>
  </controller-commit>
</rpc>]]>]]>
EOF
      )

# echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

if ! $commit ; then
    echo "Stop after push validate, no commit"
    echo OK
    exit 0
fi

sleep $sleep

new "Verify controller"
res=$(${clixon_cli} -1f $CFG show connections | grep OPEN | wc -l)

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
res=$(echo "$ret" | sed 's/OPEN/OPEN\n/g' | grep -c "OPEN")

if [ "$res" != "$nr" ]; then
    echo "Error: $res"
    exit -1;
fi

i=1
for x in $CONTAINERS; do
    NAME=$IMG$i
    new "Check $NAME"

    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
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
#    echo "ret:$ret"
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	echo "netconf rpc-error detected on $NAME"
	exit 1
    fi
    match=$(echo $ret | grep --null -Eo "<config>${CHECK}</config>") || true
    if [ -z "$match" ]; then
	echo "netconf rpc get-config $NAME no match"
        echo "$ret" 
	exit 1
    fi

    i=$((i+1))
done

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

unset push
unset commit

endtest
