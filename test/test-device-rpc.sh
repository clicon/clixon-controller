#!/usr/bin/env bash
# Send device RPCs clixon-lib:stats NETCONF
# Use rpc templates for CLI
# See test-template.sh for detailed tests of multiple templates, variables etc

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

# Send device-rpc stats
new "device-rpc NETCONF"
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
   <device-rpc xmlns="http://clicon.org/controller">
      <device>openconfig*</device>
      <config>
         <stats xmlns="http://clicon.org/lib">
            <modules>${MODULES}</modules>
         </stats>
      </config>
   </device-rpc>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"

new "Check no errors of apply"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "ok" "$ret"
fi

tid=$(echo $ret | grep --null -Eo ">[0-9]+</tid>" | grep --null -Eo "[0-9]+") || true
if [ -z "$tid" ]; then
    err "transaction-id" "$ret"
fi

new "devcice-rpc ping NETCONF"
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
   <device-rpc xmlns="http://clicon.org/controller">
      <device>openconfig*</device>
      <config>
         <ping xmlns="http://clicon.org/lib"/>
      </config>
   </device-rpc>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"

new "Check no errors of apply"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "ok" "$ret"
fi
tid=$(echo $ret | grep --null -Eo ">[0-9]+</tid>" | grep --null -Eo "[0-9]+") || true
if [ -z "$tid" ]; then
    err "transaction-id" "$ret"
fi

# Its complicated to wait for notify w netconf, skip that and do that for cli instead

# Use CLI load and apply template
new "delete template"
expectpart "$($clixon_cli -1 -f $CFG -m configure delete devices rpc-template stats)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 "^$"

new "CLI load template stats"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <rpc-template nc:operation="replace">
               <name>stats</name>
               <variables>
                 <variable><name>MODULES</name></variable>
               </variables>
               <config>
                  <stats xmlns="http://clicon.org/lib">
                     <modules>${MODULES}</modules>
                  </stats>
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

new "commit template local 2"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Send stats rpc"
expectpart "$($clixon_cli -1 -f $CFG rpc stats openconfig* variables MODULES false)" 0 "<name>openconfig1</name>" "<name>openconfig2</name>" '<module-sets xmlns="http://clicon.org/lib">'

new "CLI load template ping"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
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

new "commit template local 3"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "ping to all"
expectpart "$($clixon_cli -1 -f $CFG rpc ping openconfig*)" 0 "<name>openconfig1</name>" "<name>openconfig2</name>" "<ok"

new "ping to openconfig1"
expectpart "$($clixon_cli -1 -f $CFG rpc ping openconfig1)" 0 "<name>openconfig1</name>" "<ok" --not-- "<name>openconfig2</name>"

new "close one"
expectpart "$($clixon_cli -1 -f $CFG connect close openconfig1)" 0 ""

new "ping expect fail"
expectpart "$($clixon_cli -1 -f $CFG rpc ping openconfig1 2>&1)" 0 "No device connected" --not-- "<name>openconfig1</name>"

# Negative
new "open all"
expectpart "$($clixon_cli -1 -f $CFG connect open)" 0 ""

new "CLI load template non-existant"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <rpc-template nc:operation="replace">
               <name>notexist</name>
               <config>
                  <notexist xmlns="http://clicon.org/lib"/>
               </config>
            </rpc-template>
         </devices>
      </config>
EOF
)

new "commit template local 4"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "notexist to one"
expectpart "$($clixon_cli -1 -f $CFG rpc notexist openconfig1 2>&1)" 0 "Unrecognized RPC" "<bad-element>notexist</bad-element>" --not-- "<ok"

new "check open"
expectpart "$($clixon_cli -1 -f $CFG show connect openconfig1)" 0 "OPEN " --not-- CLOSED RPC_GENERIC

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

