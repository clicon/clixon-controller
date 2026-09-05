#!/usr/bin/env bash
# Test for the *controller-local* validate failure path of "commit push":
# rpc_controller_commit() always runs a local candidate_validate() against
# the controller's own candidate (which includes mounted device YANG, RFC
# 8528) *before* creating any transaction or contacting any device. If this
# local validate fails, the RPC returns an error immediately - no
# transaction is created and no device is touched at all (atomic rejection).
#
# This is distinct from a *device-side* validate failure (see
# test-commit-validate-fail-device.sh), where the local candidate is valid
# but the real device's own explicit NETCONF <validate> RPC fails (eg due to
# drift on a field excluded from out-of-sync comparison via ignore-compare).
#
# The local validate failure is triggered deterministically by setting
# config/name on an openconfig interface to a value that does not match the
# list key ("name"), which is a YANG leafref/must style constraint the
# controller's own local candidate_validate() already catches (schema-mount
# validation covers the same YANG as the device).

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

new "verify $DEV1 and $DEV2 OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "$DEV1.*OPEN" "$DEV2.*OPEN"

new "show transactions before: remember last TID"
before=$($clixon_cli -1 -f $CFG -E $CFD show transactions 2>&1)

new "CLI set interface x config/name mismatch on $DEV1 (fails local candidate_validate)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV1 config interfaces interface x config name MISMATCH)" 0 "^$"

new "CLI set login-banner on $DEV2 (valid, but never pushed: request is rejected atomically)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV2 config system config login-banner localvalidatefail)" 0 "^$"

new "CLI commit push (expect error: local candidate_validate rejects $DEV1's config)"
out=$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)
if [ $? -eq 0 ]; then
    err1 "non-zero exit" "exit 0: $out"
fi
if ! echo "$out" | grep -qi "instance-required"; then
    err1 "error message mentioning instance-required" "$out"
fi

new "show transactions: no new transaction was created (atomic rejection before push)"
after=$($clixon_cli -1 -f $CFG -E $CFD show transactions 2>&1)
if [ "$before" != "$after" ]; then
    err1 "show transactions unchanged (no transaction created)" "$after"
fi

new "verify $DEV1 and $DEV2 still OPEN and untouched (nothing was pushed to any device)"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "$DEV1.*OPEN" "$DEV2.*OPEN"

new "CLI restore interface x config/name to match key (cleanup) $DEV1"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV1 config interfaces interface x config name x)" 0 "^$"

new "CLI delete login-banner edits (cleanup) $DEV2"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device $DEV2 config system config login-banner localvalidatefail)" 0 "^$"

new "CLI commit local (cleanup, discard candidate remnants)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "sanity: commit push now succeeds with valid config restored"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

sudo rm -rf $dir
endtest
