#!/usr/bin/env bash
#
# Controller RESTCONF tests
# 1. GET config
# 2. PUT config
# 3. RPC: Connectivity
# 4. Notification
# 5. Services

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

: ${TIMEOUT:=10}
DATE=$(date -u +"%Y-%m-%d")

fyang=$dir/myyang.yang

# source IMG/USER etc
. ./site.sh

# Common NACM scripts
. ./nacm.sh

# RESTCONFIG for bring-your-own restconf
# file means /var/log/clixon_restconf.log
RESTCONFIG=$(restconf_config none false 1048575 file ${TIMEOUT} false)
#RESTCONFIG=$(restconf_config none false 0 syslog false)

if [ $? -ne 0 ]; then
    err1 "Error when generating certs"
fi

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_BACKEND_RESTCONF_PROCESS>true</CLICON_BACKEND_RESTCONF_PROCESS>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_STREAM_DISCOVERY_RFC8040>true</CLICON_STREAM_DISCOVERY_RFC8040>
</clixon-config>
EOF

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

# Specialize autocli.xml for openconfig vs ietf interfaces
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
      <name>Action process</name>
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
    new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
fi

new "Wait backend"
wait_backend

new "Wait restconf"
wait_restconf

# netconf baseline
check_services running

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

# 1. GET
new "restconf get restconf resource. RFC 8040 3.3 (json)"
expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf)" 0 "HTTP/$HVER 200" '{"ietf-restconf:restconf":{"data":{},"operations":{},"yang-library-version":"2019-01-04"}}'

new "restconf get restconf resource. RFC 8040 3.3 (xml)"
expectpart "$(curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' $RCPROTO://localhost/restconf)" 0 "HTTP/$HVER 200" '<restconf xmlns="urn:ietf:params:xml:ns:yang:ietf-restconf"><data/><operations/><yang-library-version>2019-01-04</yang-library-version></restconf>'

new "restconf GET datastore top"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 200" '"clixon-controller:devices":{"device":\[{"name":"openconfig1"' '"config":{"interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"ianaift:ethernetCsmacd"' '"ietf-netconf-monitoring:netconf-state":{"capabilities":{'

new "restconf GET device config"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" '"clixon-controller:config":{"interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"ianaift:ethernetCsmacd"'

new "restconf GET device config XML"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" '<config xmlns="http://clicon.org/controller"><interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config>'

# 2. PUT
new "restconf PUT top"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/description -d '{"clixon-controller:description":"Test POST"}')" 0 "HTTP/$HVER 204"

if false; then # NYI
new "restconf PUT device"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/interfaces -d '{"openconfig-interfaces:interfaces"}' )" 0 "HTTP/$HVER 204"

new "restconf PUT device XML"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces -d '<interfaces xmlns="http://openconfig.net/yang/interfaces"/>')" 0 "HTTP/$HVER 204"
fi

# 3. RPC/ connectivity
new "restconf GET connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

new "restconf RPC connection close"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"*","operation":"CLOSE"}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

new "restconf GET connection CLOSED"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"CLOSED"}'

# 4. Notification
new "check notification controller-transaction"
expectpart "$(curl $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/ietf-restconf-monitoring:restconf-state)" 0 "HTTP/$HVER 200" '{"name":"controller-transaction","description":"A transaction has been completed.","replay-support":false,"access":\[{"encoding":"xml","location":"https://localhost/streams/controller-transaction"}\]}'

# Start curl in background and save PID
new "restconf RPC connection open"
curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"*","operation":"OPEN"}}' > /dev/null &
PID=$!

new "Notification controller-transaction timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)

# echo "ret:$ret"
expect="data: <notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\"><eventTime>${DATE}T[0-9:.]*Z</eventTime><controller-transaction xmlns=\"http://clicon.org/controller\"><tid>[0-9:.]*</tid><result>SUCCESS</result></controller-transaction></notification>"
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null

new "restconf GET connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

# 5. Service
new "restconf PUT service"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data -d '{"clixon-controller:services":{"myyang:testA":[{"a_name":"bar","params":["AA"]}]}}')" 0 "HTTP/$HVER 201"

new "restconf GET interface not AA"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" --not-- '<interface><name>AA</name><config><name>AA</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config><state>'

new "Apply service"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d '{"clixon-controller:input":{"device":"*","push":"COMMIT","actions":"FORCE","source":"ds:candidate"}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

new "restconf GET interface AA"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" '<interface><name>AA</name><config><name>AA</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config><state>'

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
