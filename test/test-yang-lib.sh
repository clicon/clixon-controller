#!/usr/bin/env bash
# Test RFC8528 YANG Schema Mount state
# Reset devices and backend
# Query controller of yang-schema-mount and yanglib of root and mount-points

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml

# Reset devices 
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

new "schema-mounts"
# Query top-level schema-mount
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
  <get>
    <filter type="subtree">
      <schema-mounts xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount"/>
    </filter>
  </get>
</rpc>]]>]]>
EOF
      )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "netconf rpc-error detected"
fi
new "match mount"
expected='<schema-mounts xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount"><mount-point><module>clixon-controller</module><label>device</label><config>true</config><inline/></mount-point></schema-mounts>'
match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    err "$expected" "$ret"
fi

new "per-device yanglibs"
# Query per-device yanglibs
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
  <get>
    <filter type="subtree">
      <devices xmlns="http://clicon.org/controller">
        <device>
          <config>
            <yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"/>
          </config>
        </device>
      </devices>
    </filter>
  </get>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "not rpc-error" "$ret"
fi

new "match yanglib"
expected='<devices xmlns="http://clicon.org/controller"><device><config><yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"><module-set><name>default</name><module><name>clixon-lib</name>'

match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    err "$expected" "$ret"
fi

new "match openconfig-acl"
expected='<module><name>openconfig-acl</name><revision>2023-02-06</revision><namespace>http://openconfig.net/yang/acl</namespace><location>NETCONF</location></module>'

match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    err "openconfig-acl" "$ret"
fi


if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
