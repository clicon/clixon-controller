#!/usr/bin/env bash
# NACM + RESTCONF tests

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${check:=true}
: ${delete:=false}

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
DATA="{\"clixon-controller:input\":{\"device\":\"*\",\"push\":\"COMMIT\",\"actions\":\"FORCE\",\"source\":\"ds:candidate\",\"service-instance\":\"ssh-users[service-name='test']\"}}"

: ${TIMEOUT:=10}
DATE=$(date -u +\"%Y-%m-%d\")

# RESTCONFIG for bring-your-own restconf
# file means /var/log/clixon_restconf.log
# Example debug: 1048575
RESTCONFIG=$(restconf_config client-certificate false 0 file ${TIMEOUT} false)

if [ $? -ne 0 ]; then
    err1 \"Error when generating certs\"
fi

certdir=$dir/certs
usercert ${certdir} andy
usercert ${certdir} bob

# To debug, set CLICON_BACKEND_RESTCONF_PROCESS=false and start clixon_restconf manually
# clixon_restconf -f /usr/local/etc/clixon/controller.xml -E /var/tmp/./test-restconf.sh/confdir -o CLICON_BACKEND_RESTCONF_PROCESS=true
# and comment test "Verify restconf"

# Specialize controller.xml
cat<<EOF > $diff
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <!--CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE-->
  <CLICON_NACM_CREDENTIALS>except</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_BACKEND_RESTCONF_PROCESS>true</CLICON_BACKEND_RESTCONF_PROCESS>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_STREAM_DISCOVERY_RFC8040>true</CLICON_STREAM_DISCOVERY_RFC8040>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">/usr/local/bin/clixon_server.py -d -f ${CFG} -m ${modules}</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">${modules}</CONTROLLER_PYAPI_MODULE_PATH>
</clixon-config>
EOF

# Specialize autocli.xml for openconfig vs ietf interfaces
cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <clispec-cache>read</clispec-cache>
     <rule>
       <name>exclude ietf interfaces</name>
       <module-name>ietf-interfaces</module-name>
       <operation>disable</operation>
     </rule>
  </autocli>
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

# Then start from startup which by default should start it
# First disable services process
test -d $dir/startup.d || mkdir -p $dir/startup.d
cat <<EOF > $dir/startup.d/0.xml
<config>
  <processes xmlns="http://clicon.org/controller">
     <services>
        <enabled>true</enabled>
     </services>
  </processes>
  ${RESTCONFIG}
</config>
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
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm groups group test-group user-name andy)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules group test-group)" 0 ""
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
}

if $BE; then
    new "Kill old backend"
    stop_backend -s startup -f $CFG -E $CFD

    new "Start new backend -s startup -f $CFG -E $CFD"
    sudo clixon_backend -s startup -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

if [ $valgrindtest -eq 3 ]; then # restconf mem test
    sleep 10
fi

new "Wait restconf"
wait_restconf

# --key $certdir/andy.key --cert $certdir/andy.crt
new "Verify restconf"
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-lib:process-control -d '{"clixon-lib:input":{"name":"restconf","operation":"status"}}')" 0 "HTTP/$HVER 200" '{"clixon-lib:output":{"active":true,"description":"Clixon RESTCONF process"'

new "Close all devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close)" 0 ""

new "Connect to devices"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open)" 0 ""

new "Sleep and verify devices are open"
sleep_open

new "Show device diff, should be empty"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices diff)" 0 "^$"

# Verify that service processes are running
new "Verify service processes are running"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD processes service status)" 0 ".*running.*" ""

nacm_init

new "Verify that we have access to hostname"
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X GET -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-system:system/config/hostname)" 0 '.*{"openconfig-system:hostname":"openconfig1"}'

new "Deny access to hostname"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action deny)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X GET -H "Content-Type: application/yang-data+xml" $RCPROTO://localhost/restconf/data/clixon-controller:devices/device=openconfig1/config/openconfig-system:system/config/hostname)" 0 '.*{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"invalid-value","error-severity":"error","error-message":"Instance does not exist"}}}'

new "Create a new service instance"
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-controller:services -d '{"ssh-users:ssh-users": [{"service-name": "test","username": [{"name": "test","ssh-key": "AAAA","role": "operator"}]}]}')" 0 "HTTP/$HVER 201"

new "Apply service as Andy, should be denied"
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json'
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions last)" 0 ".*access denied.*"

new "Apply service again as Bob, should be denied"
expectpart "$(curl $CURLOPTS --key $certdir/bob.key --cert $certdir/bob.crt -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json'

new "Check with cli"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions last)" 0 ".*SUCCESS.*"

new "Permit Andy to run service"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule access-operations \*)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule action permit)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set nacm rule-list test-rules rule test-rule path /ctrl:devices/ctrl:device/ctrl:config/oc-sys:system)" 0 ""
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""
expectpart "$(curl $CURLOPTS --key $certdir/andy.key --cert $certdir/andy.crt -X POST -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/operations/clixon-controller:controller-commit -d "${DATA}")" 0 "HTTP/$HVER 200" 'Content-Type: application/yang-data+json'

new "Check with cli"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions last)" 0 ".*SUCCESS.*" --not-- ".*access denied.*"

# Kill clixon_restconf
if $RC; then
    new "Kill restconf daemon"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD processes restconf stop)" 0 ""
    if [ $valgrindtest -eq 3 ]; then
        sleep 1
        checkvalgrind
    fi
fi

# Kill backend
if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -E $CFD -z
fi

endtest
