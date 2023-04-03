# set -eux

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also sync push, not only change (useful for manually doing push)
: ${push:=true}

# Set if also sync push commit, not only sync push validate
: ${commit:=true}

# Reset devices with initial config
. ./reset-devices.sh

# Check backend is running
wait_backend

# Reset controller
. ./reset-controller.sh

new "CLI: Sync devices"
expectpart "$($clixon_cli -1 -f $CFG sync pull)" 0 "^$"

new "CLI: Configure service"
expectpart "$($clixon_cli -1 -f $CFG -m configure set services test cli_test)" 0 "^$"

new "CLI: Set invalid value type"
expectpart "$($clixon_cli -1 -f $CFG -l o -m configure set services test cli_test table parameter XXX value YYY)" 255 "CLI syntax error: \"set services test cli_test table parameter XXX value YYY\": \"YYY\" is invalid input for cli command: value"

new "CLI: Set valid value type"
expectpart "$($clixon_cli -1 -f $CFG -m configure set services test cli_test table parameter XXX value 1.2.3.4)" 0

new "CLI: Commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "CLI: Show configuration"
expectpart "$($clixon_cli -1 -f $CFG show configuration cli)" 0 "^services test cli_test" "^services test cli_test table parameter XXX" "^services test cli_test table parameter XXX value 1.2.3.4"

sleep 5

new "CLI: Push configuration"
expectpart "$($clixon_cli -1 -f $CFG sync push)" 0 "^$"
