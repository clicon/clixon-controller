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
            <template nc:operation="replace">
               <name>interfaces</name>
               <variables>
                 <variable><name>NAME</name></variable>
                 <variable><name>TYPE</name></variable>
               </variables>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Config of interface ${NAME},${NAME} and ${TYPE} type</description>
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

new "commit template local 1"
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
    err1 "$ret"
    exit 1
fi

new "Verify compare 1"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^+\ *interface z {" "^+\ *type ianaift:v35;" "^+\ *description \"Config of interface z,z and ianaift:v35 type\";" --not-- "^\-"

new "rollback"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 "^$"

# Use CLI load and apply template
new "delete template"
expectpart "$($clixon_cli -1 -f $CFG -m configure delete devices template interfaces)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 "^$"

new "CLI load template"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <variables>
                 <variable><name>NAME</name></variable>
                 <variable><name>TYPE</name></variable>
               </variables>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Config of interface ${NAME},${NAME} and ${TYPE} type</description>
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

new "commit template local 2"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Apply template CLI 1"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables NAME z TYPE ianaift:v35)" 0 "^$"

new "Verify compare 2"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare)" 0 "^+\ *interface z {" "^+\ *type ianaift:v35;" "^+\ *description \"Config of interface z,z and ianaift:v35 type\";" --not-- "^\-"

new "commit push"
expectpart "$($clixon_cli -1f $CFG -m configure commit push 2>&1)" 0 "^OK$"

# 1) commit add new version and diff
new "CLI load template (changed description)"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <variables>
                 <variable><name>NAME</name></variable>
                 <variable><name>TYPE</name></variable>
               </variables>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface>
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <description>Changed description</description>
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

new "Check description diff xml"
expectpart "$($clixon_cli -1f $CFG show compare xml 2>&1)" 0 "^-\ *<description>Config of interface " "^+\ *<description>Changed description</description>"

new "Check description diff text"
expectpart "$($clixon_cli -1f $CFG show compare text 2>&1)" 0 "^-\ *description \"Config of interface" "^+\ *description \"Changed description\""

new "commit local"
expectpart "$($clixon_cli -1f $CFG -m configure commit  2>&1)" 0 "^OK$"

new "Apply template CLI 2"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables NAME z TYPE ianaift:v35)" 0 "^$"

new "commit push"
expectpart "$($clixon_cli -1f $CFG -m configure commit push 2>&1)" 0 "^OK$"

# 2) add operation="merge" / "replace" within
new "CLI load template (removed description)"
# quote EOFfor $NAME
ret=$(${clixon_cli} -1f $CFG -m configure load merge xml <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
            <template nc:operation="replace">
               <name>interfaces</name>
               <variables>
                 <variable><name>NAME</name></variable>
                 <variable><name>TYPE</name></variable>
               </variables>
               <config>
                  <interfaces xmlns="http://openconfig.net/yang/interfaces">
                     <interface nc:operation="replace">
                        <name>${NAME}</name>
                        <config>
                           <name>${NAME}</name>
                           <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">${TYPE}</type>
                           <!-- removed description -->
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

new "commit local"
expectpart "$($clixon_cli -1f $CFG -m configure commit local 2>&1)" 0 "^$"

new "Apply template CLI 3"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables NAME z TYPE ianaift:v35)" 0 "^$"

new "Verify compare 3"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare xml)" 0 "^-\ *<description>Changed description</description>" --not-- "^+\ *"

new "commit push"
expectpart "$($clixon_cli -1f $CFG -m configure commit push 2>&1)" 0 "^OK$"

new "Check description removed"
expectpart "$($clixon_cli -1 -f $CFG show configuration devices device openconfig1 config interfaces interface)" 0 --not-- "<description"

new "check sync OK"
expectpart "$($clixon_cli -1f $CFG show devices $NAME check 2>&1)" 0 "OK" --not-- "out-of-sync"

# Negative tests CLI
new "Apply template CLI, missing NAME"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables TYPE ianaift:v35 2>&1)" 255 "missing-element Template variable <bad-element>NAME</bad-element>"

new "Apply template CLI, extra var"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables NAME z TYPE ianaift:v35 EXTRA foo 2>&1)" 255 "Unknown command"

new "Apply template CLI, duplicate var"
expectpart "$($clixon_cli -1 -f $CFG -m configure apply template interfaces openconfig* variables NAME z TYPE ianaift:v35 NAME x 2>&1)" 255 "data-not-unique"

# Negative tests NETCONF
new "Apply template NETCONF extra-var"
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
         <variable>
            <name>EXTRA</name>
            <value>dont exist</value>
         </variable>
      </variables>
   </device-template-apply>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"

new "Check no errors of apply"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -z "$match" ]; then
    err "netconf rpc-error expected" "$ret"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

