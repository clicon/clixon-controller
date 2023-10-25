#!/usr/bin/env bash
# Assume backend and devices running
# Reset devices and backend
# Commit a change to controller device config: remove x, change y, and add z
# Commit a change to _devices_ add mtu (mtu is ignored by extension)
# Push validate to devices which should be OK
# Commit a change to _devices_ remove x, change y, and add z
# Push validate to devices which should fail
# Push commit to devices which should fail
# make a cli show devices check and diff

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dir=/var/tmp/$0
CFG=$dir/controller.xml
cfdir=$dir/conf.d
mntdir=$dir/mounts
test -d $cfdir || mkdir -p $cfdir
test -d $mntdir || mkdir -p $mntdir
fyang=$mntdir/clixon-ext@2023-11-01.yang

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$cfdir</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$dir</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>/usr/local/share/clixon/controller</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>/usr/local/var/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/controller.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/controller</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>www-data</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <CONTROLLER_YANG_SCHEMA_MOUNT_DIR xmlns="http://clicon.org/controller-config">$mntdir</CONTROLLER_YANG_SCHEMA_MOUNT_DIR>
</clixon-config>
EOF

cat <<EOF > $cfdir/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>false</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include openconfig system</name>
       <module-name>openconfig*</module-name>
       <operation>enable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-ext {
   namespace "http://clicon.org/ext";
   prefix cl-ext;
   import openconfig-interfaces{
      prefix oc-if;
   }
   import clixon-lib{
      prefix cl;
   }
   revision 2023-11-01{
      description "Initial prototype";
   }
   augment "/oc-if:interfaces/oc-if:interface/oc-if:config/oc-if:mtu" {
      cl:ignore-compare;
   }
}
EOF

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init  -f $CFG
fi

# Check backend is running
wait_backend

# Reset controller 
new "reset controller"
EXTRA="<module-set><module><name>clixon-ext</name><namespace>http://clicon.org/ext</namespace></module></module-set>" . ./reset-controller.sh

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface nc:operation="remove"><name>x</name></interface><interface><name>y</name><config><type nc:operation="replace" xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:v35</type></config></interface><interface nc:operation="merge"><name>z</name><config><name>z</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:tunnel</type></config></interface></interfaces>'

i=1
# Change device on controller: Remove x, change y=v35, and add z=tunnel
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "edit device $NAME directly"
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
        echo "$ret"
        exit 1
    fi

    i=$((i+1))  
done

new "local commit"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
)
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

if ! $push ; then
    echo "Stop after changes, no push"
    echo OK
    exit 0
fi

# Change devices only mtu (should be ok)
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "Set mtu on $NAME"
    ret=$(ssh $ip -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
    <default-operation>merge</default-operation>
    <config>
       <interfaces xmlns="http://openconfig.net/yang/interfaces">
          <interface>
             <name>x</name>
             <config>
                <mtu>999</mtu>
             </config>
          </interface>
       </interfaces>
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><commit/></rpc>]]>]]>
EOF
       )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        echo "$ret"
        exit 1
    fi
    i=$((i+1))
done

# Wanted to remove cli, but it is difficult since we have to wait for notification,
# rpc-reply is not enough. the cli commands are more convenient
new "push validate expected ok"
expectpart "$($clixon_cli -1f $CFG push validate 2>&1)" 0 "OK" --not-- "failed Device"

# Change device configs on devices (not controller)
new "change devices"
. ./change-devices.sh

# Wanted to remove cli, but it is difficult since we have to wait for notification,
# rpc-reply is not enough. the cli commands are more convenient
new "push validate expected fail"
expectpart "$($clixon_cli -1f $CFG push validate 2>&1)" 0 "failed Device"

NAME=${IMG}1
new "check if in sync (should not be)"
ret=$(${clixon_cli} -1f $CFG show devices $NAME check 2>&1)
match=$(echo $ret | grep --null -Eo "out-of-sync") || true
if [ -z "$match" ]; then
    err1 "out-of-sync"
fi

new "check device diff"
ret=$(${clixon_cli} -1f $CFG show devices $NAME diff 2>&1)
match=$(echo $ret | grep --null -Eo "+ <type>ianaift:v35</type>") || true
if [ -z "$match" ]; then
    echo "show devices diff does not match"
    exit 1
fi
match=$(echo $ret | grep --null -Eo "\- <type>ianaift:atm</type>") || true
if [ -z "$match" ]; then
    echo "show devices diff does not match"
    exit 1
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

unset push

endtest

