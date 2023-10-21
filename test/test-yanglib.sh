#!/usr/bin/env bash
# Test RFC8528 YANG Schema Mount state
# Reset devices and backend
# Query controller of yang-schema-mount and yanglib of root and mount-points

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -eu

# Reset devices 
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

# Check backend is running
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
    echo "netconf rpc-error detected"
    exit 1
fi
new "match mount"
expected='<schema-mounts xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount"><mount-point><module>clixon-controller</module><label>root</label><config>true</config><inline/></mount-point></schema-mounts>'
match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    echo "netconf unexpected schema-mount"
    exit 1
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
    echo "netconf rpc-error detected"
    echo "$ret"
    exit 1
fi

new "match yanglib"

expected='<devices xmlns="http://clicon.org/controller"><device><config><yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"><module-set><name>mount</name><module><name>clixon-lib</name>'

match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    echo "netconf expected yang-library"
    echo "$ret"
    exit 1
fi

new "match openconfig-acl"
expected='<module><name>openconfig-acl</name><revision>2023-02-06</revision><namespace>http://openconfig.net/yang/acl</namespace></module>'

match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    echo "netconf expected openconfig-acl"
    echo "$ret"
    exit 1
fi


if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest

