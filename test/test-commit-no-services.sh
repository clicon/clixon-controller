#!/usr/bin/env bash
# Regression test: CLI commit with open devices but no services and no device changes
# Commit goes via actions and, since no device is pushed, a nested local commit of
# actions->candidate->running. The low-level commit transaction data referenced the
# running/candidate cache trees replaced by the nested commit, leading to a crash
# when freed.
# Uses private candidate since the nested commit then deletes the client's private
# candidate datastore (xmldb_post_commit), freeing the whole tree

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
dbdir=$dir/db
test -d $CFD || mkdir -p $CFD
test -d $dbdir/startup.d || mkdir -p $dbdir/startup.d

cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf-private-candidate:private-candidate</CLICON_FEATURE>
  <CLICON_XMLDB_PRIVATE_CANDIDATE>true</CLICON_XMLDB_PRIVATE_CANDIDATE>
  <CLICON_XMLDB_DIR>$dbdir</CLICON_XMLDB_DIR>
</clixon-config>
EOF
cp ../src/autocli.xml $CFD/

ip1=$(echo $CONTAINERS | awk '{print $1}')
ip2=$(echo $CONTAINERS | awk '{print $2}')

# Devices in startup since reset-controller.sh does not support private candidate
cat <<EOF > ${dbdir}/startup.d/0.xml
<config>
   <devices xmlns="http://clicon.org/controller">
      <device>
         <name>${IMG}1</name>
         <enabled>true</enabled>
         <user>$USER</user>
         <conn-type>NETCONF_SSH</conn-type>
         <yang-config>VALIDATE</yang-config>
         <addr>$ip1</addr>
         <config/>
      </device>
      <device>
         <name>${IMG}2</name>
         <enabled>true</enabled>
         <user>$USER</user>
         <conn-type>NETCONF_SSH</conn-type>
         <yang-config>VALIDATE</yang-config>
         <addr>$ip2</addr>
         <config/>
      </device>
   </devices>
</config>
EOF

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD

    new "Start new backend -s startup -f $CFG -E $CFD"
    start_backend -s startup -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

new "Open connections"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"

wait_devices_open_netconf $CFG $CFD 10

new "verify devices OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "${IMG}1.*OPEN"

for i in 1 2; do
    new "commit $i with open devices and no services"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit)" 0 "^$"

    new "show transaction $i"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transaction)" 0 "SUCCESS" "No device configuration changed, no push necessary"

    new "backend alive after commit $i"
    wait_backend
done

new "commit diff with open devices and no services"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit diff)" 0 "^$"

new "backend alive after commit diff"
wait_backend

new "backend responsive"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "${IMG}1.*OPEN"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
