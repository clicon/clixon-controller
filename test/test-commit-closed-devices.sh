#!/usr/bin/env bash
# Regression test: commit/push with a configured device that is CLOSED and has
# never been synced (no device-<name>-SYNCED datastore)
# No device containers are needed: the device points to a closed local port.

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
NAME="regress-closed-dev"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

new "set device $NAME enabled"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device $NAME enabled true)" 0 "^$"

new "set device $NAME conn-type"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device $NAME conn-type NETCONF_SSH)" 0 "^$"

new "set device $NAME user"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device $NAME user nouser)" 0 "^$"

new "set device $NAME addr"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device $NAME addr 127.0.0.1)" 0 "^$"

new "set device $NAME port (nothing listens)"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device $NAME port 1)" 0 "^$"

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

if $BE; then
    new "Restart backend -s running (device was never synced)"
    stop_backend -f $CFG
    start_backend -s running -f $CFG
fi

new "Wait backend"
wait_backend

new "connection open (fails, device stays CLOSED)"
$clixon_cli -1 -f $CFG connection open > /dev/null 2>&1 || true
sleep 1

new "verify $NAME is CLOSED"
expectpart "$($clixon_cli -1 -f $CFG show connections)" 0 "$NAME.*CLOSED"

new "commit with closed device and no services: device skipped, not failed"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit)" 0 "^$"

new "show transaction"
expectpart "$($clixon_cli -1 -f $CFG show transaction)" 0 "SUCCESS" "No device configuration changed, no push necessary"

new "backend alive after commit"
wait_backend

new "operation push with closed device: no changes to push"
ret=$($clixon_cli -1 -f $CFG push 2>&1) || true
if ! echo "$ret" | grep -q "No changes to push"; then
    err1 "No changes to push" "$ret"
fi

new "backend alive after push"
wait_backend

new "backend responsive"
expectpart "$($clixon_cli -1 -f $CFG show connections)" 0 "$NAME"

new "cleanup: delete device"
expectpart "$($clixon_cli -1 -m configure -f $CFG delete devices device $NAME)" 0 "^$"

new "commit local (cleanup)"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
