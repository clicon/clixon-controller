#!/usr/bin/env bash
# Load and appy RPC template for clixon-lib:stats
# Define via NETCONF and CLI
# Check output
# See test-template.sh for detaiuled tests of multiple templates, variables etc

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

CFG=${SYSCONFDIR}/clixon/controller.xml

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

new "CLI show device rpc"
expectpart "$($clixon_cli -1f $CFG show devices openconfig* rpc clixon* 2>&1)" 0 "clixon-lib:debug" "clixon-lib:stats"

new "CLI show device yang"
expectpart "$($clixon_cli -1f $CFG show devices openconfig1 rpc clixon-lib:stats yang 2>&1)" 0 "rpc stats {"

new "Create template w NETCONF"
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
            <rpc-template nc:operation="replace">
               <name>stats</name>
               <variables>
                 <variable><name>MODULES</name></variable>
               </variables>
               <rpc>
                 <module>clixon-lib</module>
                 <name>stats</name>
               </rpc>
               <input>
                  <stats xmlns="http://clicon.org/lib">
                     <modules>${MODULES}</modules>
                  </stats>
               </input>
            </rpc-template>
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

new "commit rpc template local 1"
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
   <device-rpc-template-apply xmlns="http://clicon.org/controller">
      <devname>openconfig*</devname>
      <template>stats</template>
      <variables>
         <variable>
            <name>MODULES</name>
            <value>true</value>
         </variable>
      </variables>
   </device-rpc-template-apply>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"

new "Check no errors of apply"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    err1 "$ret"
    exit 1
fi

# Its complicated to wait for notify w netconf, skip that and do that for cli instead

# Use CLI load and apply template
new "delete template"
expectpart "$($clixon_cli -1 -f $CFG -m configure delete devices rpc-template stats)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 "^$"

new "CLI load template xml"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <rpc-template nc:operation="replace">
               <name>stats</name>
               <variables>
                 <variable><name>MODULES</name></variable>
               </variables>
               <rpc>
                 <module>clixon-lib</module>
                 <name>stats</name>
               </rpc>
               <input>
                  <stats xmlns="http://clicon.org/lib">
                     <modules>${MODULES}</modules>
                  </stats>
               </input>
            </rpc-template>
         </devices>
      </config>
EOF
)

#echo "ret:$ret"

if [ -n "$ret" ]; then
    err1 "$ret"
    exit 1
fi

new "commit template local 2"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Apply template CLI 1"
expectpart "$($clixon_cli -1 -f $CFG apply rpc-template stats openconfig* variables MODULES false)" 0 "<name>openconfig1</name>" "<name>openconfig2</name>" '<module-sets xmlns="http://clicon.org/lib">'

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

