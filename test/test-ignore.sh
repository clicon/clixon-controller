#!/usr/bin/env bash
# Test clixon-lib ignore-compare extension
# Mark a field : interface/config/mtu as "ignore"
# Test different cases as follows:
# 0. Field not present on neither device nor controller
# 1. Add mtu=888 to controller - ensure field not present on device
# 2. Add mtu=999 to device - ensure field is unchanged on device
# 3. Remove mtu on controller - ensure mtu is unchanged on device

# Commit a change to _devices_ add mtu (mtu is ignored by extension)

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
mntdir=$dir/mounts
test -d $CFD || mkdir -p $CFD
test -d $mntdir || mkdir -p $mntdir
fyang=$mntdir/clixon-ext@2023-11-01.yang

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>$mntdir</CLICON_YANG_DIR>
  <CLICON_YANG_DOMAIN_DIR>$mntdir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

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

# Sleep and verify devices are open
function sleep_open()
{
    jmax=10
    for j in $(seq 1 $jmax); do
        new "cli show connections and check open"
        ret=$($clixon_cli -1 -f $CFG -E $CFD show connections)
        match1=$(echo "$ret" | grep --null -Eo "openconfig1.*OPEN") || true
        match2=$(echo "$ret" | grep --null -Eo "openconfig2.*OPEN") || true
        if [ -n "$match1" -a -n "$match2" ]; then
            break;
        fi
        echo "retry after sleep"
        sleep 1
    done
    if [ $j -eq $jmax ]; then
        err "device openconfig OPEN" "Timeout" 
    fi
}

# Set and commit mtu=999 on device
function device_mtu_set()
{
    ip=$1
    mtu=$2

    new "Edit mtu on $ip"
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
      <interfaces xmlns="http://openconfig.net/yang/interfaces">
        <interface>
          <name>y</name>
          <config>
            <mtu nc:operation="replace">$mtu</mtu>
          </config>
        </interface>
      </interfaces>
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
        err1 "$ret"
    fi
}

# Set and commit mtu=999 on device
# args:
# 1: ip
# 2: mtu
function device_mtu_get()
{
    ip=$1
    mtu=$2

    REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces/interface/mtu"/>'
    
    new "Get mtu on $ip"
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
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "$ret"
    fi
    if [ -n "$mtu" ]; then
        match=$(echo "$ret" | grep --null -Eo "<mtu>$mtu</mtu>") || true
        if [ -z "$match" ]; then
            err "<mtu>999</mtu>" "$ret"
        fi
    else
        match=$(echo "$ret" | grep --null -Eo "<mtu>") || true
        if [ -n "$match" ]; then
            err "No <mtu>" "$ret"
        fi
    fi
}

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -E $CFD -z

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "wait backend"
wait_backend

# Reset controller 
new "reset controller"
EXTRA="<module-set><module><name>clixon-ext</name><namespace>http://clicon.org/ext</namespace></module></module-set>" . ./reset-controller.sh

new "Sleep and verify devices are open"
sleep_open

new "pull"
expectpart "$($clixon_cli -1f $CFG -E $CFD pull 2>&1)" 0 "OK"

new "check sync OK 1"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

NAME=${IMG}1
for ip in $CONTAINERS; do # Just to get first element
    break
done

new "1. Add mtu=888 to controller"

new "Edit mtu field on controller"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device $NAME config interfaces interface y config mtu 888)" 0 "^$"

device_mtu_get $ip ""

new "Commit diff"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit diff)" 0 --not-- "^-\ *" "^+\ *"

new "Commit local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local)" 0 "^$"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

new "Commit push"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit push)" 0 "^$"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

device_mtu_get $ip ""

new "2. Add mtu=999 to device"

device_mtu_set $ip 999

device_mtu_get $ip 999

new "Edit mtu field on controller"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device $NAME config interfaces interface y config mtu 777)" 0 "^$"

new "Commit diff"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit diff)" 0 --not-- "^-\ *" "^+\ *"

new "Commit local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local)" 0 "^$"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

new "Commit push"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit push)" 0 "^$"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

device_mtu_get $ip 999

new "Edit description field"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device $NAME config interfaces interface y config description \"New description\")" 0 "^$"

new "Commit diff"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit diff)" 0 "^+\ *<description>New description</description>" --not-- "^-\ *"

new "Commit push"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit push)" 0 "^$"

device_mtu_get $ip 999

new "Check controller is still 777"
expectpart "$($clixon_cli -1f $CFG -E $CFD show config devices device openconfig1 config interfaces interface y config mtu)" 0 "<mtu>777</mtu>"

new "Pull"
expectpart "$($clixon_cli -1f $CFG -E $CFD pull)" 0 "^$"

new "Check controller is 999"
expectpart "$($clixon_cli -1f $CFG -E $CFD show config devices device openconfig1 config interfaces interface y config mtu)" 0 "<mtu>999</mtu>"

device_mtu_get $ip 999

new "3. Remove mtu on controller"
new "Delete mtu field on controller"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure delete devices device $NAME config interfaces interface y config mtu 999)" 0 "^$"

new "Commit diff"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit diff)" 0 --not-- "^-\ *" "^+\ *"

new "Commit local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local)" 0 "^$"

new "Check controller is gone"
expectpart "$($clixon_cli -1f $CFG -E $CFD show config devices device openconfig1 config interfaces interface y config mtu)" 0 --not-- "<mtu>"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

new "Commit push"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit push)" 0 "^$"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG -E $CFD show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

device_mtu_get $ip 999

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
