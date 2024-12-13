#!/usr/bin/env bash
# get device state
# via NETCONF device-template-apply get
# via CLI show device state

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml

dir=/var/tmp/$0
mntdir=$dir/mounts
test -d $mntdir || mkdir -p $mntdir

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller
new "reset controller"
(. ./reset-controller.sh)

# baseline
new "NETCONF: get controller state, baseline"
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
   <get>
      <filter type="xpath" select="/ctrl:devices/ctrl:device/ctrl:conn-state" xmlns:ctrl="http://clicon.org/controller" />
   </get>
</rpc>]]>]]>
EOF
      )

new "Check open"
match=$(echo $ret | grep --null -Eo "<device><name>openconfig1</name><conn-state>OPEN</conn-state></device>") || true
if [ -z "$match" ]; then
    err "openconfig1 OPEN" "$ret"
fi

new "NETCONF: get state with inline rpc template"
ret=$(${clixon_netconf} -0 -f $CFG <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="99">
   <device-template-apply xmlns="http://clicon.org/controller">
      <type>RPC</type>
      <devname>openconfig*</devname>
      <inline>
         <variables>
            <variable>
               <name>XPATH</name>
            </variable>
            <variable>
               <name>NAMESPACE</name>
            </variable>
         </variables>
         <config>
            <get xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
               <filter type="xpath" select="${XPATH}" xmlns:ns="${NAMESPACE}" />
            </get>
         </config>
      </inline>
      <variables>
         <variable>
            <name>NAMESPACE</name>
            <value>http://openconfig.net/yang/system</value>
         </variable>
         <variable>
            <name>XPATH</name>
            <value>/ns:system/ns:config</value>
         </variable>
      </variables>
   </device-template-apply>
</rpc>]]>]]>
EOF
)

new "Check ok"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "ok" "$ret"
fi

# It is complicated to wait for notify w netconf, skip that and do that for cli instead
new "CLI get device state"
expectpart "$($clixon_cli -1 -f $CFG show devices openconfig* state)" 0 '<devdata xmlns="http://clicon.org/controller">' "<name>openconfig1</name>" "<name>openconfig2</name>" '<system xmlns="http://openconfig.net/yang/system">'

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

