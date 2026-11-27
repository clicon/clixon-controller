#!/usr/bin/env bash
#
# Device groups as first-level objects
# Simple c service is used
# Connect, commit, show commands for device groups
#

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/confdir
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

fyang=$dir/myyang.yang

# Common NACM scripts
. ./nacm.sh

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

cp ../src/autocli.xml $CFD/

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
}
EOF

RULES=$(cat <<EOF
   <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
     <enable-nacm>true</enable-nacm>
     <read-default>permit</read-default>
     <write-default>permit</write-default>
     <exec-default>permit</exec-default>

     $NGROUPS

     $NADMIN

   </nacm>
EOF
)

# Start from startup which by default should start it
# First disable services process
sudo rm -rf $dir/startup.d
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>true</enabled>
    </services>
  </processes>
  $RULES
</config>
EOF

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi
if $BE; then
    new "Start new backend -s startup -f $CFG -E $CFD"
    start_backend -s startup -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

# Create some groups:
# Base flat group:
# mygroup0 --> ${IMG}2
# Hierarchical group:
# mygroup1 --> mygroup0 ---> ${IMG}2
#          --> ${IMG}1
# Duplicate:
# mygroup2 --> mygroup0 --> ${IMG}2
#          --> mygroup1 --> mygroup0 ---> ${IMG}2
#                       --> ${IMG}1
# Loop:
# mygroup3 --> mygroup0
#          --> mygroup3

cmd="set devices device-group mygroup1 device-group mygroup0"
new "Create hierarchical: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-group mygroup2 device-group mygroup0"
new "Create duplicate: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-group mygroup2 device-group mygroup1"
new "Create duplicate 2: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-group mygroup3 device-group mygroup0"
new "Create loop: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

cmd="set devices device-group mygroup3 device-group mygroup3"
new "Create loop: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

# Reset controller
ii=1
for ip in $CONTAINERS; do
    NAME="$IMG$ii"
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    cmd="set devices device $NAME conn-type NETCONF_SSH"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    cmd="set devices device $NAME user $USER"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    if [ $ii = 1 ]; then
        cmd="set devices device-group mygroup1 device-name $NAME"
        new "$cmd"
        expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"
    elif [ $ii = 2 ]; then
        cmd="set devices device-group mygroup0 device-name $NAME"
        new "$cmd"
        expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"
    fi
    ii=$((ii+1))
done
ii=$((ii-1))

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "connection open base group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open group mygroup0)" 0 "^$"

new "Verify controller 1"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)

if [ "$res" != "1" ]; then
    err1 "$ii open devices" "$res"
fi

new "show device group 1 1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show device group mygroup1 state)" 0 "<name>${IMG}2</name>" --not-- "<name>${IMG}1</name>"

new "connection close"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close)" 0 "^$"

# Hierarchy/duplicate/recursion
new "connection open hierarchical group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open group mygroup1)" 0 "^$"

new "Verify controller 2"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)

if [ "$res" != "2" ]; then
    err1 "$ii open devices" "$res"
fi

new "show device group 1 2"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show device group mygroup1 state)" 0 "<name>${IMG}1</name>" "<name>${IMG}2</name>"

new "set something"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device ${IMG}1 config system config login-banner kalle)" 0 "^$"

new "CLI load rpc template ping"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -E $CFD -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <rpc-template nc:operation="replace">
               <name>ping</name>
               <config>
                  <ping xmlns="http://clicon.org/lib"/>
               </config>
            </rpc-template>
         </devices>
      </config>
EOF
)
#echo "ret:$ret"

if [ -n "$ret" ]; then
    err1 "$ret"
fi

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"

new "show device group diff"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show device group mygroup1 diff)" 0 "login-banner kalle;"

new "show device group check fail"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show device group mygroup1 check)" 0 "out-of-sync"

new "pull group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull group mygroup1 2>&1)" 0 "^$"

new "show device group check OK"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show device group mygroup1 check)" 0 "^$"

new "ping to group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD rpc ping group mygroup1)" 0 "<name>${IMG}1</name>" "<name>${IMG}2</name>" "<ok"

new "CLI load template xml"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -E $CFD -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <variables>
                 <variable><name>NAME</name></variable>
                 <variable><name>TYPE</name></variable>
               </variables>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Config of interface ${NAME},${NAME} and ${TYPE} type</description>
                        </config>
                     </interface>
                  </interfaces>
               </config>
            </template>
         </devices>
      </config>
EOF
)

#echo "ret:$ret"

if [ -n "$ret" ]; then
    err1 "$ret"
fi

new "commit template local"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit local 2>&1)" 0 "^$"

new "Apply template CLI 1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure apply template interfaces group mygroup1 variables NAME z TYPE ianaift:v35)" 0 "^$"

new "Verify compare"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare)" 0 "^+\ *interface z {" "^+\ *type ianaift:v35;" "^+\ *description \"Config of interface z,z and ianaift:v35 type\";" --not-- "^\-"

new "connection close"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close)" 0 "^$"

new "connection open duplicate group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open group mygroup2)" 0 "^$"

new "Verify controller 3"
res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)

if [ "$res" != "2" ]; then
    err1 "$ii open devices" "$res"
fi

new "connection close"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close)" 0 "^$"

new "connection open recursive group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open group mygroup3)" 0 "^$"

res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)

if [ "$res" != "1" ]; then
    err1 "$ii open devices" "$res"
fi

# Device-group reference as device name in configure mode: @<device-group>
# See https://github.com/clicon/clixon-controller/issues/218
new "discard candidate"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Expand device-group references as device names"
expectpart "$(echo "set devices device ?" | $clixon_cli -f $CFG -E $CFD -m configure 2> /dev/null)" 0 "@mygroup0" "@mygroup1" "@mygroup3" "${IMG}1" "${IMG}2"

new "Set device config via hierarchical device-group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device @mygroup1 config system config login-banner grp1)" 0 "^$"

new "Verify both devices changed"
res=$(${clixon_cli} -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare | grep -c "^+\ *login-banner grp1;")
if [ "$res" != "2" ]; then
    err1 "2 changed devices" "$res"
fi

new "Show device config via device-group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show devices device @mygroup1 config system config login-banner)" 0 "${IMG}1:" "${IMG}2:" "grp1"

new "Delete device config via base device-group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure delete devices device @mygroup0 config system config login-banner grp1)" 0 "^$"

new "Verify only ${IMG}1 changed"
res=$(${clixon_cli} -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare | grep -c "^+\ *login-banner grp1;")
if [ "$res" != "1" ]; then
    err1 "1 changed device" "$res"
fi

new "discard candidate"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Set device config via recursive device-group"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device @mygroup3 config system config login-banner grp3)" 0 "^$"

new "Verify only one device changed"
res=$(${clixon_cli} -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare | grep -c "^+\ *login-banner grp3;")
if [ "$res" != "1" ]; then
    err1 "1 changed device" "$res"
fi

new "Verify ${IMG}2 changed"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show devices device ${IMG}2 config system config login-banner)" 0 "grp3"

new "discard candidate"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Edit mode via device-group"
expectpart "$(printf "edit devices device @mygroup1\nset config system config login-banner edit1\n" | $clixon_cli -f $CFG -E $CFD -m configure 2>&1)" 0 --not-- "error"

new "Verify edit mode changed both devices"
res=$(${clixon_cli} -1 -f $CFG -E $CFD -m configure -o CLICON_CLI_OUTPUT_FORMAT=text show compare | grep -c "^+\ *login-banner edit1;")
if [ "$res" != "2" ]; then
    err1 "2 changed devices" "$res"
fi

new "discard candidate"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Unknown device-group error"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device @nosuch description x 2>&1)" 255 "No devices in device-group: nosuch"

new "Unknown device-group: no device config syntax, same as unknown device"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device @nosuch config system config login-banner x 2>&1)" 255 "Unknown command"

new "Device-group reference only applies to device names, not device-group names"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device-group @literal description x)" 0 "^$"

new "Verify literal device-group name"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show devices device-group @literal)" 0 "description x"

new "discard candidate"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

sudo rm -rf $dir
endtest
