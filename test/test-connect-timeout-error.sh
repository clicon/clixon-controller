#!/usr/bin/env bash
# Regression test for issue #247 proposal 3: a device that hangs during the
# connect state machine (CS_CONNECTING/CS_SCHEMA_LIST/CS_SCHEMA_ONE/CS_DEVICE_SYNC)
# should time out using connect-timeout (see issue #247 proposal 1) and be
# recorded in the transaction as per-device result ERROR (not FAILED or
# silently dropped), since the controller's view of that device is left in an
# unrecoverable/inconsistent state.
#
# A hung (but not failed/refused) connection is simulated with the
# blackhole_start/blackhole_stop helpers (see lib.sh)

: ${BLACKHOLE_PORT:=12345}


# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

: ${IMG:=clixon-example}

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "wait backend"
wait_backend

# Reset controller: configures and opens all devices
. ./reset-controller.sh

# Set after sourcing reset-devices.sh/reset-controller.sh: both scripts internally
# loop over a variable also named NAME, clobbering any value set before sourcing.
DEV="${IMG}1"
ip1=$(echo $CONTAINERS | awk '{print $1}')

new "set connect-timeout 3"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices connect-timeout 3)" 0 "^$"

new "commit local connect-timeout"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "close $DEV"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close $DEV)" 0 "^$"

new "start blackhole listener on $DEV ($ip1:$BLACKHOLE_PORT) to simulate a hung connect (TCP connects, no SSH data ever sent)"
blackhole_start $ip1 $BLACKHOLE_PORT

new "point $DEV port at the blackhole listener"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device $DEV port $BLACKHOLE_PORT)" 0 "^$"

new "commit local blackhole port"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "connection open $DEV (async, since it would otherwise block until connect-timeout)"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open async $DEV)" 0 "^$"

new "wait past connect-timeout"
sleep 6

new "show transactions detail: $DEV marked ERROR after connect-timeout"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions detail)" 0 "<name>$DEV</name>" "<result>ERROR</result>" "Timeout waiting for remote peer"

new "show transactions: brief table Devices column shows 1 ERROR"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "1 ERROR"

new "stop blackhole listener on $DEV"
blackhole_stop $ip1

new "restore $DEV port to default"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD delete devices device $DEV port $BLACKHOLE_PORT)" 0 "^$"

new "commit local restore port"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "reopen $DEV (cleanup, restore for subsequent tests)"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $DEV)" 0 "^$"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

sudo rm -rf $dir
endtest
