#!/usr/bin/env bash
# Test cli edits
# Also restart backend and rerun tests

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Debug early exit
: ${early:=false}
>&2 echo "early=true for debug "

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
</clixon-config>
EOF

# Specialize autocli.xml for openconfig vs ietf interfaces
cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>exclude ietf interfaces</name>
       <module-name>ietf-interfaces</module-name>
       <operation>disable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

# Reset devices with initial config
(. ./reset-devices.sh)

# Sleep and verify devices are open
function sleep_open()
{
    jmax=10
    for j in $(seq 1 $jmax); do
        new "cli show connections and check open"
        ret=$($clixon_cli -1 -f $CFG -E $CFD show connections)
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
    new "Show devices diff, should be empty"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices diff)" 0 ""

    new "Show devices openconfig* diff, should be empty"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD 'show devices openconfig* diff')" 0 ""

    new "Show devices openconfig1 diff, should be empty"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices openconfig1 diff)" 0 ""

    new "Show devices openconfig2 diff, should be empty"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices openconfig2 diff)" 0 ""

    new "Configure hostname on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config hostname test1)" 0 ""

    new "Verify show compare on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare)" 0 "^-\ *hostname \"openconfig1\";" "^+\ *hostname \"test1\";"

    new "Verify commit diff on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit diff)" 0 "^\-\ *<hostname>openconfig1</hostname>" "^+\ *<hostname>test1</hostname>"

    new "Rollback hostname on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

    new "Configure hostnames on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

    new "Verify show compare on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare)" 0 "^\-\ *hostname \"openconfig1\";" "^+\ *hostname \"test\";" "^\-\ *hostname \"openconfig2\";"

    new "Verify commit diff on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit diff)" 0 "^\-\ *<hostname>openconfig1</hostname>" "^+\ *<hostname>test</hostname>" "^\-\ *<hostname>openconfig2</hostname>"

    new "Rollback hostnames on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

    new "Set hostname to test on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure 'set devices device openconfig* config system config hostname test')" 0 ""

    new "Commit on openconfig*"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit)" 0 ""

    for container in $CONTAINERS; do
        new "Verify hostname on $container"
        expectpart "$(ssh ${SSHID} -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname test"
    done

    for i in $(seq 1 $nr); do
        new "Edit hostname on openconfig$i"
        expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig$i config system config hostname openconfig$i)" 0 ""

        new "Commit hostname on openconfig$i"
        expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit)" 0 ""
    done

    for container in $CONTAINERS; do
        new "Verify hostname on $container"
        expectpart "$(ssh  ${SSHID} -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config hostname openconfig*"
    done

    # identityrefs in mountpoints, see https://github.com/clicon/clixon-controller/issues/32
    new "set interface"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device o* config interfaces interface test config name test)" 0 ""

    new "set identityref type"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device o* config interfaces interface test config type ianaift:ethernetCsmacd)" 0 ""
    
    new "validate"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD validate)" 0 ""

    # Completion test for https://github.com/clicon/clixon-controller/issues/72
    new "Configure interface xintf on openconfig1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config interfaces interface xintf config name xintf)" 0 ""

    new "Completion of interfaces"
    expectpart "$(echo "set devices device openconfig1 config interfaces interface ?" | $clixon_cli -f $CFG -E $CFD -m configure 2> /dev/null)" 0 "<name>" "xintf"

    new "Delete interface"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD  -m configure delete devices device openconfig1 config interfaces interface xintf)" 0 ""

    new "Rollback"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

    # check no default values for show compare
    new "Set new interface kaka"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config interfaces interface kaka)" 0 ""

    new "show compare no defaults"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show compare)" 0 "<name>kaka</name>" --not-- "<loopback-mode>NONE</loopback-mode>"

}

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "wait backend 1"
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

if ${early}; then
    exit # for starting controller with devices and debug
fi

new "Check YANG memory before reconnect"
premem=$($clixon_cli -1f $CFG -E $CFD show mem backend|grep "YANG Total")

new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close)" 0 ""

new "Ensure closed"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "openconfig1.*CLOSED" "openconfig2.*CLOSED"

echo "$clixon_cli -1 -f $CFG -E $CFD connection open"
#exit
new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 ""

new "Sleep and verify devices are open 1"
sleep_open

new "Check YANG memory after reconnect"
postmem=$($clixon_cli -1f $CFG -E $CFD show mem backend|grep "YANG Total")
if [ "$premem" != "$postmem" ]; then
    err "$premem" "$postmem"
fi

new "Reconnect to devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection reconnect)" 0 ""

new "Sleep and verify devices are open 2"
sleep_open

new "First testrun"
testrun

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

if $BE; then
    new "Restart backend -s running -f $CFG -E $CFD"
    start_backend -s running -f $CFG -E $CFD
fi

new "wait backend 2"
wait_backend
    
new "Check config after restart"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -o CLICON_CLI_OUTPUT_FORMAT=text show config)" 0 "hostname openconfig1;"

new "Connect to devices after restart"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 ""

# Remove the sleep gives:
# Nov 23 17:07:34: Get devices: application operation-failed Failed to find YANG spec of XML node: system with parent: config in namespace: http://openconfig.net/yang/system. Internal error, state callback returned invalid XML <bad-element>system</bad-element>

sleep $sleep

new "Sleep and verify devices are open 3"
sleep_open

new "Testrun after restart"
testrun # XXX This fails on regression occasionally

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -f $CFG -z
fi

endtest
