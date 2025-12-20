#!/usr/bin/env bash
#
# Controller RESTCONF tests
# 1. GET config
# 2. PUT config
# 3. RPC: Connectivity
# 4. Notification
# 5. Services
# 6. Device RPC
# 7. Device state
#
# To debug,
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

: ${TIMEOUT:=10}
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

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD</CONTROLLER_ACTION_COMMAND>
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
     <clispec-cache>read</clispec-cache>
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

# netconf baseline
check_services running

# PUT across mount-point to closed device see https://github.com/clicon/clixon-controller/issues/230
new "restconf GET across device mtpoint json of closed device, expect error"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces)" 0 "HTTP/$HVER 412" "Mountpoint operation on closed device openconfig1"

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

if ${RC} ; then
    new "Verify restconf in controller config"
    expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-lib:process-control -d '{"clixon-lib:input":{"name":"restconf","operation":"status"}}')" 0 "HTTP/$HVER 200" '{"clixon-lib:output":{"active":true,"description":"Clixon RESTCONF process"'
fi

# 1. GET
new "restconf get restconf resource. RFC 8040 3.3 (json)"
expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf)" 0 "HTTP/$HVER 200" '{"ietf-restconf:restconf":{"data":{},"operations":{},"yang-library-version":"2019-01-04"}}'

new "restconf get restconf resource. RFC 8040 3.3 (xml)"
expectpart "$(curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' $RCPROTO://localhost/restconf)" 0 "HTTP/$HVER 200" '<restconf xmlns="urn:ietf:params:xml:ns:yang:ietf-restconf"><data/><operations/><yang-library-version>2019-01-04</yang-library-version></restconf>'

# Note fake addr 127.17.0.3 but not used
new "restconf POST test device config"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices -d '{"clixon-controller:device":{"name":"test","enabled":"true","conn-type":"NETCONF_SSH","user":"admin","addr":"127.17.0.3"}}')" 0 "HTTP/$HVER 201"

new "restconf GET device config"
expectpart "$(curl $CURLOPTS -X GET -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=test)" 0 "HTTP/$HVER 200" '{"clixon-controller:device":\[{"name":"test","enabled":true,"user":"admin","conn-type":"NETCONF_SSH","addr":"127.17.0.3","conn-state":"CLOSED"}\]}'

new "restconf DELETE device config"
expectpart "$(curl $CURLOPTS -X DELETE -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=test)" 0 "HTTP/$HVER 204"

new "restconf GET empty device config"
expectpart "$(curl $CURLOPTS -X GET -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=test)" 0 "HTTP/$HVER 404"

new "restconf GET datastore top"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 200" '"clixon-controller:devices":{"device":\[{"name":"openconfig1"' '"config":{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"' '"ietf-netconf-monitoring:netconf-state":{"capabilities":{'

new "restconf GET device config json"
#expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" '"clixon-controller:config":{"interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"ianaift:ethernetCsmacd"'

new "restconf GET device config XML"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" '<config xmlns="http://clicon.org/controller"><interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config>'

# Across device mount-point
new "restconf GET across device mtpoint json"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces)" 0 "HTTP/$HVER 200" '{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"'

new "restconf GET across device mtpoint xml"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces)" 0 "HTTP/$HVER 200" '<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config>'

# 2. PUT
new "restconf PUT top"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/description -d '{"clixon-controller:description":"Test POST"}')" 0 "HTTP/$HVER 204"

new "restconf PUT across device mtpoint json"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces/interface=x/config -d '{"openconfig-interfaces:config":{"name":"x","type":"iana-if-type:ethernetCsmacd","description":"My description"}}')" 0 "HTTP/$HVER 204"

new "Verify local with GET"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces/interface=x/config)" 0 "HTTP/$HVER 200" '{"openconfig-interfaces:config":{"name":"x","type":"iana-if-type:ethernetCsmacd"'

new "Commit push to device"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d '{"clixon-controller:input":{"device":"openconfig1","push":"COMMIT","actions":"NONE","source":"ds:running"}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

sleep 1

i=1
for ip in $CONTAINERS; do
    new "Verify with GET on device"
    NAME=$IMG$i
    ret=$(ssh $ip ${SSHID} -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <get-config>
    <source>
      <running/>
    </source>
    <filter type='subtree'>
      <interfaces xmlns="http://openconfig.net/yang/interfaces">
        <interface>
          <name>x</name>
          <config/>
        </interface>
      </interfaces>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
# echo "ret:$ret"
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "netconf rpc-error detected"
    fi
    match=$(echo $ret | grep --null -Eo "<description>My description</description>") || true
    if [ -z "$match" ]; then
        err "No description on device" "$ret"
    fi
    i=$((i+1))
    break
done

new "restconf PUT across device mtpoint XML"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces/interface=x/config -d '<config xmlns="http://openconfig.net/yang/interfaces"><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type><description>My description</description></config>')" 0 "HTTP/$HVER 204"

# 3. RPC/ connectivity
new "restconf GET connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

new "restconf RPC connection close"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"*","operation":"CLOSE"}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}'

new "restconf GET connection CLOSED"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"CLOSED"}'

# 4. Notification
new "Check notification controller-transaction state monitoring"
expectpart "$(curl $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/ietf-restconf-monitoring:restconf-state)" 0 "HTTP/$HVER 200" "{\"name\":\"controller-transaction\",\"description\":\"A transaction has been completed.\",\"replay-support\":false,\"access\":\[{\"encoding\":\"xml\",\"location\":\"${RCPROTO}://localhost/streams/controller-transaction\"}\]}" '{"name":"services-commit","description":"A commit has been made that changes the services declaration"'

if [ "${WITH_RESTCONF}" = "fcgi" ]; then
new "restconf RPC connection open"
curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"*","operation":"OPEN"}}'

sleep 2

else

# Start curl in background and save PID
new "restconf RPC connection open"
sleep 1 && curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"*","operation":"OPEN"}}' > /dev/null &
PID=$!

new "Notification controller-transaction timeout:${TIMEOUT}s 1"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" ${RCPROTO}://localhost/streams/controller-transaction)

#echo "ret:$ret"

expect="data: <notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\"><eventTime>${DATE}T[0-9:.]*Z</eventTime><controller-transaction xmlns=\"http://clicon.org/controller\"><tid>[0-9:.]*</tid><username>[a-zA-Z0-9]*</username><result>SUCCESS</result></controller-transaction></notification>"

match=$(echo "$ret" | grep --null -Eo "$expect") || true

if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null

fi

new "restconf GET connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

# 5. Service
new "restconf POST service AA"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"myyang:testA":[{"a_name":"bar","params":["AA"]}]}')" 0 "HTTP/$HVER 201"

new "restconf GET interface not AA"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" --not-- '<interface><name>AA</name><config><name>AA</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config><state>'

new "Apply service"
# quoting frenzy, also clean service (no instance) does not seem to work
DATA="{\"clixon-controller:input\":{\"device\":\"*\",\"push\":\"COMMIT\",\"actions\":\"FORCE\",\"source\":\"ds:candidate\",\"service-instance\":\"testA[a_name='bar']\"}}"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

sleep 1

new "restconf GET interface AA"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces)" 0 "HTTP/$HVER 200" '<interface><name>AA</name><config><name>AA</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config><state>'

new "Delete service"
# quoting frenzy, also clean service (no instance) does not seem to work
DATA="{\"clixon-controller:input\":{\"device\":\"*\",\"push\":\"COMMIT\",\"actions\":\"DELETE\",\"source\":\"ds:candidate\",\"service-instance\":\"testA[a_name='bar']\"}}"

expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":'

sleep 3

new "Poll transaction result after apply delete"
expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:transactions/transaction=7)" 0 "HTTP/$HVER 200" '{"clixon-controller:transaction":[{"tid":"[0-9.]*","username":"anonymous","result":"SUCCESS"'

new "restconf DELETE service A"
expectpart "$(curl $CURLOPTS -X DELETE -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services/myyang:testA=bar)" 0 "HTTP/$HVER 204"

new "restconf verify that interface don't exist by doing GET interface AA"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 200" --not-- '<interface><name>AA</name><config><name>AA</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config><state>'

# 6. Device RPC
if [ "${WITH_RESTCONF}" = "native" ]; then
new "Send device RPC in background and save PID"
#echo "curl $CURLOPTS -X POST -H \"Content-Type: application/yang-data+json\" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '{\"clixon-controller:input\":{\"device\":\"openconfig*\",\"config\":{\"clixon-lib:stats\":{\"modules\":\"true\"}}}}'"

sleep 1 && expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '{"clixon-controller:input":{"device":"openconfig*","config":{"clixon-lib:stats":{"modules":"true"}}}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}' &
PID=$!

new "Wait for notification timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)

#echo "ret:$ret"

expect="<controller-transaction xmlns=\"http://clicon.org/controller\"><tid>[0-9:.]*</tid><username>[a-zA-Z0-9]*</username><result>SUCCESS</result><devices><devdata xmlns=\"http://clicon.org/controller\"><name>openconfig1</name>"
match=$(echo "$ret" | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null

fi

new "Get rpc transaction result"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:transactions)" 0 "HTTP/$HVER 200" '{"clixon-controller:transactions":{"transaction":\[{"tid":"1","username":"[a-zA-Z0-9]*","result":"SUCCESS"' # XXX devdata

# 6a. Device RPC no args
if [ "${WITH_RESTCONF}" = "native" ]; then
new "Send device rpc no args in background and save PID"
sleep 1 && expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '{"clixon-controller:input":{"device":"openconfig*","config":{"clixon-lib:ping":null}}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}' &
PID=$!

new "Wait for notification timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)

#echo "ret:$ret"

expect="<controller-transaction xmlns=\"http://clicon.org/controller\"><tid>[0-9:.]*</tid><username>[a-zA-Z0-9]*</username><result>SUCCESS</result><devices><devdata xmlns=\"http://clicon.org/controller\"><name>openconfig1</name><data><ok xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/></data></devdata>"
match=$(echo "$ret" | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null
fi

# 6b. Device RPC using XML
if [ "${WITH_RESTCONF}" = "native" ]; then
new "Send device rpc using RPC xml"
sleep 1 && expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '<input xmlns="http://clicon.org/controller"><device>openconfig*</device><config><ping xmlns="http://clicon.org/lib"></ping></config></input>')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}' &
PID=$!

new "Wait for notification timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)

#echo "ret:$ret"
expect="<controller-transaction xmlns=\"http://clicon.org/controller\"><tid>[0-9:.]*</tid><username>[a-zA-Z0-9]*</username><result>SUCCESS</result><devices><devdata xmlns=\"http://clicon.org/controller\"><name>openconfig1</name><data><ok xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/></data></devdata>"
match=$(echo "$ret" | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null
fi

# 7. Get device state
new "Get state using device rpc get XML"
sleep 1 && expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '<input xmlns="urn:example:clixon-controller">
    <device>openconfig*</device>
    <config>
      <get xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
        <filter type="xpath" select="/oc-sys:system/oc-sys:ssh-server" xmlns:oc-sys="http://openconfig.net/yang/system" />
      </get>
    </config>
</input>')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}' &
PID=$!

new "Wait for notification timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)
#echo "ret:$ret"
expect="<ssh-server><state><enable>true</enable><protocol-version>V2</protocol-version></state></ssh-server>"
match=$(echo "$ret" | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null

# note hard-coded transaction-id=10
new "poll transaction result"
expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:transactions/transaction=11)" 0 "HTTP/$HVER 200" '"ssh-server":{"state":{"enable":"true","protocol-version":"V2"}}'

new "Get state using device rpc get JSON"
sleep 1 && expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:device-rpc -d '{"clixon-controller:input":{"device":"openconfig*","config":{"ietf-netconf:get":null}}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}' &
PID=$!

new "Wait for notification timeout:${TIMEOUT}s"
ret=$(curl $CURLOPTS -X GET -H "Accept: text/event-stream" -H "Cache-Control: no-cache" -H "Connection: keep-alive" $RCPROTO://localhost/streams/controller-transaction)
#echo "ret:$ret"
expect="<tpid xmlns=\"http://openconfig.net/yang/vlan\">oc-vlan-types:TPID_0X8100</tpid>"
match=$(echo "$ret" | grep --null -Eo "$expect") || true
if [ -z "$match" ]; then
    err "$expect" "$ret"
fi

kill $PID 2> /dev/null

# PUT/POST dont work over mountpoints
# see https://github.com/clicon/clixon-controller/issues/210
#
new "restconf PUT across device mtpoint XML 1"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces -d '<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type><description>My description</description></config></interface></interfaces>')" 0 "HTTP/$HVER 204"

new "restconf PUT across device mtpoint XML 2"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config -d '<config xmlns="http://clicon.org/controller"><interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type><description>My description</description></config></interface></interfaces></config>')" 0 "HTTP/$HVER 204"

new "restconf PUT across device mtpoint XML 3"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1 -d "<device xmlns=\"http://clicon.org/controller\"><name>openconfig1</name><enabled>true</enabled><description>Test POST</description><user>$USER</user><conn-type>NETCONF_SSH</conn-type><yang-config>VALIDATE</yang-config><addr>$ADDR1</addr><config><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type><description>My description</description></config></interface></interfaces></config></device>")" 0 "HTTP/$HVER 204"

# since meta-config was replaced
new "restconf check connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

# PUT JSON
new "restconf PUT across device mtpoint json 1"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces -d '{"openconfig-interfaces:interfaces":{"interface":{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd","description":"My description"}}}}')" 0 "HTTP/$HVER 204"

new "restconf PUT across device mtpoint json 2"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config -d '{"clixon-controller:config":{"openconfig-interfaces:interfaces":{"interface":{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd","description":"My description"}}}}}')" 0 "HTTP/$HVER 204"

new "restconf PUT across device mtpoint json 3"
expectpart "$(curl $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1 -d "{\"clixon-controller:device\":{\"name\":\"openconfig1\",\"enabled\":\"true\", \"description\":\"Test POST\",\"user\":\"$USER\",\"conn-type\":\"NETCONF_SSH\",\"addr\":\"$ADDR1\",\"config\":{\"openconfig-interfaces:interfaces\":{\"interface\":{\"name\":\"x\",\"config\":{\"name\":\"x\",\"type\":\"iana-if-type:ethernetCsmacd\",\"description\":\"My description\"}}}}}}")" 0 "HTTP/$HVER 204"

# POST JSON
new "restconf DELETE to make room for POST 1"
expectpart "$(curl $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-interfaces:interfaces)" 0 "HTTP/$HVER 204"

new "restconf POST across device mtpoint json 1"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config -d '{"openconfig-interfaces:interfaces":{"interface":{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd","description":"My description"}}}}')" 0 "HTTP/$HVER 201" "location:"

new "restconf DELETE to make room for POST 2"
expectpart "$(curl $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config)" 0 "HTTP/$HVER 204"

new "restconf POST across device mtpoint json 2"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1 -d '{"clixon-controller:config":{"openconfig-interfaces:interfaces":{"interface":{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd","description":"My description"}}}}}')" 0 "HTTP/$HVER 201" "location:"

new "restconf check connection OPEN XXX"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

new "restconf DELETE to make room for POST 3"
expectpart "$(curl $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1)" 0 "HTTP/$HVER 204"

new "restconf POST across device mtpoint json 3"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:devices -d "{\"clixon-controller:device\":{\"name\":\"openconfig1\",\"enabled\":\"true\", \"user\":\"$USER\",\"conn-type\":\"NETCONF_SSH\",\"addr\":\"$ADDR1\",\"config\":{\"openconfig-interfaces:interfaces\":{\"interface\":{\"name\":\"x\",\"config\":{\"name\":\"x\",\"type\":\"iana-if-type:ethernetCsmacd\",\"description\":\"My description\"}}}}}}")" 0 "HTTP/$HVER 201" "location:"

# Must open, delete closes
new "restconf RPC connection open"
expectpart "$(curl $CURLOPTS -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:connection-change -d '{"clixon-controller:input":{"device":"openconfig1","operation":"OPEN"}}')" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json' '{"clixon-controller:output":{"tid":"[0-9:.]*"}}'

sleep 2

new "restconf GET check connection OPEN"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+json" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/conn-state)" 0 "HTTP/$HVER 200" '{"clixon-controller:conn-state":"OPEN"}'

new "restconf GET config"
expectpart "$(curl $CURLOPTS -H "Accept: application/yang-data+xml" -X GET $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config?content=config)" 0 "HTTP/$HVER 200" '<config xmlns="http://clicon.org/controller"><interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type><description>My description</description></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces><system xmlns="http://openconfig.net/yang/system"><config><hostname>openconfig1</hostname></config></system></config>'

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
