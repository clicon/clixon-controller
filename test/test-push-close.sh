#!/usr/bin/env bash
# Close devices and push
# see https://github.com/clicon/clixon-controller/issues/95
# 1st device: always open
# 2nd device: up & down
# Edit 1st and 2nd device
#      devices edit   result
#      --------------------------
#      1+2     1+2    ok
#      1+2     -      fail empty 
#      1       -      fail empty
#      1       1      ok
#      1       1+2    fail
#      1       2      fail

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

new "devices:1+2  edit: 1+2, ok"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device ${IMG}* config interfaces interface x config description aaa)" 0 "^$"

new "push commit: ok"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "edit:- push commit: OK"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 "^OK$"

# Problem closing device:
# 1. Killing the controller ssh sub-process hangs controller (consider SIGCHLD?)
# 2. Killing the container itself makes starting it again complex in different contexts
#sudo docker kill ${IMG}2 || true
# Therefore we close via administrative down

new "Close ${IMG}2"
expectpart "$($clixon_cli -1 -f $CFG connection close ${IMG}2)" 0 "^$"

new "edit:- push commit: no devices"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "edit 1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device ${IMG}1 config interfaces interface x config description bbb)" 0 "^$"

new "push commit ok"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "edit 1+2"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device ${IMG}* config interfaces interface x config description ccc)" 0 "^$"

new "push commit fail closed"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 255 "Device is closed" "${IMG}2"

new "rollback"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 "^$"

if false; then # disabled off-line editing
new "edit 2"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device ${IMG}2 config interfaces interface x config description ddd)" 0 "^$" 

new "push commit fail closed"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 255 "Device is closed" "${IMG}2"

new "rollback"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 "^$"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
