#!/usr/bin/env bash
#
# See https://github.com/clicon/clixon-controller/issues/236
# Check timeout of service daemon does not lock candidate
# Force by -d option to service daemon using RESTCONF
#
# Start restconf manually:
# 1. Kill existing restconf
# 2. clixon_restconf -f /usr/local/etc/clixon/controller.xml -E /var/tmp/./test-restconf.sh/confdir -o CLICON_BACKEND_RESTCONF_PROCESS=true
# 3. Start test with RC=false ./test-restconf.sh

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/confdir
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

: ${TIMEOUT:=5}
DATE=$(date -u +"%Y-%m-%d")

fyang=$dir/myyang.yang

# Common NACM scripts
. ./nacm.sh

# RESTCONFIG for bring-your-own restconf
# file means /var/log/clixon_restconf.log
# Example debug: 1048575
RESTCONFIG=$(restconf_config none false 0 file ${TIMEOUT} false)

if [ $? -ne 0 ]; then
    err1 "Error when generating certs"
fi

if ${RC} ; then
    STARTFROMBACKEND=true
else
    STARTFROMBACKEND=false
fi

# Get addr of first container
set -- $CONTAINERS
ADDR1=$1

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_BACKEND_RESTCONF_PROCESS>${STARTFROMBACKEND}</CLICON_BACKEND_RESTCONF_PROCESS>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_STREAM_DISCOVERY_RFC8040>true</CLICON_STREAM_DISCOVERY_RFC8040>
</clixon-config>
EOF

# Note set -d drop transaction = 2
cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD -d 1</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

# Specialize autocli.xml for openconfig vs ietf interfaces (debug)
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
}
EOF

# Send process-control and check status of services daemon
# Args:
# 0: stopped/running   Expected process status
function check_services()
{
    status=$1
    new "Query process-control of action process"
    ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
   <process-control $LIBNS>
      <name>Services process</name>
      <operation>status</operation>
   </process-control>
</rpc>]]>]]>
EOF
      )
    new "Check rpc-error"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err "<reply>" "$ret"
    fi

    new "Check rpc-error status=$status"
    match=$(echo "$ret" | grep --null -Eo "<status $LIBNS>$status</status>") || true
    if [ -z "$match" ]; then
        err "<status>$status</status>" "$ret"
    fi
}

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

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -E $CFD -z
fi

# Then start from startup which by default should start it
# First disable services process
test -d $dir/startup.d || mkdir -p $dir/startup.d
cat <<EOF > $dir/startup.d/0.xml
<config>
  <processes xmlns="http://clicon.org/controller">
     <services>
        <enabled>true</enabled>
     </services>
  </processes>
  ${RESTCONFIG}
  ${RULES}
</config>
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi
if $BE; then
    new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
fi

new "Wait backend"
wait_backend

if [ $valgrindtest -eq 3 ]; then # restconf mem test
    sleep 10
fi
new "Wait restconf"
wait_restconf

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

new "restconf PUT service timeout 5s"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services/service-timeout -d '{"clixon-controller:service-timeout":"5"}')" 0 "HTTP/$HVER 201"

new "restconf POST service AA"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"myyang:testA":[{"a_name":"bar","params":["AA"]}]}')" 0 "HTTP/$HVER 201"

new "Apply service"
# quoting frenzy, also clean service (no instance) does not seem to work
DATA="{\"clixon-controller:input\":{\"device\":\"*\",\"push\":\"COMMIT\",\"actions\":\"FORCE\",\"source\":\"ds:candidate\",\"service-instance\":\"testA[a_name='bar']\"}}"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

sleep 1

new "restconf POST service BB"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"myyang:testA":[{"a_name":"foo","params":["BB"]}]}')" 0 "HTTP/$HVER 201"

new "Apply service (this is dropped)"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

sleep 1

new "restconf POST service CC (is locked)"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"myyang:testA":[{"a_name":"fie","params":["BB"]}]}')" 0 "HTTP/$HVER 409" "lock is already held"

new "Apply service (is locked)"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 412" "Candidate db is locked"

new "Wait until timeout"
sleep 10

new "Verify action timeout"
ret=$($clixon_cli -1 -f $CFG -E $CFD show transaction)
match=$(echo "$ret" | grep --null -Eo "<reason>Timeout waiting for service daemon</reason>") || true
if [ -z "$match" ]; then
    err "<Timeout>" "$ret"
fi

new "restconf POST service CC" # Here is still held
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"myyang:testA":[{"a_name":"fie","params":["CC"]}]}')" 0 "HTTP/$HVER 201"

new "Apply service"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

# netconf baseline
check_services running

if $RC; then
    new "Kill restconf daemon"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD processes restconf stop)" 0 ""
    if [ $valgrindtest -eq 3 ]; then
        sleep 1
        checkvalgrind
    fi
fi

# Kill backend
if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -E $CFD -z
fi

endtest
