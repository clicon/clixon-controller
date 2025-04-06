#!/usr/bin/env bash

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}
: ${check:=false}

# Reset devices with initial config
(. ./reset-devices.sh)

dir=/var/tmp/$0
modules=$dir/modules
fyang=$dir/ssh-users.yang
CFG=${SYSCONFDIR}/clixon/controller.xml
CFD=$dir/confdir
test -d $dir || mkdir -p $dir
test -d $modules || mkdir -p $modules
test -d $CFD || mkdir -p $CFD
pycode=$modules/ssh-users.py

: ${RUN_NOT_WORKING:=false}

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_NACM_CREDENTIALS>exact</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
</clixon-config>
EOF

cat <<EOF > $fyang
module ssh-users {
    namespace "http://clicon.org/ssh-users";
    prefix ssh-users;

    import ietf-inet-types { prefix inet; }
    import clixon-controller { prefix ctrl; }
    import clixon-autocli{
        prefix autocli;
    }
    revision 2023-05-22{
        description "Initial prototype";
    }
    augment "/ctrl:services" {
        list ssh-users {
            key service-name;
            leaf service-name {
                type string;
            }
            description "SSH users service";
            list username {
                key name;
                leaf name {
                    type string;
                }
                leaf ssh-key {
                    type string;
                }
                leaf role {
                     type string;
                }
            }
            uses ctrl:created-by-service;
        }
    }
}
EOF

cat <<EOF > $pycode
from clixon.element import Element
from clixon.parser import parse_template

SERVICE = "ssh-users"

USER_XML = """
<user cl:creator="ssh-users[service-name='{{SERVICE_NAME}}']" nc:operation="merge" xmlns:cl="http://clicon.org/lib">
    <username>{{USERNAME}}</username>
    <config>
        <username>{{USERNAME}}</username>
        <ssh-key>{{SSH_KEY}}</ssh-key>
        <role>{{ROLE}}</role>
    </config>
</user>
"""

def setup(root, log, **kwargs):
    try:
        _ = root.services
    except Exception:
        return

    for instance in root.services.ssh_users:
        if instance.service_name != kwargs["instance"]:
            continue
        for user in instance.username:
            service_name = instance.service_name.cdata
            username = user.name.cdata
            ssh_key = user.ssh_key.cdata
            role = user.role.cdata

            new_user = parse_template(USER_XML, SERVICE_NAME=service_name,
                                      USERNAME=username, SSH_KEY=ssh_key, ROLE=role).user

            for device in root.devices.device:
                if device.config.system.get_elements("aaa") == []:
                    device.config.system.create("aaa")
                if device.config.system.aaa.get_elements("authentication") == []:
                    device.config.system.aaa.create("authentication")
                if device.config.system.aaa.authentication.get_elements("users") == []:
                    device.config.system.aaa.authentication.create("users")
                device.config.system.aaa.authentication.users.add(new_user)

if __name__ == "__main__":
    setup()
EOF

# Sleep and verify devices are open
function sleep_open()
{
    for j in $(seq 1 10); do
        new "Verify devices are open"
        ret=$($clixon_cli -1 -f $CFG show connections)
        match1=$(echo "$ret" | grep --null -Eo "openconfig1.*OPEN") || true
        match2=$(echo "$ret" | grep --null -Eo "openconfig2.*OPEN") || true
        if [ -n "$match1" -a -n "$match2" ]; then
            break;
        fi
        echo "retry after sleep"
        sleep 1
    done
    if [ $j -eq 10 ]; then
        err "device openconfig OPEN" "Timeout" 
    fi
}

function nacm_init() {
    new "Create default configuration for NACM"
    expectpart "$($clixon_cli -1 -f $CFG -m configure delete nacm)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm enable-nacm true)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm read-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm write-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm exec-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm groups group test-group user-name root)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules group test-group)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
}

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

SERVICE_DIFF="openconfig1:
   <system xmlns="http://openconfig.net/yang/system">
+     <aaa>
+        <authentication>
+           <users>
+              <user>
+                 <username>test-user</username>
+                 <config>
+                    <username>test-user</username>
+                    <ssh-key>test-key</ssh-key>
+                    <role>operator</role>
+                 </config>
+              </user>
+           </users>
+        </authentication>
+     </aaa>
   </system>
openconfig2:
   <system xmlns="http://openconfig.net/yang/system">
+     <aaa>
+        <authentication>
+           <users>
+              <user>
+                 <username>test-user</username>
+                 <config>
+                    <username>test-user</username>
+                    <ssh-key>test-key</ssh-key>
+                    <role>operator</role>
+                 </config>
+              </user>
+           </users>
+        </authentication>
+     </aaa>
   </system>"

# Get the current user
USERNAME=$(whoami)

new "Wait backend"
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG connection close)" 0 ""

new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 ""

new "Sleep and verify devices are open"
sleep_open

new "Show device diff, should be empty"
expectpart "$($clixon_cli -1 -f $CFG show devices diff)" 0 "^$"

# Verify that service processes are running
new "Verify service processes are running"
expectpart "$($clixon_cli -1 -f $CFG processes service status)" 0 ".*running.*" ""

nacm_init

if $RUN_NOT_WORKING; then
    new "Deny access to device addr"
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules group test-group)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:addr access-operations read)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:addr action deny)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure show devices device openconfig1 addr)" 0 "<!-- openconfig1: -->
    <devices xmlns=\"http://clicon.org/controller\">
       <device>
          <name>openconfig1</name>
       </device>
    </devices>"
fi

nacm_init

new "Permit access to device addr"
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules group test-group)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:addr access-operations read)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:addr action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure show devices device openconfig1 addr)" 0 "<!-- openconfig1: -->
<devices xmlns=\"http://clicon.org/controller\">
   <device>
      <name>openconfig1</name>
      <addr>.*</addr>
   </device>
</devices>"

nacm_init

# Only run if the variable RUN_NOT_WOKRING is set
if $RUN_NOT_WORKING; then
    new "Deny access to device configuration"
    expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device \* config system configuration host-name test)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 "OK"
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules group test-group)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:host-name access-operations \*)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:host-name action deny)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device \* config system configuration host-name test)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 ".*clicon_rpc_edit_config: 679: Netconf error: Editing configuration: application access-denied access denied"
fi

new "Permit access to device configuration"
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:host-name action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device \* config system config hostname test)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 "OK"

nacm_init

if $RUN_NOT_WORKING; then
    new "Test NACM for services, add SSH user"
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system access-operations \*)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system action deny)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set services ssh-users test username test-user role operator)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure set services ssh-users test username test-user ssh-key test-key)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 ".*clicon_rpc_create_subscription: 1692: Netconf error: Create subscription: application access-denied access denied"
    expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""
fi

new "Test NACM for services, permit adding SSH user"
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set nacm rule-list test-rules rule /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set services ssh-users test username test-user role operator)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure set services ssh-users test username test-user ssh-key test-key)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "${SERVICE_DIFF}"

if $BE; then
     new "Kill old backend"
     #stop_backend -f $CFG
fi

endtest
