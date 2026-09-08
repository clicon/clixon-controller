#!/usr/bin/env bash
# Regression test: a device connection that dies at the OS level (ssh
# sub-process killed, eg device crashed / TCP reset) while conn-state is
# still (stale) OPEN should be detected fast when a push transaction is
# started against it, rather than only failing later via the full
# device-timeout.
#
# This exercises two complementary fixes:
# - devices_diff() in controller_rpc.c does a non-blocking clixon_event_poll_hup()
#   check on the device socket before adding a changed OPEN device to a push
#   transaction, catching a socket the OS already knows is dead (POLLHUP/POLLERR).
# - The backend's own event loop normally also detects the dead ssh sub-process
#   on its own (independent of any push), updating conn-state to CLOSED.
#
# To reliably exercise the race the poll check targets (conn-state still OPEN
# at the moment devices_diff() runs), the backend is briefly SIGSTOPped so it
# cannot process the socket hangup itself before the push transaction starts.
#
# Regardless of which of the two mechanisms actually catches it, the
# externally observable contract under test is: the push must fail promptly
# (well within a fraction of the configured device-timeout), not hang for the
# full device-timeout.

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

: ${IMG:=clixon-example}
: ${DEVTIMEOUT:=60}    # Deliberately large so a fast failure is unambiguous
: ${FASTFAIL_MAX:=10}  # Upper bound (s) for the push to fail, well under DEVTIMEOUT

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

# Disable ietf-interfaces in the autocli so that "interfaces interface"
# unambiguously resolves to openconfig-interfaces (see test-cli-edit-config.sh)
cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <clispec-cache>read</clispec-cache>
     <rule>
       <name>exclude ietf interfaces</name>
       <module-name>ietf-interfaces</module-name>
       <operation>disable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG"
DBG=ctrl
    start_backend -s init -f $CFG -lf/tmp/backend.log
fi

new "wait backend"
wait_backend

# Reset controller: configures and opens all devices
. ./reset-controller.sh

DEV="${IMG}1"
ip1=$(echo $CONTAINERS | awk '{print $1}')

new "Verify $DEV OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $DEV)" 0 "OPEN"

# Ensure a clean baseline: remove any leftover "interface x" from a previous
# test run so the staged edit below is guaranteed to produce a real diff.
new "cleanup: remove any leftover interface x on $DEV"
$clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device $DEV config interfaces interface x > /dev/null 2>&1
$clixon_cli -1 -m configure -f $CFG -E $CFD commit local > /dev/null 2>&1

new "set device-timeout $DEVTIMEOUT"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device-timeout $DEVTIMEOUT)" 0 "^$"

new "commit local device-timeout"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "stage an uncommitted config change on $DEV (interface x name+type)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV config interfaces interface x config name x)" 0 "^$"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV config interfaces interface x config type ianaift:ethernetCsmacd)" 0 "^$"

new "find backend pid"
BPID=$(pgrep -x clixon_backend | tail -1)
if [ -z "$BPID" ]; then
    err1 "clixon_backend pid found" "none"
fi

new "find ssh sub-process pid for $DEV ($ip1)"
SSHPID=$(pgrep -f "ssh .*@$ip1 " | tail -1)
if [ -z "$SSHPID" ]; then
    err1 "ssh sub-process pid found for $ip1" "none"
fi

new "stop backend (SIGSTOP) so it cannot itself process the socket hangup"
sudo kill -STOP $BPID

new "kill ssh sub-process for $DEV, simulating an abrupt device/connection death"
sudo kill -9 $SSHPID

new "issue commit push against dead-but-still-OPEN $DEV in background"
start=$(date +%s)
( $clixon_cli -1 -m configure -f $CFG -E $CFD commit push > $dir/push.out 2>&1 ; echo $? > $dir/push.rc ) &
PUSHPID=$!

# Give the backend a moment to actually be stopped and the push RPC to be
# queued (accepted into the kernel socket backlog) before resuming, so the
# backend sees both the dead socket and the pending push together on resume.
sleep 1

new "resume backend (SIGCONT)"
sudo kill -CONT $BPID

new "wait for commit push to complete"
wait $PUSHPID
end=$(date +%s)
elapsed=$((end - start))

rc=$(cat $dir/push.rc)
out=$(cat $dir/push.out)

new "commit push failed (non-zero exit)"
if [ "$rc" -eq 0 ]; then
    err1 "non-zero exit" "exit 0: $out"
fi

new "commit push failed fast (<= ${FASTFAIL_MAX}s, not the full ${DEVTIMEOUT}s device-timeout)"
if [ $elapsed -gt $FASTFAIL_MAX ]; then
    err1 "elapsed <= ${FASTFAIL_MAX}s" "elapsed ${elapsed}s: $out"
fi

new "show transactions: overall result FAILED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "FAILED"

new "show transactions detail: $DEV marked FAILED"
out=$($clixon_cli -1 -f $CFG -E $CFD show transactions detail 2>&1)
devblock=$(echo "$out" | grep -A 5 "<name>$DEV</name>")
if [ -z "$devblock" ]; then err1 "$DEV in transaction detail" "$out"; fi
if ! echo "$devblock" | grep -q "FAILED"; then
    err1 "$DEV result FAILED in transaction detail" "$out"
fi

new "cleanup: discard uncommitted candidate change"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD discard)" 0 "^$"

new "cleanup: reset device-timeout to default"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device-timeout $DEVTIMEOUT)" 0 "^$"

new "cleanup: commit local reset device-timeout"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "cleanup: reconnect $DEV"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $DEV)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

sudo rm -rf $dir
endtest
