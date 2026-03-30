#!/usr/bin/env bash
#
# Simple C / non-python service checking shared object create and delete
# Uses util/controller_service.c as C-based server
# Uses NACM
# Also privcand and python test at the end
#
# Check starting of service (startup/disable/init)
#
# see https://github.com/SUNET/snc-services/issues/12
# Assume a testA(1) --> testA(2) and a testB and a non-service 0
# where
# testA(1):  Ax, Ay, ABx, ABy
# testA(2)2: Ay, Az, ABy, ABz
# testB:     ABx, ABy, ABz, Bx
#
# Algoritm: Clear actions
#
# Operations shown below, all others keep:
# +------------------------------------------------+
# |                       0x                       |
# +----------------+---------------+---------------+
# |     A0x        |      A0y      |      A0z      |
# +----------------+---------------+---------------+
# |     Ax         |      Ay       |      Az       |
# |    (delete)    |               |     (add)     |
# +----------------+---------------+---------------+
# |     ABx        |      ABy      |      ABz      |
# +----------------+---------------+---------------+
# |                       Bx                       |
# +------------------------------------------------+
#
# Also, after testA(1) do a pull and a restart to ensure creator attributes are intact
#
# For debug start service daemon externally: ${BINDIR}/clixon_controller_service -f $CFG -E $CFD and
# disable CONTROLLER_ACTION_COMMAND
# May fail if python service runs
#

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Debug early exit
: ${early:=false}
>&2 echo "early=true for debug "

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/confdir
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

fyang=$dir/myyang.yang

let w=1

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
         key service-name;
         leaf service-name {
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
#    echo "ret:$ret"
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

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
fi

new "Wait backend $w"
wait_backend
let w++

check_services running

new "Start service process, expect fail (already started)"
expectpart "$(clixon_controller_service -f $CFG -E $CFD -1 -l o)" 255 "services-commit client already registered"

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

DEV0="<config>
         <interfaces xmlns=\"http://openconfig.net/yang/interfaces\" xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">
            <interface>
               <name>0x</name>
               <config>
                  <name>0x</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0x</name>
               <config>
                  <name>A0x</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0y</name>
               <config>
                  <name>A0y</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0z</name>
               <config>
                  <name>A0z</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
         </interfaces>
      </config>"

new "edit testA(1)"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <service-name>foo</service-name>
	     <params>A0x</params>
	     <params>A0y</params>
	     <params>Ax</params>
	     <params>Ay</params>
	     <params>ABx</params>
	     <params>ABy</params>
	 </testA>
	  <testB xmlns="urn:example:test" nc:operation="replace">
	     <b_name>foo</b_name>
	     <params>A0x</params>
	     <params>A0y</params>
	     <params>ABx</params>
	     <params>ABy</params>
	     <params>ABz</params>
	     <params>Bx</params>
	 </testB>
      </services>
      <devices xmlns="http://clicon.org/controller">
         <device>
            <name>${IMG}1</name>
            $DEV0
         </device>
         <device>
            <name>${IMG}2</name>
            $DEV0
         </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
      )

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

#edit # edit
sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit push 2>&1)" 0 OK --not-- Error

#edit # push

# see https://github.com/clicon/clixon-controller/issues/70
new "commit diff 1"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit diff 2>&1)" 0 OK --not-- "<interface"

if ${early}; then
    exit # for starting controller with devices and debug
fi

# see https://github.com/clicon/clixon-controller/issues/78
new "local change w default"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD set devices device openconfig1 config system dns servers server 1.1.1.1 config address 1.1.1.1)" 0 ""

new "show compare text"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD show compare 2>&1)" 0 "^+\ *address 1.1.1.1;" --not-- "^+\ *port 53;"

new "show compare xml"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD show compare xml 2>&1)" 0 "^+\ *<address>1.1.1.1</address>" --not-- "^+\ *<port>53</port>"

new "commit diff 2"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit diff 2>&1)" 0 "^+\ *<address>1.1.1.1</address>" --not-- "^+\ *<port>53</port>"

new "commit"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 ""

new "commit diff 3"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit diff 2>&1)" 0 --not-- "^+\ *<address>1.1.1.1</address>"

# This works for nr>=2
CREATORSA="<path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Ax\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Ay\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Ax\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Ay\"\]</path>"

CREATORSB="<path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABz\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Bx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABz\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Bx\"\]</path>"

new "Check creator attributes testA"
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <service-name>foo</service-name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

new "Check creator attributes testB"
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testB xmlns="urn:example:test">
          <b_name>foo</b_name>
        </testB>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"

match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSB") || true
if [ -z "$match" ]; then
    err "$CREATORSB" "$ret"
fi

# Pull and ensure attributes remain
new "Pull"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD pull)" 0 ""
#exit # pull

new "Check creator attributes after pull"
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <service-name>foo</service-name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

# Here, if FINAL_COMMIT, then creator attributes are in candidate but not in running
# It is logial in a way since attributes are removed in actions, then added by service
# then copied to candidate.
# When pulled configs arrive, they are commited into tmpdev and committed into running
# Have to draw,...

# exit

# Restart backend and ensure attributes remain
if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -E $CFD -z
fi

# Note start from previous running
if $BE; then
    new "Start new backend -s running -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s running -f $CFG -E $CFD -D $DBG
fi

new "Wait backend $w"
let w++
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD connect open async)" 0 ""

new "Verify open devices"
sleep $sleep

imax=5
for i in $(seq 1 $imax); do
    res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)
    if [ "$res" = "$nr" ]; then
        break;
    fi
    echo "retry $i after sleep"
    sleep $sleep
done
if [ $i -eq $imax ]; then
    err1 "$nr open devices" "$res"
fi

new "Check creator attributes after restart"
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <service-name>foo</service-name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

new "edit testA(2)"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <service-name>foo</service-name>
	     <params>A0y</params>
	     <params>A0z</params>
	     <params>Ay</params>
	     <params>Az</params>
	     <params>ABy</params>
	     <params>ABz</params>
	 </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "commit diff 4"
# Ax removed, Az added
ret=$(${clixon_cli} -m configure -1f $CFG -E $CFD commit diff 2> /dev/null)
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo '\+ <name>Az</name>') || true
if [ -z "$match" ]; then
    err "commit diff + Az entry" "$ret"
fi
match=$(echo $ret | grep --null -Eo '\- <name>Ax</name') || true
if [ -z "$match" ]; then
    err "diff - Ax entry" "$ret"
fi

# Delete testA completely
new "delete testA(3)"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
          <testA xmlns="urn:example:test" nc:operation="delete">
            <service-name>foo</service-name>
          </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>$NAME</name>
	  <config>
	    <table xmlns="urn:example:clixon"/>
	  </config>
	</device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Ax</name></parameter>") || true

if [ -n "$match" ]; then
    err "Ax is removed in $NAME" "$ret"
fi

# Delete testB completely
new "delete testB(4)"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
          <testB xmlns="urn:example:test" nc:operation="delete">
            <b_name>foo</b_name>
          </testB>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>$NAME</name>
	  <config>
	    <table xmlns="urn:example:clixon"/>
	  </config>
	</device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Bx</name></parameter>") || true
if [ -n "$match" ]; then
    err "Bx is removed in $NAME" "$ret"
fi

# Edit service, pull, check service still in candidate,
# see https://github.com/clicon/clixon-controller/issues/82
new "edit testC"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <service-name>fie</service-name>
	     <params>ZZ</params>
	 </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "show compare service"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show compare xml)" 0 "^\ *<services xmlns=\"http://clicon.org/controller\">" "^+\ *<testA xmlns=\"urn:example:test\">" "^+\ *<service-name>fie</service-name>" "^+\ *<params>ZZ</params>"

new "discard"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD -m configure discard)" 0 ""

new "Pull replace"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD pull)" 0 ""

new "Rollback hostnames on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

# Negative errors
new "Create empty testA"
ret=$(${clixon_cli} -m configure -1f $CFG -E $CFD set services testA foo 2> /dev/null)

new "commit push expect fail"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit push 2>&1)" 255 too-few-elements

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

# Note start from previous running
# Edit sub-service fields https://github.com/clicon/clixon-controller/issues/89
if $BE; then
    new "Start new backend -s running -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s running -f $CFG -E $CFD -D $DBG
fi

new "Wait backend $w"
let w++
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD connect open async)" 0 ""

new "Verify open devices"
sleep $sleep

imax=5
for i in $(seq 1 $imax); do
    res=$(${clixon_cli} -1f $CFG -E $CFD show connections | grep OPEN | wc -l)
    if [ "$res" = "$nr" ]; then
        break;
    fi
    echo "retry $i after sleep"
    sleep $sleep
done
if [ $i -eq $imax ]; then
    err1 "$nr open devices" "$res"
fi

new "Add Sx"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD set services testA foo params Sx)" 0 "^$"

new "Add Sy"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD set services testA foo params Sy)" 0 "^$"

new "commit base"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 "OK"

new "Check Sy"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD show configuration devices device openconfig1 config interfaces interface Sy)" 0 "interface Sy {"

new "remove Sy"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD delete services testA foo params Sy)" 0 "^$"

new "commit remove"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 "OK"

new "Check not Sy"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD show configuration devices device openconfig1 config interfaces interface Sy)" 0 --not-- "interface Sy"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

# Hardcoded to two devices
ip1=$(echo $CONTAINERS | awk '{print $1}')
ip2=$(echo $CONTAINERS | awk '{print $2}')

sudo rm -rf "$dir/startup.d"
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>true</enabled>
    </services>
  </processes>
  <services xmlns="http://clicon.org/controller">
    <testA xmlns="urn:example:test">
      <service-name>foo</service-name>
      <params>Sx</params>
    </testA>
  </services>
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
  $RULES
</config>
EOF

# Sleep and verify devices are open
function apply_services()
{
    privcand=$1

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
  <CLICON_XMLDB_PRIVATE_CANDIDATE>$privcand</CLICON_XMLDB_PRIVATE_CANDIDATE>
</clixon-config>
EOF

    # apply services
    # Note start from previous running
    if $BE; then
        new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
        sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
    fi

    new "Wait backend $w"
    let w++
    wait_backend

    new "open connections"
    expectpart "$(${clixon_cli} -1f $CFG -E $CFD connect open async)" 0 ""
    sleep $sleep

    new "Spawn expect script"
    # -d to debug matching info
    sudo expect - "$clixon_cli" "$CFG" "$CFD" $(whoami) <<'EOF'
log_user 0
set timeout 5
set clixon_cli [lindex $argv 0]
set CFG [lindex $argv 1]
set CFD [lindex $argv 2]
set USER [lindex $argv 3]

puts "Spawn CLI session"
global session1
spawn {*}sudo -u $USER clixon_cli -f $CFG -E $CFD -m configure -o CLICON_CLI_LINES_DEFAULT=0
set session1 $spawn_id

proc clifn { session command reply } {
    send -i $session "$command\n"
    expect {
        -i $session
        -re "$command.*$reply.*\@.*# " {puts -nonewline "$expect_out(buffer)"}
	    timeout { puts "\n\ntimeout"; exit 2 }
	    eof { puts "\n\neof"; exit 3 }
    }
}

puts "cli service testA foo A0x"
clifn $session1 "set services testA foo params A0x" ""

puts "cli service testB foo A0x"
clifn $session1 "set services testA foo params A0x" ""

puts "commit local"
clifn $session1 "commit local" ""

puts "edit service C"
clifn $session1 "set services testC foo" ""

puts "validate local"
clifn $session1 "validate local" "missing-element Missing mandatory XML extra node"

puts "discard"
clifn $session1 "discard" ""
EOF

    if [ $? -ne 0 ]; then
        err1 "Failed: test"
    fi

    new "Count datastores"
    nr0=$(${clixon_cli} -1f $CFG -E $CFD show mem backend detail | grep candidate | wc -l)

    new "apply single services diff"
    expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD apply services myyang:testA foo diff 2>&1)" 0 "OK"

    new "apply single services 1"
    expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD apply services myyang:testA foo 2>&1)" 0 "OK"

    new "apply single services 2"
    expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD apply services myyang:testA foo 2>&1)" 0 "OK"

    new "apply all services"
    expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD apply services 2>&1)" 0 "OK"

    new "Compare nr datastores"
    nr1=$(${clixon_cli} -1f $CFG -E $CFD show mem backend detail | grep candidate | wc -l)

    if [ $nr0 -ne $nr1 ]; then
        err "$nr0" "$nr1"
    fi

    new "Create python script"
    cat<<EOF > $dir/p.py
from clixon.clixon import Clixon

with Clixon(source="running", read_only=True) as clx:
    diff = clx.apply_service("testA", "foo", diff=True)
EOF

    new "Run python script: apply testA foo diff"
    expectpart "$(python3 $dir/p.py 2> /dev/null)" 0 ""

    new "Compare nr datastores after py"
    nr1=$(${clixon_cli} -1f $CFG -E $CFD show mem backend detail | grep candidate | wc -l)

    if [ $nr0 -ne $nr1 ]; then
        err "$nr0" "$nr1"
    fi
    if $BE; then
        new "Kill old backend"
        stop_backend -f $CFG -E $CFD
    fi
} # apply_services

new "Apply services shared"
apply_services false

new "Apply services privcand"
apply_services true

endtest
