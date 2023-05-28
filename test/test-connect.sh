# Controller initial push when no devices are still present,
# I.e., boostrapping from empty config
# See also https://github.com/clicon/clixon-controller/issues/5
# The test is essentially a precursor to test-cli-edit-commit-push but with no pre-configuration
# I.e., run reset-controller.sh from cli
# Start w no config
# 1. Open first device
# 2. Open second device

set -u

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    echo "CONTAINERS unset"
    exit -1
fi

# Default container name, postfixed with 1,2,..,<nr>
: ${IMG:=clixon-example}

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
fi
CFG=$dir/controller.xml
fyang=$dir/clixon-test@2023-03-22.yang
: ${user:=root}

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>/usr/local/etc/controller.xml</CLICON_CONFIGFILE>
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
  <CLICON_XMLDB_DIR>/usr/local/var/controller</CLICON_XMLDB_DIR>
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
  <autocli>
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
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
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend -s init  -f $CFG -D $DBG"
    sudo clixon_backend -s init -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller
ii=1
for ip in $CONTAINERS; do
    NAME="$IMG$ii"
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    cmd="set devices device $NAME conn-type NETCONF_SSH"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME user $user"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    new "commit"
    expectpart "$($clixon_cli -1 -m configure -f $CFG commit)" 0 "^$"

#    sleep $sleep

    new "connection open"
    expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"
    
    sleep $sleep
    
    new "Verify controller"
    res=$(${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)

    if [ "$res" != "$ii" ]; then
        echo "Error: $res devices open, expected $ii"
        exit -1;
    fi

    ii=$((ii+1))

done

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

echo "test-connect"
echo OK
