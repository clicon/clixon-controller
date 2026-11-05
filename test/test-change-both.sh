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

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
mntdir0=$dir/mounts
mntdir=$mntdir0/default
test -d $CFD || mkdir -p $CFD
test -d $mntdir || mkdir -p $mntdir
fyang=$mntdir/clixon-ext@2023-11-01.yang

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>$dir</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>${DATADIR}/controller/main</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$mntdir0</CLICON_YANG_DOMAIN_DIR>
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

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "wait backend"
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
    ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
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
        err "netconf rpc-error detected" "$ret"
    fi

    i=$((i+1))  
done

new "local commit"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
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
    err1 "netconf rpc-error detected"
fi

# Change devices only mtu (mtu is ignored by extension)
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    new "Set mtu on $NAME"
    ret=$(ssh $ip ${SSHID} -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
        err1 "netconf rpc-error detected"
    fi
    i=$((i+1))
done

# Capture controller-transaction progress notifications emitted
# during a push, to check progress-notification invariants independent of
# device timing.
# Subscribe in the background to the controller-transaction stream, holding stdin open via a fifo.
NFIFO=$dir/progress-in.fifo
NOUT=$dir/progress-out.xml
NHELLO="<?xml version=\"1.0\" encoding=\"UTF-8\"?><hello $DEFAULTONLY><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>]]>]]>"

progress_capture_start()
{
    rm -f $NFIFO $NOUT
    mkfifo $NFIFO
    new "push validate expected ok (capture progress)"
    $clixon_netconf -q0f $CFG -E $CFD < $NFIFO > $NOUT 2>/dev/null &
    NCPID=$!
    exec 8>$NFIFO
    echo "$NHELLO" >&8
    echo "<rpc $DEFAULTNS><create-subscription xmlns=\"urn:ietf:params:xml:ns:netmod:notification\"><stream>controller-transaction</stream></create-subscription></rpc>]]>]]>" >&8
    # Wait for subscription rpc-reply to avoid losing early notifications
    for j in $(seq 1 25); do
        grep -q "rpc-reply" $NOUT 2>/dev/null && break
        sleep 0.2
    done
}

progress_capture_stop()
{
    sleep 2       # let trailing async notifications flush
    exec 8>&-     # close fifo write-end -> EOF subscription session
    wait $NCPID 2>/dev/null
    # Normalize: one notification per line (strip EOM markers and newlines)
    tr -d '\r\n' < $NOUT | sed 's/]]>]]>/\n/g' > $NOUT.lines
    grep 'controller-transaction-progress' $NOUT.lines > $NOUT.progress || true
}

progress_capture_start

new "Push validate"
expectpart "$($clixon_cli -1f $CFG -E $CFD push validate 2>&1)" 0 "^$" --not-- "failed Device"

progress_capture_stop

new "progress: notifications received"
if [ ! -s $NOUT.progress ]; then
    err "controller-transaction-progress notifications" "$(cat $NOUT 2>/dev/null)"
fi

# Every progress notification that lists any device must list
# all push participants (devices added atomically, none appears late)
new "progress: complete device membership (issue #263)"
viol=$(awk -v d1="<name>${IMG}1</name>" -v d2="<name>${IMG}2</name>" '
    /<device>/ { if (index($0,d1)==0 || index($0,d2)==0) c++ }
    END { print c+0 }' $NOUT.progress)
if [ "$viol" != "0" ]; then
    err "0 partial-membership progress notifications" "$viol ($(cat $NOUT.progress))"
fi

# No two consecutive progress notifications are identical
new "progress: no consecutive duplicate notifications (issue #263)"
sed 's/.*\(<controller-transaction-progress.*<\/controller-transaction-progress>\).*/\1/' $NOUT.progress > $NOUT.bodies
dup=$(awk '$0==prev{c++} {prev=$0} END{print c+0}' $NOUT.bodies)
if [ "$dup" != "0" ]; then
    err "0 consecutive duplicate progress notifications" "$dup ($(cat $NOUT.bodies))"
fi

# Devices that finish report their result once they leave the transaction (here both succeed)
new "progress: finished devices report result (issue #263)"
match=$(grep -o "<result>SUCCESS</result>" $NOUT.progress | head -1) || true
if [ -z "$match" ]; then
    err "a progress notification with <result>SUCCESS</result>" "$(cat $NOUT.progress)"
fi

# Change device configs on devices (not controller)
new "change devices"
. ./change-devices.sh

# Wanted to remove cli, but it is difficult since we have to wait for notification,
# rpc-reply is not enough. the cli commands are more convenient
new "push validate expected fail"
expectpart "$($clixon_cli -1f $CFG -E $CFD push validate 2>&1)" 0 "Transaction [0-9]* failed"

# Out-of-sync devices fail at PUSH-CHECK; are recorded as FAILED
new "transaction detail: failed devices recorded FAILED (issue #262)"
ret=$($clixon_cli -1f $CFG -E $CFD show transactions detail 2>&1)
match=$(echo "$ret" | grep -E "^    ${IMG}[12][[:space:]]+SUCCESS") || true
if [ -n "$match" ]; then
    err "failed device not reported SUCCESS" "$ret"
fi
match=$(echo "$ret" | grep -E "^    ${IMG}1[[:space:]]+FAILED") || true
if [ -z "$match" ]; then
    err "device ${IMG}1 FAILED in transaction detail" "$ret"
fi
match=$(echo "$ret" | grep -E "^    ${IMG}2[[:space:]]+FAILED") || true
if [ -z "$match" ]; then
    err "device ${IMG}2 FAILED in transaction detail" "$ret"
fi

NAME=${IMG}1
new "check if in sync (should not be)"
ret=$(${clixon_cli} -1f $CFG -E $CFD show devices $NAME check 2>&1)
match=$(echo $ret | grep --null -Eo "out-of-sync") || true
if [ -z "$match" ]; then
    err1 "out-of-sync"
fi

new "check device diff"
ret=$(${clixon_cli} -1f $CFG -E $CFD pull $NAME diff 2>&1)
match=$(echo $ret | grep --null -Eo "+ <type>ianaift:v35</type>") || true
if [ -z "$match" ]; then
    err "+ <type>ianaift:v35</type>" "$dir"
fi
match=$(echo $ret | grep --null -Eo "\- <type>ianaift:atm</type>") || true
if [ -z "$match" ]; then
    err "- <type>ianaift:atm</type>" "$ret"
fi

# Cannot pull if edits in candidate
new "edit local candidate"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure delete devices device ${IMG}1 config interfaces interface z)" 0 "^$"

new "pull dont expect error"
expectpart "$($clixon_cli -1f $CFG -E $CFD pull replace 2>&1)" 0 "^$"

new "discard"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure discard)" 0 "^$"

new "pull again"
expectpart "$($clixon_cli -1f $CFG -E $CFD pull replace 2>&1)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

sudo rm -rf $dir
endtest

