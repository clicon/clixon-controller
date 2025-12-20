#!/usr/bin/env bash
# Test exclusive lock
# See  https://github.com/clicon/clixon-controller/issues/92 and
# https://github.com/clicon/clixon-controller/issues/204
# Assume two users, one admin (clicon) and one limited (whoami)
# With CLICON_AUTOLOCK, NACM and mini-service
# protected data is devices/device/openconfig1/config/keychains
# The tests are:
# 1) admin makes edits of protected data in openconfig-keychain
# 2a) admin can see protected data with show config
# 2b) limited cannot see protected data with show config
# 3a) admin can see protected data with show compare
# 3b) limited cannot see protected data with show compare
# 4a) admin can see protected data with commit diff
# 4b) limited cannot see protected data with commit diff
# 5a) limited cannot make commit
# 5b) admin can make commit
# 6a) limited cannot edit service data
# 6b) admin can edit service data
# 7a) limited cannot do commit diff
# 7b) admin can do commit diff
# 8a) limited cannot do commit
# 8b) admin can do commit
# 9)  admin makes change
# 10) kill admin session
# 11) Check change removed
# 12) Configuration on devices is updated

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Debug early exit
: ${early:=false}
>&2 echo "early=true for debug "

: ${check:=false}

# Reset devices with initial config
(. ./reset-devices.sh)

dir=/var/tmp/$0
modules=$dir/modules
fyang=$dir/example.yang
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $modules || mkdir -p $modules
test -d $CFD || mkdir -p $CFD
pycode=$modules/example.py

# Use two users
ADMIN=${CLICON_USER} # Clicon user
LIMITED=$(whoami)    # Who runs the script

if [ "$ADMIN" = "${LIMITED}" ]; then
    echo "Test requires whoami != CLICON_USER"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [ "${LIMITED}" = root ]; then   # Cant run limited as root
   id clixon-test-user
   if [ $? = 1 ]; then
       # Debian only
       sudo useradd -M -U clixon-test-user
       sudo usermod -a -G clicon clixon-test-user;
   fi
   LIMITED=clixon-test-user
fi

# Common NACM scripts
. ./nacm.sh

cat <<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:fcgi</CLICON_FEATURE>
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_YANG_DIR>${DATADIR}/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/common</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <CLICON_YANG_SCHEMA_MOUNT_SHARE>true</CLICON_YANG_SCHEMA_MOUNT_SHARE>
  <CLICON_YANG_USE_ORIGINAL>true</CLICON_YANG_USE_ORIGINAL>
  <CLICON_BACKEND_DIR>${LIBDIR}/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_USER>${ADMIN}</CLICON_BACKEND_USER>
  <CLICON_BACKEND_PRIVILEGES>none</CLICON_BACKEND_PRIVILEGES>
  <CLICON_BACKEND_PIDFILE>${LOCALSTATEDIR}/run/controller/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_RESTCONF_INSTALLDIR>${SBINDIR}</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_CLI_DIR>${LIBDIR}/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>${LIBDIR}/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_AUTOCLI_CACHE_DIR>${LIBDIR}/controller/autocli</CLICON_AUTOCLI_CACHE_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_SOCK>${LOCALSTATEDIR}/run/controller/controller.sock</CLICON_SOCK>
  <CLICON_SOCK_GROUP>${CLICON_GROUP}</CLICON_SOCK_GROUP>
  <CLICON_SOCK_PRIO>true</CLICON_SOCK_PRIO>
  <CLICON_AUTOLOCK>true</CLICON_AUTOLOCK>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_MULTI>true</CLICON_XMLDB_MULTI>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_CREDENTIALS>except</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <!-- Cant split config file since pyapi cant read that -->
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_server.py -f $CFG</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">$modules</CONTROLLER_PYAPI_MODULE_PATH>
  <CONTROLLER_PYAPI_MODULE_FILTER xmlns="http://clicon.org/controller-config"></CONTROLLER_PYAPI_MODULE_FILTER>
  <CONTROLLER_PYAPI_PIDFILE xmlns="http://clicon.org/controller-config">/tmp/clixon_pyapi.pid</CONTROLLER_PYAPI_PIDFILE>  <!-- clicon goup cannot write in ${LOCALSTATEDIR} -->
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

cat <<EOF > $fyang
module example {
    namespace "http://clicon.org/example";
    prefix ex;

    import clixon-controller {
        prefix ctrl;
    }
    revision 2025-05-17{
        description "TNSR service example";
    }
    augment "/ctrl:services" {
        list example {
            key service-name;
            leaf service-name {
                type string;
            }
            description "TNSR example service";
            uses ctrl:created-by-service;
        }
    }
}
EOF

cat <<EOF > $pycode
SERVICE = "example"

def setup(root, log, **kwargs):
    try:
        _ = root.services
    except Exception:
        return
    for instance in root.services.example:
        if instance.service_name != kwargs["instance"]:
            continue
        description = instance.service_name.get_data()
        for device in root.devices.device:
            device.config.system.config.create("login-banner", data=description)
EOF


RULES=$(cat <<EOF
   <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
     <enable-nacm>true</enable-nacm>
     <!--read-default>permit</read-default-->
     <read-default>deny</read-default>
     <write-default>deny</write-default>
     <exec-default>deny</exec-default>

     <groups>
       <group>
         <name>admin</name>
         <user-name>root</user-name>
         <user-name>${ADMIN}</user-name>
       </group>
       <group>
         <name>limited</name>
         <user-name>$LIMITED</user-name>
       </group>
     </groups>

     $NADMIN

     <rule-list>
       <name>limited-acl</name>
       <group>limited</group>
       <rule>
         <name>permit-ops</name>
         <comment>Allow all operations</comment>
         <rpc-name>*</rpc-name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
       </rule>
       <rule>
         <name>permit-system-path</name>
         <comment>Allow openconfig system reads</comment>
         <module-name>*</module-name>
         <access-operations>read</access-operations>
         <path xmlns:ctrl="urn:example:controller" xmlns:oc-sys="http://openconfig.net/yang/system">/ctrl:devices/ctrl:device/ctrl:config/oc-sys:system</path>
         <action>permit</action>
       </rule>
       <rule>
         <name>permit-read-monitoring</name>
         <comment>Allow monitoring reads</comment>
         <module-name>ietf-netconf-monitoring</module-name>
         <path xmlns:ncm="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">/ncm:netconf-state</path>
         <access-operations>read</access-operations>
         <action>permit</action>
       </rule>

     </rule-list>
   </nacm>
EOF
)

# Set initial NACM rules in startup enabling admin and a single param config
sudo rm -rf $dir/startup.d/*
cat <<EOF > $dir/startup_db
<config>
  ${RULES}
  <devices xmlns="http://clicon.org/controller">
EOF

ii=1
for ip in $CONTAINERS; do
    NAME="$IMG$ii"
    cat <<EOF >> $dir/startup_db
    <device>
      <name>$NAME</name>
      <enabled>true</enabled>
      <user>$USER</user>
      <conn-type>NETCONF_SSH</conn-type>
      <addr>$ip</addr>
    </device>
EOF
    ii=$((ii+1))
done

cat <<EOF >> $dir/startup_db
  </devices>
</config>
EOF

# Sleep and verify devices are open
function sleep_open()
{
    pre=$1
    for j in $(seq 1 10); do
        new "Verify devices are open"
        ret=$($pre $clixon_cli -1 -f $CFG show connections)
        match1=$(echo "$ret" | grep --null -Eo "openconfig1.*OPEN") || true
        match2=$(echo "$ret" | grep --null -Eo "openconfig2.*OPEN") || true
        if [ -n "$match1" -a -n "$match2" ]; then
            break;
        fi
        echo "retry after sleep"
        sleep 1
    done
    if [ $j -eq 10 ]; then
        err "device openconfig OPEN" "Timeout"
    fi
}

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

new "Wait backend"
wait_backend

new "connection open"
expectpart "$(sudo -u ${CLICON_USER} $clixon_cli -1 -f $CFG connection open)" 0 "^$"

new "Sleep and verify devices are open"
sleep_open "sudo -u ${CLICON_USER}"

if ${early}; then
    exit # for starting controller with devices and debug
fi

new "Spawn expect script"

# -d to debug matching info
sudo expect - "$clixon_cli" "$CFG" "$ADMIN" "$LIMITED" <<'EOF'
# Use of expect to start parallel cli:s
#log_user 1
set stty_init "rows 10000 cols 128"

# valgrind requires timeout > 2
set timeout 5
set clixon_cli [lindex $argv 0]
set CFG [lindex $argv 1]
set ADMIN [lindex $argv 2]
set LIMITED [lindex $argv 3]

set hostname [exec hostname]

puts "Test Expect lock-exclusive"

spawn {*}sudo -u $ADMIN clixon_cli -f $CFG -m configure -- -e
set cli1 $spawn_id
set prompt1 "$ADMIN@$hostname\\\[/\\\]# "

spawn sudo -u $LIMITED clixon_cli -f $CFG -m configure -- -e
set cli2 $spawn_id
set prompt2 "$LIMITED@$hostname\\\[/\\\]# "

puts "Test: 1a) admin makes edits of protected data in openconfig-keychains (name)"
set cmd "set devices device openconfig1 config keychains keychain e0 config name e0"
sleep 1
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "^\r$prompt1$cmd\r\n\r\r$prompt1\$" { puts "Test 1a: empty OK" }
    timeout { puts "Timeout 1a"; exit 2 }
    eof { exit 3 }
}

set cmd "show devices device openconfig1 config"
puts "Test: 2a) admin can see protected data with show config"
send -i $cli1 "$cmd\n"
sleep 1
expect {
    -i $cli1
     -re "openconfig-keychain:keychains \{.+name e0.+\$" { puts "Test 2a: can see data" }
    timeout { puts "Timeout 2a"; exit 2 }
    eof { exit 3 }
}

puts "Test: 2b) limited cannot see protected data with show config"
send -i $cli2 "$cmd\n"
sleep 1
expect {
    -i $cli2
    -re "openconfig-keychain:keychains \{.+name e0;.+\$" { puts "Test 2b: can see data"; exit 1 }
    timeout { puts "Test 2b: timeout cannot see data" }
    eof { exit 3 }
}
expect {
    -i $cli2
    -re "^.*$cmd.*\$" { puts "Test 2bb: prompt"; }
    timeout { puts "Test 2bb: no prompt"; exit 2 }
    eof { exit 3 }
}

set cmd "show compare"
puts "Test: 3a) admin can see protected data with show compare"
send -i $cli1 "$cmd\n"
sleep 1
expect {
    -i $cli1
    -re "\\+.+keychain e0 \{.+$prompt1\$" { puts "Test 3a see data"; }
    timeout { puts "Timeout 3a"; exit 2 }
    eof { exit 3 }
}

puts "Test: 3b) limited cannot see protected data with show compare"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "\\+.+keychain e0 \{.+$prompt2\$" { puts "Test 3b can see data"; puts "<<<$expect_out(buffer)>>>"; exit 2 }
    timeout { puts "Timeout 3b cannot see data" }
    eof { exit 3 }
}

set cmd "commit diff"
puts "Test: 4a) admin can see protected data with commit diff"
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "\\+.+<keychain>.+<name>e0</name>.+$prompt1\$" { puts "Test 4a can see data"; }
    timeout { puts "Timeout 4a"; exit 2 }
    eof { exit 3 }
}

puts "Test: 4b) limited cannot see protected data with commit diff"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 4b cannot commit diff"; }
    -re "\\+.+<keychain>.+<name>e0</name>.+$prompt2\$" { puts "Test 4b can see data"; exit 1 }
    timeout { puts "Timeout 4b: cannot see data"; }
    eof { exit 3 }
}

set cmd "commit"
puts "Test: 5a) limited cannot make commit"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 5a cannot commit"; }
    timeout { puts "Timeout 5a: cannot see data"; }
    eof { exit 3 }
}

puts "Test: 5b) admin can do commit"
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 5b cannot commit"; exit 1 }
    -re "^$cmd\r\n\rOK\r\n\r$prompt1\$" { puts "Test 5b: empty OK" }
    timeout { puts "Timeout 5b"; exit 2}
    eof { exit 3 }
}

set cmd "set service example Mybanner"
puts "Test: 6a) limited cannot edit service data"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "$cmd.+access-denied default deny.+$prompt2" { puts "Test 6a: cannot commit" }
    timeout { puts "Timeout 6a"; exit 2}
    eof { exit 3 }
}

puts "Test: 6b) admin can edit service data"
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "$cmd\r\n\r\r$prompt1\$" { puts "Test 6b: edit" }
    timeout { puts "Timeout 6b"; exit 2 }
    eof { exit 3 }
}

set cmd "commit diff"
puts "Test: 7a) limited cannot do commit diff"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 7a cannot commit diff"; }
    timeout { puts "Timeout 7a: can see data"; exit 2}
    eof { exit 3 }
}

puts "Test: 7b) admin can do commit diff"
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 7b cannot commit diff"; exit 1}
    -re "\\+.+ <login-banner>Mybanner</login-banner>.+$prompt1\$" { puts "Test 7b see data"}
    timeout { puts "Timeout 7b: cannot see data"; exit 2}
    eof { exit 3 }
}

set cmd "commit"
puts "Test: 8a) limited cannot do commit"
send -i $cli2 "$cmd\n"
expect {
    -i $cli2
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 8a cannot commit"; }
    timeout { puts "Timeout 8a: can see data"; exit 2}
    eof { exit 3 }
}

puts "Test: 8b) admin can do commit"
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "Candidate db is locked.+$prompt2\$" { puts "Test 8b cannot commit"; exit 1 }
    -re "^$cmd\r\n\rOK\r\n\r$prompt1\$" { puts "Test 8b: empty OK" }
    timeout { puts "Timeout 8b"; exit 2}
    eof { exit 3 }
}

if { true } {
puts "Test: 9) admin makes change"
set cmd "set devices device openconfig1 config keychains keychain e1 config name e1"
sleep 1
send -i $cli1 "$cmd\n"
expect {
    -i $cli1
    -re "^$cmd\r\n\r\r$prompt1\$" { puts "Test 9: empty OK" }
    timeout { puts "Timeout 9"; exit 2 }
    eof { exit 3 }
}

puts "Test: 10) kill admin session"

puts "Test: 11) Check change removed"
}
EOF
# NOTE not completed checking removed datastore
ret=$?
# echo "ret:$ret"
if [ $ret -ne 0 ]; then
    err1 "Failed: test lock exclusive using expect"
fi

# Show the configuration on the devices after mini-service using SSH
new "12) Configuration on devices is updated"
for container in $CONTAINERS; do
    new "Verify configuration on $container"
    expectpart "$(ssh ${SSHID} -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config login-banner Mybanner"
done

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
