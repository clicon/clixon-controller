#!/usr/bin/env bash
# Netconf private candidate functionality
# Tests controller's own private candidate, ie not privcand of devices
# See NETCONF and RESTCONF Private Candidate Datastores draft-ietf-netconf-privcand-07

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
dbdir=$dir/db
# Debug early exit
: ${early:=false}
>&2 echo "early=true for debug "

test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
test -d $dbdir || mkdir -p $dbdir
test -d $dbdir/startup.d || mkdir -p $dbdir/startup.d

fyang=$dir/myyang.yang

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf-private-candidate:private-candidate</CLICON_FEATURE>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_XMLDB_DIR>$dbdir</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_PRIVATE_CANDIDATE>true</CLICON_XMLDB_PRIVATE_CANDIDATE>
</clixon-config>
EOF

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

ip1=$(echo $CONTAINERS | awk '{print $1}')
ip2=$(echo $CONTAINERS | awk '{print $2}')

cat <<EOF > ${dbdir}/startup.d/0.xml
<config>
   <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>openconfig1</name>
	  <enabled>true</enabled>
	  <user>$USER</user>
	  <conn-type>NETCONF_SSH</conn-type>
	  <yang-config>VALIDATE</yang-config>
	  <addr>$ip1</addr>
	  <config/>
	</device>
	<device>
	  <name>openconfig2</name>
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

cat <<EOF > $fyang
module myyang {
   yang-version 1.1;
   namespace "urn:example:test";
   prefix test;
   import clixon-controller {
      prefix ctrl;
   }
   revision 2023-03-22{
      description "Initial prototype";
   }
   augment "/ctrl:services" {
      list testA {
         description "Test A service";
         key a_name;
         leaf a_name {
            description "Test A instance";
            type string;
         }
         leaf-list params{
            type string;
            min-elements 1; /* For validate fail*/
         }
         uses ctrl:created-by-service;
      }
   }
   augment "/ctrl:services" {
   list testB {
      description "Test B service";
         key b_name;
         leaf b_name {
            description "Test B instance";
            type string;
         }
         leaf-list params{
            type string;
         }
         uses ctrl:created-by-service;
      }
   }
   augment "/ctrl:services" {
      list testC {
         description "To test mandatory";
         key c_name;
         leaf c_name {
            description "Test C instance";
            type string;
         }
         leaf extra {
            type string;
            mandatory true;
         }
         uses ctrl:created-by-service;
      }
   }
}
EOF

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s startup -f $CFG -E $CFD"
    start_backend -s startup -f $CFG -E $CFD
fi

new "wait backend"
wait_backend

new "NETCONF server advertise private candidate capability"
ret=$(${clixon_netconf} -f $CFG -E $CFD<<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello $DEFAULTONLY>
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
      <capability>urn:ietf:params:netconf:base:1.1</capability>
      <capability>urn:ietf:params:netconf:capability:private-candidate:1.0</capability>
   </capabilities>
</hello>
]]>]]>
EOF
)
# echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<capability>urn:ietf:params:netconf:capability:private-candidate:1.0") || true
if [ -z "$match" ]; then
    err "urn:ietf:params:netconf:capability:private-candidate:1.0" "$ret"
fi

new "Open connections"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"

if ${early}; then
    exit # for starting controller with devices and debug
fi

# Start two cli:s
# (1) Edit device config in both in different parts
# Commit one, and then second
# (2) Edit device config in both in same parts
# Commit one, cause conflict in other
# Revert, and commit again
new "Spawn expect script to simulate two CLI sessions"

# -d to debug matching info
sudo expect - "$clixon_cli" "$CFG" "$CFD" $(whoami) <<'EOF'
# Use of expect to start two NETCONF sessions
log_user 0
set timeout 5
set clixon_cli [lindex $argv 0]
set CFG [lindex $argv 1]
set CFD [lindex $argv 2]
set USER [lindex $argv 3]

puts "Spawn First CLI session"
global session1
spawn {*}sudo -u $USER clixon_cli -f $CFG -E $CFD -m configure -o CLICON_CLI_LINES_DEFAULT=0
set session1 $spawn_id

puts "Spawn Second CLI session"
global session2
spawn {*}sudo -u $USER clixon_cli -f $CFG -E $CFD -m configure -o CLICON_CLI_LINES_DEFAULT=0
set session2 $spawn_id

proc clifn { session command reply } {
    send -i $session "$command\n"
    expect {
        -i $session
        -re "$command.*$reply.*\@.*# " {puts -nonewline "$expect_out(buffer)"}
	    timeout { puts "\n\ntimeout"; exit 2 }
	    eof { puts "\n\neof"; exit 3 }
    }
}

puts "1 cli1 configure login-banner 111 on openconfig1"
clifn $session1 "set devices device openconfig1 config system config login-banner 111" ""

puts "2 cli2 configure login-banner 222 on openconfig1"
clifn $session2 "set devices device openconfig1 config system config login-banner 222" ""

puts "3a cli1 show compare"
clifn $session1 "show compare" "111"

puts "3b cli2 show compare"
clifn $session2 "show compare" "222"

puts "4 cli1 commit"
clifn $session1 "commit" "OK"
sleep 1

puts "5 cli2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "111"

puts "6 cli2 update"
clifn $session2 "update" "operation-failed Conflict occured: Cannot add leaf node, another leaf node is added"

puts "7 cli2 commit"
clifn $session2 "commit" "Conflict occured: Cannot add leaf node, another leaf node is added"

puts "8 cli2 discard"
clifn $session2 "discard" ""

puts "9 cli2 update"
clifn $session2 "update" ""

puts "10 cli2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "111"

puts "11 cli2 configure login-banner on openconfig1"
clifn $session2 "set devices device openconfig1 config system config login-banner 222" ""

puts "12 cli2 commit again"
clifn $session2 "commit" "OK"

puts "13 cli2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "222"

puts "14 cli1 edit service"
clifn $session1 "set services testA foo params A0x" ""

puts "15 cli1 commit diff"
clifn $session1 "commit diff" "<name>A0x</name>"

puts "16 cli1 commit"
clifn $session1 "commit" "OK"

puts "17 cli1 show"
clifn $session1 "op show config devices device openconfig1 config interfaces interface A0x config" "<name>A0x</name>"

puts "18 cli2 show"
clifn $session2 "op show config devices device openconfig1 config interfaces interface A0x config" "<name>A0x</name>"

puts "19 cli1 configure top-level description"
clifn $session1 "set devices device openconfig1 description newdesc" ""

puts "20 cli1 show compare"
clifn $session1 "show compare" "<description>newdesc</description>"

puts "21 pull"
clifn $session1 "op pull" ""

puts "22 cli1 show compare"
clifn $session1 "show compare" "<description>newdesc</description>"

EOF

if [ $? -ne 0 ]; then
    err1 "Failed: test private candidate using expect"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
