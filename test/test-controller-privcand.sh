#!/usr/bin/env bash
# Netconf private candidate functionality
# See NETCONF and RESTCONF Private Candidate Datastores draft-ietf-netconf-privcand-07

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
dbdir=$dir/db

test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
test -d $dbdir || mkdir -p $dbdir
test -d $dbdir/startup.d || mkdir -p $dbdir/startup.d

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf-private-candidate:private-candidate</CLICON_FEATURE>
  <CLICON_XMLDB_DIR>$dbdir</CLICON_XMLDB_DIR>
</clixon-config>
EOF

# Specialize autocli.xml for openconfig vs ietf interfaces
# XXX needed?
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

ip1=$(echo $CONTAINERS | awk '{print $1}')
ip2=$(echo $CONTAINERS | awk '{print $2}')

cat <<EOF > ${dbdir}/startup.d/0.xml
<config>
   <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>openconfig1</name>
	  <enabled>true</enabled>
	  <conn-type>NETCONF_SSH</conn-type>
	  <user>$USER</user>
	  <addr>$ip1</addr>
	  <yang-config>VALIDATE</yang-config>
	  <config/>
	</device>
	<device>
	  <name>openconfig2</name>
	  <enabled>true</enabled>
	  <conn-type>NETCONF_SSH</conn-type>
	  <user>$USER</user>
	  <addr>$ip2</addr>
	  <yang-config>VALIDATE</yang-config>
	  <config/>
	</device>
    </devices>
</config>
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

# Start two cli:s
# (1) Edit device config in both in different parts
# Commit one, and then second
# (1) Edit device config in both in same parts
# Commit one, cause conflict in other
# Revert, and commit again
new "Spawn expect script to simulate two CLI sessions"

# -d to debug matching info
sudo expect -d - "$clixon_cli" "$CFG" $(whoami) <<'EOF'
# Use of expect to start two NETCONF sessions
log_user 0
set timeout 5
set clixon_cli [lindex $argv 0]
set CFG [lindex $argv 1]
set USER [lindex $argv 2]

puts "Spawn First CLI session"
global session1
spawn {*}sudo -u $USER clixon_cli -f $CFG -m configure
set session1 $spawn_id

puts "Spawn Second CLI session"
global session2
spawn {*}sudo -u $USER clixon_cli -f $CFG -m configure
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

puts "1 configure login-banner on openconfig1"
clifn $session1 "set devices device openconfig1 config system config login-banner 111" ""

puts "2 configure login-banner on openconfig1"
clifn $session2 "set devices device openconfig1 config system config login-banner 222" ""

puts "2 show compare"
clifn $session2 "show compare" "222"

puts "1 commit"
clifn $session1 "commit" ""
sleep 1

puts "2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "111"

puts "2 update"
clifn $session2 "update" "operation-failed Conflict occured: Cannot add leaf node, another leaf node is added"

puts "2 commit"
clifn $session2 "commit" "Conflict occured: Cannot add leaf node, another leaf node is added"

puts "2 discard"
clifn $session2 "discard" ""

puts "2 update"
clifn $session2 "update" ""

puts "2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "111"

puts "2 configure login-banner on openconfig1"
clifn $session2 "set devices device openconfig1 config system config login-banner 222" ""

puts "2 commit again"
clifn $session2 "commit" ""

puts "2 show running"
clifn $session2 "op show config devices device openconfig1 config system config login-banner" "222"


EOF

if [ $? -ne 0 ]; then
    err1 "Failed: test private candidate using expect"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
