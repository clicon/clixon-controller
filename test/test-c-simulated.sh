#!/usr/bin/env bash
#
# Simple C / non-python service checking shared object create and delete
# Uses util/controller_service.c as C-based server
# Simulated errors:
# - send an error
# - duplicate entries
# - invalid tag

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
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
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

# Start from startup which by default should start it
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

# Simulated error
cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD -e 1</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -E $CFD -z
fi

if $BE; then
    new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
fi

new "Wait backend 1"
wait_backend

check_services running

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

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
	     <a_name>foo</a_name>
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

new "commit 1"
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 "Transaction [0-9]* failed" "in c-service: simulated error"

new "commit diff" # not locked
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit diff 2>&1)" 0 "simulated error"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

sleep 1 # time to make controller-service die

# Simulated duplicate
# The service generates duplicates both for openconfig1 and openconfig2
# This is accepted by the controller CLICON_NETCONF_DUPLICATE_ALLOW=true
# and duplicates removed
cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD -e 2</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

if $BE; then
    new "Start new backend -s running -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s running -f $CFG -E $CFD -D $DBG -o CLICON_NETCONF_DUPLICATE_ALLOW=true
fi

new "Wait backend 2"
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
	     <a_name>foo</a_name>
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

# A0y is duplicated
new "commit 2"
# Issue 161
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 "" --not-- "operation-failed"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

# Issue 191, Simulate tag error
cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -E $CFD -e 3 -E testA[service_name='foo']</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

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

. ./reset-devices.sh

if $BE; then
    new "Start new backend -s startup -f $CFG -E $CFD -D $DBG"
    sudo clixon_backend -s startup -f $CFG -E $CFD -D $DBG
fi

new "Wait backend 3"
wait_backend

check_services running

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG -E $CFD connect open async)" 0 ""

sleep $sleep

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
	     <a_name>foo</a_name>
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

new "commit 3"
# Issue 161
expectpart "$(${clixon_cli} -m configure -1f $CFG -E $CFD commit 2>&1)" 0 "failed in c-service: Invalid tag"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
