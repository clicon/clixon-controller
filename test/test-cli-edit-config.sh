#!/usr/bin/env bash
# Test cli edits
# ALso restart backend and rerun tests

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

function testrun()
{
    new "Show device diff, should be empty"
    expectpart "$($clixon_cli -1 -f $CFG show devices diff)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG 'show devices openconfig* diff')" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG show devices openconfig1 diff)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG show devices openconfig2 diff)" 0 ""

    new "Configure hostname on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config hostname test1)" 0 ""

    new "Verify show compare on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^-\ *hostname openconfig1;" "^+\ *hostname test1;"

    new "Verify commit diff on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^\-\ *<hostname>openconfig1</hostname>" "^+\ *<hostname>test1</hostname>"

    new "Rollback hostname on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

    new "Configure hostnames on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

    new "Verify show compare on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^\-\ *hostname openconfig1;" "^+\ *hostname test;" "^\-\ *hostname openconfig2;"

    new "Verify commit diff on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^\-\ *<hostname>openconfig1</hostname>" "^+\ *<hostname>test</hostname>" "^\-\ *<hostname>openconfig2</hostname>"

    new "Rollback hostnames on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

    new "Set hostname on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

    new "Commit on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

    for container in $CONTAINERS; do
        new "Verify hostname on $container"
        expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname test"
    done

    for i in $(seq 1 $nr); do
        new "Commit hostname on openconfig1"
        expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig$i config system config hostname openconfig$i)" 0 ""
        expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""
    done

    for container in $CONTAINERS; do
        new "Verify hostname on $container"
        expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname openconfig*"
    done

    # identityrefs in mountpoints, see https://github.com/clicon/clixon-controller/issues/32
    new "set interface"
    expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device o* config interfaces interface test config name test)" 0 ""

    new "set identityref type"
    expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device o* config interfaces interface test config type ianaift:ethernetCsmacd)" 0 ""
    
    new "validate"
    expectpart "$($clixon_cli -1 -m configure -f $CFG validate)" 0 ""

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

new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG connection close)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "openconfig1.*CLOSED" "openconfig2.*CLOSED"

new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 ""

new "Sleep and verify devices are open 1"
sleep_open

new "Reconnect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection reconnect)" 0 ""

new "Sleep and verify devices are open 2"
sleep_open

new "First testrun"
testrun

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

if $BE; then
    new "Restart backend -s running -f $CFG"
    start_backend -s running -f $CFG
fi  

new "wait backend 2"
wait_backend
    
new "Check config after restart"
expectpart "$($clixon_cli -1 -f $CFG show config)" 0 '<system xmlns="http://openconfig.net/yang/system">' "<hostname>openconfig1</hostname>"

new "Connect to devices after restart"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 ""

# Remove the sleep gives:
# Nov 23 17:07:34: Get devices: application operation-failed Failed to find YANG spec of XML node: system with parent: config in namespace: http://openconfig.net/yang/system. Internal error, state callback returned invalid XML <bad-element>system</bad-element>

sleep $sleep

new "Sleep and verify devices are open 3"
sleep_open

new "Testrun after restart"
testrun # XXX This fails on regression occasionally

endtest
