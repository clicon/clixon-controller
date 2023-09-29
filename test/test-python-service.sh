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

if [ ! -d $dir ]; then
    mkdir $dir
else
    rm -rf $dir/*
fi

if [ ! -d $modules ]; then
	mkdir $modules
fi

fyang=$dir/ssh-users.yang
cfg=$dir/controller.xml
pycode=$modules/ssh-users.py

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>/usr/local/etc/controller.xml</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>/usr/local/var/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/controller.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>www-data</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>

  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">/usr/local/bin/clixon_server.py -f $cfg -F</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">$modules</CONTROLLER_PYAPI_MODULE_PATH>
  <CONTROLLER_PYAPI_MODULE_FILTER xmlns="http://clicon.org/controller-config"></CONTROLLER_PYAPI_MODULE_FILTER>
  <CONTROLLER_PYAPI_PIDFILE xmlns="http://clicon.org/controller-config">/tmp/clixon_server.pid</CONTROLLER_PYAPI_PIDFILE>
  <autocli>
     <module-default>false</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include openconfig</name>
       <module-name>openconfig*</module-name>
       <operation>enable</operation>
     </rule>
     <!-- there are many more arista/openconfig top-level modules -->
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module ssh-users {
    namespace "http://clicon.org/ssh-users";
    prefix ssh-users;

    import ietf-inet-types { prefix inet; }
    import clixon-controller { prefix ctrl; }

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
        }
    }
}
EOF

cat <<EOF > $pycode
from clixon.clixon import rpc
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

@rpc()
def setup(root, log, **kwargs):
    try:
        _ = root.services
    except Exception:
        return

    for instance in root.services.ssh_users:
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
        ret=$($clixon_cli -1 -f $CFG show devices)
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

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $cfg -z

    new "Start new backend -s init -f $cfg"
    start_backend -s init -f $cfg
fi

# Check backend is running
wait_backend

# Reset controller 
new "reset controller"
(. ./reset-controller.sh)

new "Close all devices"
expectpart "$($clixon_cli -1 -f $cfg connection close)" 0 ""

new "Connect to devices"
expectpart "$($clixon_cli -1 -f $cfg connection open)" 0 ""

new "Sleep and verify devices are open"
sleep_open

new "Show device diff, should be empty"
expectpart "$($clixon_cli -1 -f $cfg show devices diff)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg show devices openconfig* diff)" 0 ""

# Verify that service processes are running
new "Verify service processes are running"
expectpart "$($clixon_cli -1 -f $cfg processes service status)" 0 ".*running.*" ""

# Why do we need to restart the service?
expectpart "$($clixon_cli -1 -f $cfg processes service restart)" 0 '<ok xmlns="http://clicon.org/lib"/>'

# Make sure that service processes are running after restart
new "Verify service processes are running after restart"
expectpart "$($clixon_cli -1 -f $cfg processes service status)" 0 ".*running.*" ""

# Configure serice
new "Configure ssh-users with user test1"
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test1)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test1 username test1)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test1 username test1 role admin)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test1 username test1 ssh-key key1)" 0 ""

# Commit diff
new "Commit diff for user test1"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit diff)" 0 "openconfig1:
  <system>
+     <aaa>
+        <authentication>
+           <users>
+              <user>
+                 <username>test1</username>
+                 <config>
+                    <username>test1</username>
+                    <ssh-key>key1</ssh-key>
+                    <role>admin</role>
+                 </config>
+              </user>
+           </users>
+        </authentication>
+     </aaa>
  </system>
openconfig2:
  <system>
+     <aaa>
+        <authentication>
+           <users>
+              <user>
+                 <username>test1</username>
+                 <config>
+                    <username>test1</username>
+                    <ssh-key>key1</ssh-key>
+                    <role>admin</role>
+                 </config>
+              </user>
+           </users>
+        </authentication>
+     </aaa>
  </system>
OK"

new "Commit configuration for user test1"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit)" 0 ""

# Show the configuration on the devices using SSH
for container in $CONTAINERS; do
    new "Verify configuration on $container for user test1"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system aaa authentication users user test1 config username test1" "system aaa authentication users user test1 config ssh-key key1" "system aaa authentication users user test1 config role admin"
done

new "Configure ssh-users with user test2"
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test2)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test2 username test2)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test2 username test2 role admin)" 0 ""
expectpart "$($clixon_cli -1 -f $cfg -m configure set service ssh-users test2 username test2 ssh-key key2)" 0 ""

# Commit diff
new "Commit diff for user test2"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit diff)" 0 "openconfig1:
           <users>
+              <user>
+                 <username>test2</username>
+                 <config>
+                    <username>test2</username>
+                    <ssh-key>key2</ssh-key>
+                    <role>admin</role>
+                 </config>
+              </user>
           </users>
openconfig2:
           <users>
+              <user>
+                 <username>test2</username>
+                 <config>
+                    <username>test2</username>
+                    <ssh-key>key2</ssh-key>
+                    <role>admin</role>
+                 </config>
+              </user>
           </users>
OK"

new "Commit configuration for user test2"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit)" 0 ""

# Show the configuration on the devices using SSH
for container in $CONTAINERS; do
    new "Verify configuration on $container for user test2"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system aaa authentication users user test1 config username test1" "system aaa authentication users user test1 config ssh-key key1" "system aaa authentication users user test1 config role admin" "system aaa authentication users user test2 config username test2" "system aaa authentication users user test2 config ssh-key key2" "system aaa authentication users user test2 config role admin"
done

new "Delete user test1"
expectpart "$($clixon_cli -1 -f $cfg -m configure delete service ssh-users test1)" 0 ""

new "Commit diff for user test1"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit diff)" 0 "openconfig1:
  <system>
-     <aaa>
-        <authentication>
-           <users>
-              <user>
-                 <username>test1</username>
-                 <config>
-                    <username>test1</username>
-                    <ssh-key>key1</ssh-key>
-                    <role>admin</role>
-                 </config>
-              </user>
-           </users>
-        </authentication>
-     </aaa>
  </system>
openconfig2:
  <system>
-     <aaa>
-        <authentication>
-           <users>
-              <user>
-                 <username>test1</username>
-                 <config>
-                    <username>test1</username>
-                    <ssh-key>key1</ssh-key>
-                    <role>admin</role>
-                 </config>
-              </user>
-           </users>
-        </authentication>
-     </aaa>
  </system>
OK"

# Show the configuration on the devices using SSH
for container in $CONTAINERS; do
    new "Verify configuration on $container for user test1"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 "system aaa authentication users user test2 config username test2" "system aaa authentication users user test2 config ssh-key key2" "system aaa authentication users user test2 config role admin"
done

new "Delete user test2"
expectpart "$($clixon_cli -1 -f $cfg -m configure delete service ssh-users test2)" 0 ""

new "Commit diff for user test2"
expectpart "$($clixon_cli -1 -f $cfg -m configure commit diff)" 0 "openconfig1:
  <system>
-     <aaa>
-        <authentication>
-           <users>
-              <user>
-                 <username>test2</username>
-                 <config>
-                    <username>test2</username>
-                    <ssh-key>key2</ssh-key>
-                    <role>admin</role>
-                 </config>
-              </user>
-           </users>
-        </authentication>
-     </aaa>
  </system>
openconfig2:
  <system>
-     <aaa>
-        <authentication>
-           <users>
-              <user>
-                 <username>test2</username>
-                 <config>
-                    <username>test2</username>
-                    <ssh-key>key2</ssh-key>
-                    <role>admin</role>
-                 </config>
-              </user>
-           </users>
-        </authentication>
-     </aaa>
  </system>
OK"

# Show the configuration on the devices using SSH
for container in $CONTAINERS; do
    new "Verify configuration on $container for user test2"
    expectpart "$(ssh -l $USER $container clixon_cli -1 show configuration cli)" 0 ""
done

if $BE; then
    new "Kill old backend"
    stop_backend -f $cfg
fi

endtest
