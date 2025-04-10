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
diff=$CFD/diff.xml
test -d $dir || mkdir -p $dir
test -d $modules || mkdir -p $modules
test -d $CFD || mkdir -p $CFD
pycode=$modules/ssh-users.py

: ${RUN_NOT_WORKING:=false}

# Specialize controller.xml
cat<<EOF > $diff
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:fcgi</CLICON_FEATURE>
  <CLICON_CONFIGFILE>/usr/local/etc/clixon/controller.xml</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>/usr/local/etc/clixon/controller</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_YANG_DIR>${DATADIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>${dir}</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>${dir}</CLICON_YANG_DOMAIN_DIR>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <CLICON_YANG_SCHEMA_MOUNT_SHARE>true</CLICON_YANG_SCHEMA_MOUNT_SHARE>
  <CLICON_YANG_USE_ORIGINAL>true</CLICON_YANG_USE_ORIGINAL>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <CLICON_BACKEND_PRIVILEGES>none</CLICON_BACKEND_PRIVILEGES>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/run/controller/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_BACKEND_RESTCONF_PROCESS>true</CLICON_BACKEND_RESTCONF_PROCESS>
  <CLICON_NETCONF_DUPLICATE_ALLOW>true</CLICON_NETCONF_DUPLICATE_ALLOW>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_RESTCONF_USER>clicon</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_DIR>/usr/local/lib/controller/restconf</CLICON_RESTCONF_DIR>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_CLI_OUTPUT_FORMAT>xml</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_SOCK>/usr/local/var/run/controller/controller.sock</CLICON_SOCK>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_SOCK_PRIO>true</CLICON_SOCK_PRIO>
  <CLICON_AUTOLOCK>false</CLICON_AUTOLOCK>
  <CLICON_XMLDB_DIR>${dir}</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_MULTI>true</CLICON_XMLDB_MULTI>
  <CLICON_STARTUP_MODE>running</CLICON_STARTUP_MODE>
  <CLICON_NACM_CREDENTIALS>except</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>  
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_LOG_DESTINATION>syslog stderr</CLICON_LOG_DESTINATION>
  <CLICON_LOG_STRING_LIMIT>0</CLICON_LOG_STRING_LIMIT>
  <CLICON_CLI_PIPE_DIR>/usr/local/share/controller/pipe/</CLICON_CLI_PIPE_DIR>
  <CONTROLLER_SERVICES_USER xmlns="http://clicon.org/controller-config">clicon</CONTROLLER_SERVICES_USER> 
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">/usr/local/bin/clixon_server.py -d -f ${CFG} -m ${modules}</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">${modules}</CONTROLLER_PYAPI_MODULE_PATH>       
  <CONTROLLER_PYAPI_MODULE_FILTER xmlns="http://clicon.org/controller-config"></CONTROLLER_PYAPI_MODULE_FILTER>
  <CONTROLLER_PYAPI_PIDFILE xmlns="http://clicon.org/controller-config">/usr/local/var/run/controller/clixon_server.pid</CONTROLLER_PYAPI_PIDFILE>
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
                if device.config.get_elements("system") == []:
                    device.config.create("system", attributes={"xmlns": "http://openconfig.net/yang/system"})
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
        ret=$($clixon_cli -1 -f $CFG -E $CFD show connections)
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
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure delete nacm)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm enable-nacm true)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm read-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm write-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm exec-default permit)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm groups group test-group user-name root)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
}

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG -E $CFD
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

new "Deny access to device addr"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address access-operations read)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address path /ctrl:devices/ctrl:device/ctrl:addr)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show devices device openconfig1 addr)" 0 "<!-- openconfig1: -->
<devices xmlns=\"http://clicon.org/controller\">
   <device>
      <name>openconfig1</name>
   </device>
</devices>"

nacm_init

new "Permit access to device addr"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address access-operations read)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule device-address path /ctrl:devices/ctrl:device/ctrl:addr)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure show devices device openconfig1 addr)" 0 "<!-- openconfig1: -->
<devices xmlns=\"http://clicon.org/controller\">
   <device>
      <name>openconfig1</name>
      <addr>.*</addr>
   </device>
</devices>"

nacm_init

new "Deny access to device hostname but make sure we can modify doamin-name"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:hostname)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config hostname test 2>&1)" 255 ".*Netconf error: Editing configuration: application access-denied access denied.*"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config domain-name example.com 2>&1)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit diff 2>&1)" 0 "openconfig1:
      <config xmlns="http://openconfig.net/yang/system">
+        <domain-name>example.com</domain-name>
      </config>
OK"

new "Permit access to device configuration"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config hostname test)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit 2>&1)" 0 "OK"

nacm_init

new "Test NACM for services, deny adding SSH user"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user role operator)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user ssh-key test-key)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit 2>&1)" 0 ".*access denied.*"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

new "Test NACM for services, permit adding SSH user"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user role operator)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user ssh-key test-key)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit diff)" 0 "${SERVICE_DIFF}"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure rollback)" 0 ""

nacm_init

new "Test NACM for services, permit adding SSH user but deny modifying hostname"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:hostname)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user role operator)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set services ssh-users test username test-user ssh-key test-key)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit diff)" 0 "${SERVICE_DIFF}"

nacm_init

new "Test NACM and RPCs, deny config-pull"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule rpc-name config-pull)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG pull 2>&1)" 255 ".*application access-denied access denied"

nacm_init

new "Test NACM and RPCs, allow config-pull"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule rpc-name config-pull)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG pull 2>&1)" 0 "OK"

if $BE; then
     new "Kill old backend"
     stop_backend -f $CFG -E $CFD
fi

endtest
