#!/usr/bin/env bash
# Test that NACM data-node rules apply to all devices, also when the devices run different
# revisions of the same YANG module.
# Two devices, both in the "default" YANG domain, the only difference is the revision of
# openconfig-system: the YANG file of ${IMG}2 is patched with a new (newest) revision and with
# an extra top-level container declared before "system", so that the YANG order of "system"
# differs between the two revisions.
# A NACM rule denying .../oc-sys:system/oc-sys:config/oc-sys:hostname has no device key, so it
# must apply to both devices. Before the fix the rule path was resolved in one of the two
# mounted YANG specs only, and the device using the other revision bypassed NACM.
# Note that the YANG file of ${IMG}2 is restored at the end of the test, also on failure.
# See https://github.com/clicon/clixon-controller/issues/251

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

: ${check:=false}

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
test -d $dir || mkdir -p $dir
CFD=$dir/conf.d
test -d $CFD || mkdir -p $CFD
mounts=$dir/mounts
test -d $mounts || mkdir $mounts

USERNAME=$(whoami)

# openconfig-system on the device, patched on ${IMG}2 below
DEVYANG=/usr/local/share/openconfig/public/release/models/system/openconfig-system.yang
# Revision added to the openconfig-system of ${IMG}2, must be newer than the ones in the file
REVISION2=2030-01-01

# The device must announce another revision, which means patching its YANG file. That requires
# docker access to the device containers, which is not the case if the tests run inside the
# controller-test container. If so, skip
if ! sudo docker exec ${IMG}2 true 2>/dev/null; then
    echo "Skip $0: no docker access to device ${IMG}2"
    endtest
    return 0
fi

# Start from a clean domain, it is populated by the device connections below
rm -rf $mounts/*

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DOMAIN_DIR>$mounts</CLICON_YANG_DOMAIN_DIR>
  <CLICON_NACM_CREDENTIALS>except</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

# Restart the backend of a device container, keeping its running config, and wait for it
function restart_device()
{
    name=$1

    sudo docker exec $name sh -c "/usr/local/sbin/clixon_backend -z" || true
    sudo docker exec -d $name /usr/local/sbin/clixon_backend -s running -l e \
         -o CLICON_NETCONF_MONITORING_GETSCHEMA_CDATA=true
    for j in $(seq 1 10); do
        if sudo docker exec $name sh -c "pgrep clixon_backend >/dev/null"; then
            break
        fi
        sleep 1
    done
    sleep $sleep
}

# Restore the original YANG of ${IMG}2 and restart its backend.
# Called both on success and on failure so that ${IMG}2 is not left patched
function restore_device2()
{
    if [ -f $dir/openconfig-system.yang.orig ]; then
        sudo docker cp $dir/openconfig-system.yang.orig ${IMG}2:$DEVYANG
        rm -f $dir/openconfig-system.yang.orig
        restart_device ${IMG}2
    fi
}
trap restore_device2 EXIT

new "Save original openconfig-system of ${IMG}2"
sudo docker cp ${IMG}2:$DEVYANG $dir/openconfig-system.yang.orig

# 1) Add a new newest revision, 2) add a top-level container before "uses system-top", which
# gives "system" another YANG order than in the original revision
new "Patch openconfig-system of ${IMG}2 to revision $REVISION2"
awk -v rev="$REVISION2" '
    !done1 && /^  revision "/ {
        print "  revision \"" rev "\" {";
        print "    description \"Test revision, see clixon-controller issue 251\";";
        print "  }";
        print "";
        done1=1
    }
    /^  uses system-top;/ {
        print "  container extra {";
        print "    description \"Only in the new revision, shifts the YANG order of system\";";
        print "    leaf value {";
        print "      type string;";
        print "    }";
        print "  }";
        print ""
    }
    { print }
' $dir/openconfig-system.yang.orig > $dir/openconfig-system.yang

sudo docker cp $dir/openconfig-system.yang ${IMG}2:$DEVYANG
restart_device ${IMG}2

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "wait backend"
wait_backend

new "reset controller"
. ./reset-controller.sh

new "Verify devices are open"
sleep_open "$CFD" ""

new "Check that ${IMG}2 announces revision $REVISION2"
if [ ! -f $mounts/default/openconfig-system@${REVISION2}.yang ]; then
    err1 "$mounts/default/openconfig-system@${REVISION2}.yang" "not found, did ${IMG}2 announce the patched YANG?"
fi

new "Check that the devices use two different openconfig-system revisions"
nrev=$(ls $mounts/default/openconfig-system@*.yang | wc -l)
if [ "$nrev" != 2 ]; then
    err1 "2 openconfig-system revisions" "$nrev"
fi

# Sanity check that the two devices really use different YANG revisions
new "Set extra on ${IMG}2, only exists in the new revision"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}2 config extra value xyz)" 0 ""

new "Set extra on ${IMG}1, expect fail"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}1 config extra value xyz 2> /dev/null)" 255 ""

new "Rollback"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Enable NACM"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure delete nacm)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm enable-nacm true)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm read-default permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm write-default permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm exec-default permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm groups group test-group user-name ${USERNAME})" 0 ""

new "Deny hostname on all devices, note no device key in the path"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:hostname)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""

new "Set hostname on ${IMG}1, expect access denied"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}1 config system config hostname test 2>&1)" 255 "access-denied"

new "Set hostname on ${IMG}2 using the other revision, expect access denied"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}2 config system config hostname test 2>&1)" 255 "access-denied"

new "Set domain-name on ${IMG}1, expect ok"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}1 config system config domain-name example.com)" 0 ""

new "Set domain-name on ${IMG}2, expect ok"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}2 config system config domain-name example.com)" 0 ""

new "Rollback"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Permit hostname again"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""

new "Set hostname on ${IMG}1, expect ok"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}1 config system config hostname test1)" 0 ""

new "Set hostname on ${IMG}2, expect ok"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device ${IMG}2 config system config hostname test2)" 0 ""

new "Rollback"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

new "Restore openconfig-system of ${IMG}2"
restore_device2
trap - EXIT

endtest
