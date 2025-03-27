#!/usr/bin/env bash
# Change device config: Remove x, set y=122, and add z=99

set -u

echo "change-devices"

CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces">\
          <interface nc:operation="remove">\
            <name>x</name>\
          </interface>\
          <interface>
            <name>y</name>\
            <config>\
               <type nc:operation="replace" xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type>\
            </config>\
          </interface>\
         <interface nc_operation="merge">\
           <name>z</name>\
           <config>\
             <name>z</name>\
             <type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:usb</type>\
           </config>\
         </interface>\
       </interfaces>'

# Remove x, change y, and add z directly on devices
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    echo "Check config $NAME"
    ret=$(ssh $ip ${SSHID} -l ${USER} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
      ${CONFIG}
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><commit/></rpc>]]>]]>
EOF
       )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf rpc-error detected"
        echo "$ret"
        exit 1
    fi
    i=$((i+1))
done

echo "change-devices OK"
