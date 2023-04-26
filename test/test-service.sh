#!/usr/bin/env bash
#
# see https://github.com/SUNET/snc-services/issues/12
# 
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

set -eux

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# If set to false, override starting of services_action utility
: ${SA:=true}

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
fi

fyang=$dir/myyang.yang

cat <<EOF > $fyang
module myyang {
    yang-version 1.1;
    namespace "urn:example:test";
    prefix test;
    import ietf-inet-types { 
      prefix inet; 
    }
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

# XXX problem in how to add an application model to all clixon applications
sudo cp $fyang /usr/local/share/clixon/controller/

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend -s init  -f $CFG -D $DBG"
    sudo clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller 
. ./reset-controller.sh

if $SA; then
    echo "Kill previous service action"
    pkill service_action || true
    
    echo "Start service action"
    services_action -f $CFG &
fi

echo "edit testA(1)"
ret=$(${PREFIX} ${clixon_netconf} -0 -f $CFG <<EOF
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
           <name>clixon-example1</name>
           <config>
             <table xmlns="urn:example:clixon">
               <parameter>
                 <name>0x</name>
               </parameter>
               <parameter>
                 <name>A0x</name>
               </parameter>
               <parameter>
                 <name>A0y</name>
               </parameter>
               <parameter>
                 <name>A0z</name>
               </parameter>
             </table>
           </config>
         </device>
         <device>
           <name>clixon-example2</name>
           <config>
             <table xmlns="urn:example:clixon">
               <parameter>
                 <name>0x</name>
               </parameter>
               <parameter>
                 <name>A0x</name>
               </parameter>
               <parameter>
                 <name>A0y</name>
               </parameter>
               <parameter>
                 <name>A0z</name>
               </parameter>
             </table>
           </config>
         </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
      )

echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

echo "commit push"
echo "${PREFIX} ${clixon_cli} -m configure -1f $CFG commit push"
${PREFIX} ${clixon_cli} -m configure -1f $CFG commit push

echo "edit testA(2)"
ret=$(${PREFIX} ${clixon_netconf} -0 -f $CFG <<EOF
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
echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi
      
echo "commit diff"
echo "${PREFIX} ${clixon_cli} -m configure -1f $CFG commit diff"
ret=$(${PREFIX} ${clixon_cli} -m configure -1f $CFG commit diff)

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

sudo rm -rf /usr/local/share/clixon/controller/$fyang

if $SA; then
    echo "Kill service action"
    pkill services_action || true
fi

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

unset SA

echo "test-service OK"
