# Controller connect of static yang modules, ie no get-schame
# Boostrapping from empty config
# Note that the test restarts devices backends with CLICON_NETCONF_MONITORING=false to disable RFC 6025
# 1. Non-complete module, check YANG bind failed
# 2. Non-existent module, check No yangs found
# 3. Full module

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

dockerbin=$(which docker)
if [ -z "$dockerbin" ]; then
    echo "Skip test since inside docker"
    exit 0
fi

# Default container name, postfixed with 1,2,..,<nr>
: ${IMG:=clixon-example}

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir

fi
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $CFD || mkdir -p $CFD
fyang=$dir/clixon-test@2023-03-22.yang
# openconfig devices have noc user
: ${USER:=noc}

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>/usr/local/var/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/controller.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>www-data</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
</clixon-config>
EOF

cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>false</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-test {
    namespace "http://clicon.org/test";
    prefix test;
    import ietf-inet-types { prefix inet; }
    import clixon-controller { prefix ctrl; }
    revision 2023-03-22{
        description "Initial prototype";
    }
    augment "/ctrl:services" {
        list test {
            key service-name;
            leaf service-name {
                type string;
            }
            description "Test service";
            list parameter {
                key name;
                leaf name{
                    type string;
                }
                leaf value{
                    type inet:ipv4-address;
                }
            }
        }
    }
}
EOF

# Reset devices with initial config
NETCONF_MONITORING=false
. ./reset-devices.sh
NETCONF_MONITORING=true

# Preset device-profile with a non-complete yang (openconfig-interfaces)
cat <<EOF > $dir/startup_db
<config>
   <devices xmlns="http://clicon.org/controller">
      <device-profile>
         <name>myprofile</name>
         <user>$USER</user>
         <conn-type>NETCONF_SSH</conn-type>
         <yang-config>BIND</yang-config>
         <module-set>
           <module>
              <name>openconfig-interfaces</name>
              <namespace>http://openconfig.net/yang/interfaces</namespace>
           </module>
         </module-set>
      </device-profile>
   </devices>
</config>
EOF

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -f $CFG -z

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

# Check backend is running
wait_backend

ii=0
for ip in $CONTAINERS; do
    ii=$((ii+1))
    NAME="$IMG$ii"
    cmd="set devices device $NAME device-profile myprofile"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
done

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

new "connection open"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

sleep $sleep

# Not complete YANG
new "Verify controller: all closed"
res=$(${clixon_cli} -1f $CFG show devices | grep CLOSED | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii closed devices" "$res"
fi

new "Verify reason: YANG bind failed"
res=$(${clixon_cli} -1f $CFG show devices | grep "YANG bind failed" | wc -l)
if [ "$res" != "$ii" ]; then
    err1 "$ii bind failed" "$res"
fi

# 2. YANG file not found
cmd="delete devices device-profile myprofile module-set"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

cmd="set devices device-profile myprofile module-set module openconfig-xxx namespace xxx:uri"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"
    
new "connection open"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

sleep $sleep

new "Verify controller: all closed"
res=$(${clixon_cli} -1f $CFG show devices | grep CLOSED | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii closed devices" "$res"
fi

new "Verify reason: No yang files found"
res=$(${clixon_cli} -1f $CFG show devices detail | grep "<logmsg>Yang \"openconfig-xxx\" not found in the list of CLICON_YANG_DIRs</logmsg>" | wc -l)
if [ "$res" != "$ii" ]; then
    err1 "$ii bind failed" "$res"
fi

# Add proper yang
cmd="delete devices device-profile myprofile module-set"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

cmd="set devices device-profile myprofile module-set module openconfig-system namespace http://openconfig.net/yang/system"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

new "connection open"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

sleep $sleep

new "Verify controller: all open"
res=$(${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii open devices" "$res"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

unset NAME
unset nr
unset ii

endtest
