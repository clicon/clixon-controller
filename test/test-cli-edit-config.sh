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

# new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG connection close)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "r1.*CLOSED" "r2.*CLOSED"

# new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 ""

# new "Verify devices are open"
sleep 10
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "r1.*OPEN" "r2.*OPEN"

# new "Reconnect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection reconnect)" 0 ""

# new "Verify devices are open"
sleep 10
expectpart "$($clixon_cli -1 -f $CFG show devices)" 0 "r1.*OPEN" "r2.*OPEN"

new "Show device diff, should be empty"
expectpart "$($clixon_cli -1 -f $CFG show devices diff)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG 'show devices r* diff')" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices r1 diff)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG show devices r2 diff)" 0 ""

new "Configure hostname on r1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device r1 config system config hostname test1)" 0 ""

new "Verify show compare on r1"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^-               hostname r1;" "^\+               hostname test1;"

new "Verify commit diff on r1"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^-    <hostname>r1</hostname>" "^\+    <hostname>test1</hostname>"

new "Rollback hostname on r1"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

new "Configure hostnames on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device r* config system config hostname test')" 0 ""

new "Verify show compare on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^-               hostname r1;" "^\+               hostname test;" "^-               hostname r2;"

new "Verify commit diff on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "^-    <hostname>r1</hostname>" "^\+    <hostname>test</hostname>" "^-    <hostname>r2</hostname>"

new "Rollback hostnames on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

new "Commit hostname on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure 'set devices device r* config system config hostname test')" 0 ""

new "Commit on r*"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

for container in $CONTAINERS; do
    new "Verify hostname on $container"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname test"
done

new "Commit hostname on r1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device r1 config system config hostname r1)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

new "Commit hostname on r2"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device r2 config system config hostname r2)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

for container in $CONTAINERS; do
    new "Verify hostname on $container"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname r*"
done
