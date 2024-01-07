#!/usr/bin/env bash
# Test ordered-by user
# Use openconfig-system dns servers which is ordered by user
# See https://github.com/clicon/clixon/issues/475

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

: ${check:=false}

# Reset devices with initial config
(. ./reset-devices.sh)

# Sleep and verify devices are open
function sleep_open()
{
    jmax=10
    for j in $(seq 1 $jmax); do
        new "cli show devices and check open"
        ret=$($clixon_cli -1 -f $CFG show devices)
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

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "wait backend"
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

new "add 1.1.1.1"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device openconfig1 config system dns servers server 1.1.1.1 config address 1.1.1.1)" 0 ""

new "add 3.3.3.3"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device openconfig1 config system dns servers server 3.3.3.3 config address 3.3.3.3)" 0 ""

new "add 2.2.2.2"
expectpart "$($clixon_cli -1  -m configure -f $CFG set devices device openconfig1 config system dns servers server 2.2.2.2 config address 2.2.2.2)" 0 ""

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit)" 0 ""

new "delete middle 3.3.3.3"
expectpart "$($clixon_cli -1 -m configure -f $CFG delete devices device openconfig1 config system dns servers server 3.3.3.3)" 0 ""

expectpart "$($clixon_cli -1 -m configure -f $CFG show compare xml)" 0 "\-\ *<address>3.3.3.3</address>" --not-- "+" "1.1.1.1" "2.2.2.2"

expectpart "$($clixon_cli -1 -m configure -f $CFG show compare text)" 0 "\-\ *server 3.3.3.3" --not-- "+" "1.1.1.1" "2.2.2.2"

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit)" 0 ""

expectpart "$($clixon_cli -1 -f $CFG show config)" 0 "<address>1.1.1.1</address>" "<address>2.2.2.2</address>" --not-- "<address>3.3.3.3</address>"

endtest
