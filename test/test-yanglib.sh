#!/usr/bin/env bash
# Test RFC8528 YANG Schema Mount state
# Assume backend and devics running
# Reset devices and backend
# Query controller of yang-schema-mount and yanglib of root and mount-points

set -eu

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Reset devices 
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    ${PREFIX} clixon_backend -s init -f $CFG -z

    echo "Start new backend"
    ${PREFIX} clixon_backend -s init  -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller
. ./reset-controller.sh

new "schema-mounts"
# Query top-level schema-mount
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
  <get>
    <filter type="subtree">
      <schema-mounts xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount"/>
    </filter>
  </get>
</rpc>]]>]]>
EOF
      )
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

expected='<schema-mounts xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount"><mount-point><module>clixon-controller</module><label>root</label><config>true</config><inline/></mount-point></schema-mounts>'
match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    echo "netconf unexpected schema-mount"
    exit 1
fi

# Query per-device yanglibs
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
echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    echo "netconf rpc-error detected"
    exit 1
fi

expected='<devices xmlns="http://clicon.org/controller"><device><config><yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"><module-set><name>mount</name><module><name>clixon-autocli</name><revision>2022-02-11</revision><namespace>http://clicon.org/autocli</namespace></module>'
match=$(echo $ret | grep --null -Eo "$expected") || true
if [ -z "$match" ]; then
    echo "netconf unexpected yang-library"
    exit 1
fi

if $BE; then
    echo "Kill old backend"
    ${PREFIX} clixon_backend -s init -f $CFG -z
fi

echo "test-yanglib OK"

