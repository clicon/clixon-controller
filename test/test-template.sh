#!/usr/bin/env bash
# Load a template and apply it via XML and check compare
# Reset and load a template and apply if via CLI

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dir=/var/tmp/$0
mntdir=$dir/mounts
test -d $mntdir || mkdir -p $mntdir

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

new "Create template w XML"
ret=$(${clixon_netconf} -0 -f $CFG <<'EOF'
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
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Config of interface ${NAME} and ${TYPE} type</description>
                        </config>
                     </interface>
                  </interfaces>
               </config>
            </template>
         </devices>
      </config>
   </edit-config>
</rpc>]]>]]>
EOF
)
new "Check ok"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "$ret"
    exit 1
fi

new "commit template local"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Apply template NETCONF"
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
   <device-template-apply xmlns="http://clicon.org/controller">
      <devname>openconfig*</devname>
      <template>interfaces</template>
      <variables>
         <variable>
            <name>NAME</name>
            <value>z</value>
         </variable>
         <variable>
            <name>TYPE</name>
            <value>ianaift:v35</value>
         </variable>
      </variables>
   </device-template-apply>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"

new "Check no errors of apply"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    echo "$ret"
    exit 1
fi

new "Verify compare"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^+\ *interface z {" "^+\ *type ianaift:v35;" "^+\ *description \"Config of interface z and ianaift:v35 type\";" --not-- "^\-" 

new "rollback"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 "^$"

# Use CLI load and apply template
new "delete template"
expectpart "$($clixon_cli -1 -f $CFG -m configure delete devices template interfaces)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "load template"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Config of interface ${NAME} and ${TYPE} type</description>
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
    exit 1
fi

new "commit template local"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Apply template CLI"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply interfaces openconfig* variables NAME z TYPE ianaift:v35)" 0 "^$"

new "Verify compare"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^+\ *interface z {" "^+\ *type ianaift:v35;" "^+\ *description \"Config of interface z and ianaift:v35 type\";" --not-- "^\-" 

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

