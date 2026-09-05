#!/usr/bin/env bash
# Regression test for a per-device reporting bug found while investigating
# issue #247: when a "config push" transaction spans several devices and one
# device fails during the initial candidate <lock>, the whole transaction is correctly marked
# FAILED. Ensure other devices that are further along (already locked,
# edited, or validated their own candidate) get their changes silently
# discarded/unlocked as a consequence
#
# The device-side candidate lock is taken deterministically by a raw
# NETCONF/SSH session opened directly to the device (bypassing the
# controller), the same technique as test-lock-devices.sh.

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

# Reset controller: configures and opens all devices
. ./reset-controller.sh

DEV1="${IMG}1"
DEV2="${IMG}2"
ip1=$(echo $CONTAINERS | awk '{print $1}')

new "verify $DEV1 and $DEV2 OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "$DEV1.*OPEN" "$DEV2.*OPEN"

new "asynchronous lock running $DEV1 (bypassing the controller)"
sleep 60 | cat <(echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?><hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>]]>]]><rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"42\"><lock><target><candidate/></target></lock></rpc>]]>]]>") -| ssh ${SSHID} -l $USER $ip1 -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf &

PIDS=($(jobs -l % | cut -c 6- | awk '{print $1}'))

sleep 1

new "CLI set login-banner on $DEV1 (will fail to lock)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV1 config system config login-banner lockfail)" 0 "^$"

new "CLI set login-banner on $DEV2 (should validate fine, then be aborted)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV2 config system config login-banner lockfail)" 0 "^$"

new "CLI commit push (expect error: $DEV1 fails to lock candidate)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "lock" --not-- "^OK$"

new "show transactions: overall result FAILED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "FAILED"

new "show transactions detail: $DEV1 result FAILED (own lock failure)"
out=$($clixon_cli -1 -f $CFG -E $CFD show transactions detail 2>&1)
devblock=$(echo "$out" | grep -A 5 "<name>$DEV1</name>")
if ! echo "$devblock" | grep -q "FAILED"; then
    err1 "$DEV1 result FAILED in transaction detail" "$out"
fi

new "show transactions detail: $DEV2 result FAILED (aborted due to peer failure, not left as default SUCCESS)"
devblock=$(echo "$out" | grep -A 5 "<name>$DEV2</name>")
if ! echo "$devblock" | grep -q "FAILED"; then
    err1 "$DEV2 result FAILED in transaction detail" "$out"
fi
if ! echo "$devblock" | grep -q "Aborted"; then
    err1 "$DEV2 reason mentioning Aborted (peer failure)" "$out"
fi

new "show transactions: brief table Devices column shows 2 FAILED (not success)"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "2 FAILED"

new "release candidate lock on $DEV1"
kill ${PIDS[0]}
wait ${PIDS[0]} 2>/dev/null

wait_devices_open_netconf $CFG $CFD

new "CLI delete login-banner edits (cleanup) $DEV1"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device $DEV1 config system config login-banner lockfail)" 0 "^$"

new "CLI delete login-banner edits (cleanup) $DEV2"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device $DEV2 config system config login-banner lockfail)" 0 "^$"

new "CLI commit local (cleanup, discard failed candidate remnants)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "sanity: commit push now succeeds with lock released"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

sudo rm -rf $dir
endtest
