#!/usr/bin/env bash
#
# Simple non-python service checking shared object create and delete
# Uses util/controller_service.c as C-based server
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
# For debug start service daemon externally: /usr/local/bin/controller_service -f $CFG

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
else
    rm -rf $dir/*
fi
CFG=$dir/controller.xml
fyang=$dir/myyang.yang

# source IMG/USER etc
. ./site.sh

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>/usr/local/etc/controller.xml</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">/usr/local/bin/clixon_controller_service -f $CFG -D 3 -ls</CONTROLLER_ACTION_COMMAND> <!-- Debug: -D 3 -l s -->
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
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
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>www-data</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
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
     <rule>
       <name>include openconfig</name>
       <module-name>openconfig*</module-name>
       <operation>enable</operation>
     </rule>
     <!-- there are many more arista/openconfig top-level modules -->
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
	    key name;
	    leaf name {
		description "Not used";
		type string;
	    }
	    description "Test service A";
	    leaf-list params{
	       type string;
	   } 
	}
    }
    augment "/ctrl:services" {
	list testB {
	    key name;
	    leaf name {
		type string;
	    }
	    description "Test service B";
	    leaf-list params{
	       type string;
	    }
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
    ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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

# Enable services process to check for already running
# if you run a separate debug clixon_controller_service process for debugging, set this to false
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>false</enabled>
    </services>
  </processes>
</config>
EOF

if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -z

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

# Check backend is running
wait_backend

check_services stopped

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi
# Then start from init which by default should start it
# First disable services process
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>true</enabled>
    </services>
  </processes>
</config>
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -z

    echo "Start new backend -s init -f $CFG -D $DBG"
    sudo clixon_backend -s startup -f $CFG -D $DBG
fi

check_services running

new "Start service process, expect fail (already started)"
#echo "/usr/local/bin/clixon_controller_service -f $CFG -1"
expectpart "$(clixon_controller_service -f $CFG -1 -l o)" 255 "services-commit client already registered"

# Reset controller by initiaiting with clixon/openconfig devices and a pull
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
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
	     <name>foo</name>
	     <params>A0x</params>
	     <params>A0y</params>
	     <params>Ax</params>
	     <params>Ay</params>
	     <params>ABx</params>
	     <params>ABy</params>
	 </testA>
	  <testB xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
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
    echo "netconf rpc-error detected"
    exit 1
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

new "edit testA(2)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
	     <name>foo</name>
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
    echo "netconf rpc-error detected"
    exit 1
fi

new "commit diff"
ret=$(${clixon_cli} -m configure -1f $CFG commit diff 2> /dev/null) 
#echo "$ret"
match=$(echo $ret | grep --null -Eo '+ <name>Az</name>') || true
if [ -z "$match" ]; then
    echo "commit diff failed"
    exit 1
fi
match=$(echo $ret | grep --null -Eo '\- <name>Ax</name') || true
if [ -z "$match" ]; then
    echo "commit diff failed"
    exit 1
fi

# Delete testA completely
new "delete testA(3)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
            <name>foo</name>
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
    echo "netconf rpc-error detected"
    exit 1
fi

sleep $sleep
new "commit push"

set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
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
    echo "netconf rpc-error detected on $NAME"
    exit 1
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Ax</name></parameter>") || true
if [ -n "$match" ]; then
    echo "Error:Ax is not removed in $NAME as it should be"
    exit 1
fi


# Delete testB completely
new "delete testB(4)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
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
            <name>foo</name>
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
    echo "netconf rpc-error detected"
    exit 1
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
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
    echo "netconf rpc-error detected on $NAME"
    exit 1
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Bx</name></parameter>") || true
if [ -n "$match" ]; then
    echo "Error:Bx is not removed in $NAME as it should be"
    exit 1
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
