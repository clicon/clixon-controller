#!/usr/bin/env bash
# Ensure a device-sync timeout does not discards an otherwise completed pull,
# resulting in a stale device config in running
# See https://github.com/clicon/clixon-controller/issues/253

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml

# The bug requires >1 device in the pull transaction: with a single device it
# trivially observes nr_devices==1 and commits correctly.
if [ "$nr" -lt 2 ]; then
    echo "test-pull-timeout-revert: needs nr>=2 (have $nr), skipping"
    sudo rm -rf $dir
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

# Device whose successfully pulled config should not be thrown away
VICTIM=${IMG}1
VICTIMIP=$(echo $CONTAINERS | awk '{print $1}')
# Device that is made unresponsive so that it times out last
LAGGARD=${IMG}2
LAGGARDIP=$(echo $CONTAINERS | awk '{print $2}')
# Interface added directly on $VICTIM, never via the controller
OOB=oob-iface
# Keep the timeout short so the test does not wait the 60s default
DEVTIMEOUT=10

# Make $LAGGARD stop answering NETCONF without dropping the TCP connection, so
# the controller sits in DEVICE-SYNC until device-timeout fires.  Stopping the
# device-side netconf process is used rather than pausing the container because
# the tests run inside controller-test, which has no docker socket.
function freeze_laggard()
{
    ssh $LAGGARDIP ${SSHID} -l ${USER} -o StrictHostKeyChecking=no \
        -o PasswordAuthentication=no pkill -STOP clixon_netconf
}

function thaw_laggard()
{
    ssh $LAGGARDIP ${SSHID} -l ${USER} -o StrictHostKeyChecking=no \
        -o PasswordAuthentication=no pkill -CONT clixon_netconf || true
}

# Never leave the device frozen, whatever happens below
trap thaw_laggard EXIT

# Extract <tid> from an rpc-reply
function tidof()
{
    echo "$1" | sed -n 's/.*<tid[^>]*>\([0-9][0-9]*\)<\/tid>.*/\1/p'
}

# Poll until transaction $1 reaches state DONE; echo its result (SUCCESS/FAILED).
# Read as structured state rather than the "show transactions" table, whose
# Description column contains spaces and so defeats positional parsing.
function transaction_result()
{
    tid=$1
    jmax=$((DEVTIMEOUT + 20))
    for j in $(seq 1 $jmax); do
        ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <get cl:content="all" xmlns:cl="http://clicon.org/lib">
    <nc:filter nc:type="xpath" nc:select="co:transactions/co:transaction[co:tid='$tid']" xmlns:co="http://clicon.org/controller"/>
  </get>
</rpc>]]>]]>
EOF
           )
        state=$(echo "$ret" | sed -n 's/.*<state>\([A-Z]*\)<\/state>.*/\1/p')
        if [ "$state" = "DONE" ]; then
            # NOTE: result is a leaf at BOTH transaction level and per-device level
            # (devices/device/result); take the first match, which is the
            # transaction-level one (it precedes the devices container in the
            # transaction-common grouping), not a greedy last-match.
            echo "$ret" | grep -o '<result>[A-Z]*</result>' | head -1 | sed 's/<result>\(.*\)<\/result>/\1/'
            return 0
        fi
        sleep 1
    done
    echo "PENDING"
}

# Does the named controller-side copy of $VICTIM contain $OOB?  Echoes a count.
function victim_has()
{
    ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <get-device-config xmlns="http://clicon.org/controller">
    <device>$VICTIM</device>
    <config-type>$1</config-type>
  </get-device-config>
</rpc>]]>]]>
EOF
       )
    echo "$ret" | grep -c "$OOB" || true
}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller: devices configured, opened and pulled
. ./reset-controller.sh

new "set devices device-timeout $DEVTIMEOUT"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device-timeout $DEVTIMEOUT)" 0 "^$"

new "commit local device-timeout"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "Verify both devices open before the pull"
sleep_open "$CFD" ""

# ----------------------------------------------------------------------------
# Part 1: a tail-device timeout throws away the pull of every other device
# ----------------------------------------------------------------------------

new "Add interface $OOB directly on $VICTIM, out of band"
ret=$(ssh $VICTIMIP ${SSHID} -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
      <interfaces xmlns="http://openconfig.net/yang/interfaces">
        <interface nc:operation="merge">
          <name>$OOB</name>
          <config>
            <name>$OOB</name>
            <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type>
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
    err1 "netconf rpc-error detected" "$ret"
fi

new "Freeze $LAGGARD so it times out at the tail of the pull"
freeze_laggard

new "Fleet-wide pull"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <config-pull xmlns="http://clicon.org/controller"><device>*</device></config-pull>
</rpc>]]>]]>
EOF
   )
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "netconf rpc-error detected" "$ret"
fi
tid=$(tidof "$ret")
if [ -z "$tid" ]; then
    err1 "config-pull tid" "$ret"
fi

new "Pull transaction $tid fails on the paused device"
res=$(transaction_result $tid)
if [ "$res" != "FAILED" ]; then
    err "FAILED" "$res"
fi

new "Thaw $LAGGARD"
thaw_laggard

new "Reconnect devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"
sleep_open "$CFD" ""

# $VICTIM answered the pull, so its SYNCED copy was written before the
# transaction failed.  This half already works.
new "SYNCED copy of $VICTIM has $OOB"
res=$(victim_has SYNCED)
if [ "$res" -eq 0 ]; then
    err "SYNCED containing $OOB" "not found"
fi

# ...but tmpdev was discarded with the failed transaction, so running never
# received it.  BUG: this is the defect - running is silently left behind.
new "BUG 1: running copy of $VICTIM should also have $OOB"
res=$(victim_has RUNNING)
if [ "$res" -eq 0 ]; then
    err1 "running containing $OOB (pull was discarded by the tail-device timeout: commit_after_pull gate, controller_device_state.c:1685)" "not found"
fi

# ----------------------------------------------------------------------------
# Part 2: pushing from that stale copy reverts the device, and says nothing
# ----------------------------------------------------------------------------
# NOTE: this part consumes the stale running left behind by Part 1, so it only
# exercises defect 2 while defect 1 is still present.  Once the commit_after_pull
# gate is fixed, running here is fresh, diff(SYNCED, running) is empty and these
# checks pass vacuously.  A fix for defect 2 therefore needs its own coverage:
# construct "SYNCED fresh, pushed-db stale" by other means - eg an already-open
# private candidate that a later successful pull does not refresh, which is the
# route the incident actually took - and assert the push does not revert.

new "Push from running to devices"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <controller-commit xmlns="http://clicon.org/controller">
    <push>COMMIT</push>
    <source>ds:running</source>
  </controller-commit>
</rpc>]]>]]>
EOF
   )
# With defect 1 fixed, running here is fresh (identical to SYNCED), so there is
# nothing to push and the controller correctly rejects with "No changes to
# push" - this is not a bug, just this test's precondition for defect 2 no
# longer existing.  Skip the rest of Part 2 in that case; a dedicated defect-2
# test needs a different way to construct "SYNCED fresh, source stale" (see
# NOTE above).
if echo "$ret" | grep -q "No changes to push"; then
    echo "test-pull-timeout-revert: Part 2 precondition (stale running) no longer holds now that defect 1 is fixed - skipping BUG 2 checks, see NOTE above"
else
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "netconf rpc-error detected" "$ret"
    fi
    tid=$(tidof "$ret")

    new "Push transaction $tid completes"
    res=$(transaction_result $tid)
    if [ "$res" != "SUCCESS" ]; then
        err "SUCCESS" "$res"
    fi

    # The push payload is diff(SYNCED, running).  With running stale, that diff is
    # a deletion of $OOB - a change nobody asked for, on a device nobody touched,
    # and the drift check raised nothing because it only compares SYNCED against
    # the device.  BUG: $OOB must survive a push that never mentioned it.
    new "BUG 2: $OOB still present on $VICTIM after the push"
    ret=$(ssh $VICTIMIP ${SSHID} -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <get-config><source><running/></source></get-config>
</rpc>]]>]]>
EOF
       )
    match=$(echo "$ret" | grep --null -Eo "$OOB") || true
    if [ -z "$match" ]; then
        err1 "$OOB on $VICTIM (silently reverted by push of a stale copy: push guard reads SYNCED vs TRANSIENT, controller_device_state.c:982, while payload is diff(SYNCED,db), controller_rpc.c:563)" "deleted"
    fi
fi

new "Thaw $LAGGARD and drop the cleanup trap"
thaw_laggard
trap - EXIT

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

sudo rm -rf $dir
endtest
