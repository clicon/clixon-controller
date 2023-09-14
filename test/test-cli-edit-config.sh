#!/usr/bin/env bash

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi


# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

: ${check:=false}

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend"
    sudo clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG connection close)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "openconfig1.*CLOSED" "openconfig2.*CLOSED"

new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 ""

new "Sleep and verify devices are open"
sleep 10
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "openconfig1.*OPEN" "openconfig2.*OPEN"

new "Reconnect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection reconnect)" 0 ""

new "Verify devices are open"
sleep 10
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "openconfig1.*OPEN" "openconfig2.*OPEN"

new "Show device diff, should be empty"
expectpart "$($clixon_cli -1 -f $CFG show devices diff)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG 'show devices openconfig* diff')" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices openconfig1 diff)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices openconfig2 diff)" 0 ""

new "Configure hostname on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config hostname test1)" 0 ""

new "Verify show compare on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^-               hostname openconfig1;" "^\+               hostname test1;"

new "Verify commit diff on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^-    <hostname>openconfig1</hostname>" "^\+    <hostname>test1</hostname>"

new "Rollback hostname on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

new "Configure hostnames on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

new "Verify show compare on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^-               hostname openconfig1;" "^\+               hostname test;" "^-               hostname openconfig2;"

new "Verify commit diff on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^-    <hostname>openconfig1</hostname>" "^\+    <hostname>test</hostname>" "^-    <hostname>openconfig2</hostname>"

new "Rollback hostnames on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

new "Commit hostname on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

new "Commit on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

for container in $CONTAINERS; do
    new "Verify hostname on $container"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname test"
done

new "Commit hostname on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config hostname openconfig1)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

new "Commit hostname on openconfig2"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig2 config system config hostname openconfig2)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

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

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

echo "test-cli-edit-config"
echo OK
