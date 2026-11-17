#!/usr/bin/env bash
# Test device sync-rpc: use an rpc-template instead of a full config get to detect out-of-sync
# The rpc-template is clixon-lib commit-info which returns a digest of the device running config.
# - The reply is stored at connect/pull, and after a successful push commit
# - A commit from the controller succeeds as long as the digest is unchanged (also repeatedly)
# - A direct change on the devices changes the digest: commit fails as out-of-sync
# - A pull stores the new digest and commit works again
# See also test-change-both.sh for the default (full config) out-of-sync check

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
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

new "CLI load rpc-template cinfo (commit-info)"
ret=$(${clixon_cli} -1f $CFG -E $CFD -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <rpc-template nc:operation="replace">
               <name>cinfo</name>
               <config>
                  <commit-info xmlns="http://clicon.org/lib"/>
               </config>
            </rpc-template>
         </devices>
      </config>
EOF
)
if [ -n "$ret" ]; then
    err1 "$ret"
fi

new "commit template local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local 2>&1)" 0 "^$"

for i in $(seq 1 $nr); do
    new "set sync-rpc on ${IMG}$i"
    expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device ${IMG}$i sync-rpc cinfo 2>&1)" 0 "^$"
done

new "commit sync-rpc local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local 2>&1)" 0 "^$"

# sync-rpc is read on connect, and the reply is stored in the sync
new "reconnect devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection reconnect)" 0 ""

new "verify devices OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "${IMG}1.*OPEN" "${IMG}2.*OPEN"

new "set login-banner"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device ${IMG}* config system config login-banner b1 2>&1)" 0 "^$"

new "commit push 1: digest unchanged, expect ok"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit 2>&1)" 0 --not-- "failed" "has changed"

new "set login-banner again"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device ${IMG}* config system config login-banner b2 2>&1)" 0 "^$"

new "commit push 2: digest refreshed after previous commit, expect ok"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit 2>&1)" 0 --not-- "failed" "has changed"

# Change device config directly on the devices (not via controller)
. ./change-devices.sh

new "set login-banner a third time"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device ${IMG}* config system config login-banner b3 2>&1)" 0 "^$"

new "commit push 3: devices changed, expect out-of-sync failure"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit 2>&1)" 0 "sync-rpc reply differs"

new "pull replace to resync"
expectpart "$($clixon_cli -1f $CFG -E $CFD pull replace 2>&1)" 0 "^$"

new "set login-banner after pull"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure set devices device ${IMG}* config system config login-banner b4 2>&1)" 0 "^$"

new "commit push 4: after pull, expect ok"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit 2>&1)" 0 --not-- "failed" "has changed"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

sudo rm -rf $dir
endtest
