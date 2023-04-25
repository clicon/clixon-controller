set -eux

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

# Set if also push commit, not only push validate
: ${commit:=true}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend -s init  -f $CFG -D $DBG"
    sudo clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller
. ./reset-controller.sh

# Tests
new "CLI: syncronize devices"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG pull)" 0 ""

new "CLI: Configure service"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG -m configure set services test cli_test)" 0 ""

new "CLI: Set invalid value type"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG -l o -m configure set services test cli_test parameter XXX value YYY)" 255 "CLI syntax error: \"set services test cli_test parameter XXX value YYY\": \"YYY\" is invalid input for cli command: value"

new "CLI: Set valid value type"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG -m configure set services test cli_test parameter XXX value 1.2.3.4)" 0 ""

sleep 2

new "CLI: Commit"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG -m configure commit)" 0 ""

new "CLI: Show configuration"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG show configuration cli)" 0 "^services test cli_test" "^services test cli_test parameter XXX" "^services test cli_test parameter XXX value 1.2.3.4"

new "CLI: Push configuration"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG push)" 0 ""

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi
