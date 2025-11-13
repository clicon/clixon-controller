# Controller connect of static yang modules, ie no get-schema
# Boostrapping from empty config
# 1. Non-complete module, check YANG bind failed
# 2. Non-existent module, check No yangs found
# 3. Full module
# SKIP TEXT SINCE UNCLEAR AND DONT WORK WITH ISOLATED DOMAINS

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
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi
# XXX: SKIP TEST SINCE UNCLEAR AND DONT WORK WITH ISOLATED DOMAINS
if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Default container name, postfixed with 1,2,..,<nr>
: ${IMG:=clixon-example}

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
fyang=$dir/clixon-test@2023-03-22.yang
# openconfig devices have noc user
: ${USER:=noc}

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
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

    new "Start new backend -s startup -f $CFG -E $CFD"
    start_backend -s startup -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

ii=0
for ip in $CONTAINERS; do
    ii=$((ii+1))
    NAME="$IMG$ii"
    cmd="set devices device $NAME device-profile myprofile"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"
    
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"
done

new "commit local 1"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "connection open"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"

sleep $sleep

# Not complete YANG
new "Verify controller: all closed"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep CLOSED | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii closed devices" "$res"
fi

new "Verify reason: YANG bind failed"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep "YANG bind failed" | wc -l)
if [ "$res" != "$ii" ]; then
    err1 "$ii bind failed" "$res"
fi

# 2. YANG file not found
cmd="delete devices device-profile myprofile module-set"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-profile myprofile module-set module openconfig-xxx namespace xxx:uri"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

new "commit local 2"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"
    
new "connection open"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"

sleep $sleep

new "Verify controller: all closed"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep CLOSED | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii closed devices" "$res"
fi

new "Verify reason: No yang files found"
res=$(${clixon_cli} -1f $CFG -E $CFD show state xml | grep "<logmsg>Yang \"openconfig-xxx\" not found in the list of CLICON_YANG_DIRs</logmsg>" | wc -l)
if [ "$res" != "$ii" ]; then
   err "$ii" "$res"
fi

# Add proper yang
cmd="delete devices device-profile myprofile module-set"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-profile myprofile module-set module openconfig-system namespace http://openconfig.net/yang/system"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

new "commit local 3"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "connection open"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 "^$"

sleep $sleep

new "Verify controller: all open"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)

if [ "$res" != "$ii" ]; then
    err1 "$ii open devices" "$res"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

unset NAME
unset nr
unset ii

endtest
