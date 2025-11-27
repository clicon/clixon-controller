#!/usr/bin/env bash
# Minimal python service, for hello world / smoketest of services
# Just set a description and write it to all openconfig system/config-login-banner
# Note that the service does not really make sense, just for test

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Debug early exit
: ${early:=false}
>&2 echo "early=true for debug "

: ${check:=false}

# Reset devices with initial config
(. ./reset-devices.sh)

dir=/var/tmp/$0
modules=$dir/modules
fyang=$dir/example.yang
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $modules || mkdir -p $modules
test -d $CFD || mkdir -p $CFD
pycode=$modules/example.py

# Common NACM scripts
. ./nacm.sh

cat <<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:fcgi</CLICON_FEATURE>
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_YANG_DIR>${DATADIR}/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/common</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <CLICON_YANG_SCHEMA_MOUNT_SHARE>true</CLICON_YANG_SCHEMA_MOUNT_SHARE>
  <CLICON_YANG_USE_ORIGINAL>true</CLICON_YANG_USE_ORIGINAL>
  <CLICON_BACKEND_DIR>${LIBDIR}/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_USER>${CLICON_USER}</CLICON_BACKEND_USER>
  <CLICON_BACKEND_PRIVILEGES>none</CLICON_BACKEND_PRIVILEGES>
  <CLICON_BACKEND_PIDFILE>${LOCALSTATEDIR}/run/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_RESTCONF_INSTALLDIR>${SBINDIR}</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_CLI_DIR>${LIBDIR}/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>${LIBDIR}/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_AUTOCLI_CACHE_DIR>${LIBDIR}/controller/autocli</CLICON_AUTOCLI_CACHE_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_SOCK>${LOCALSTATEDIR}/run/controller.sock</CLICON_SOCK>
  <CLICON_SOCK_GROUP>${CLICON_GROUP}</CLICON_SOCK_GROUP>
  <CLICON_SOCK_PRIO>true</CLICON_SOCK_PRIO>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_MULTI>true</CLICON_XMLDB_MULTI>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_CREDENTIALS>exact</CLICON_NACM_CREDENTIALS>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <!-- Cant split config file since pyapi cant read that -->
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_server.py -f $CFG</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">$modules</CONTROLLER_PYAPI_MODULE_PATH>
  <CONTROLLER_PYAPI_MODULE_FILTER xmlns="http://clicon.org/controller-config"></CONTROLLER_PYAPI_MODULE_FILTER>
  <CONTROLLER_PYAPI_PIDFILE xmlns="http://clicon.org/controller-config">/tmp/clixon_pyapi.pid</CONTROLLER_PYAPI_PIDFILE>  <!-- clicon goup cannot write in ${LOCALSTATEDIR} -->
</clixon-config>
EOF

cat <<EOF > $fyang
module example {
    namespace "http://clicon.org/example";
    prefix ex;

    import clixon-controller {
        prefix ctrl;
    }
    revision 2025-05-17{
        description "TNSR service example";
    }
    augment "/ctrl:services" {
        list example {
            key service-name;
            leaf service-name {
                type string;
            }
            description "TNSR example service";
            uses ctrl:created-by-service;
        }
    }
}
EOF

cat <<EOF > $pycode
SERVICE = "example"

def setup(root, log, **kwargs):
    try:
        _ = root.services
    except Exception:
        return
    for instance in root.services.example:
        if instance.service_name != kwargs["instance"]:
            continue
        description = instance.service_name.get_data()
        for device in root.devices.device:
            device.config.system.config.create("login-banner", data=description)
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

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller
new "reset controller"
(. ./reset-controller.sh)

if ${early}; then
    exit # for starting controller with devices and debug
fi

# Configure service
new "Configure example services"
expectpart "$($clixon_cli -1 -f $CFG -m configure set service example Mybanner)" 0 ""

# Commit diff
# XXX Hangs here sometimes
new "Commit diff"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit diff)" 0 "openconfig1:
      <config xmlns=\"http://openconfig.net/yang/system\">
+        <login-banner>Mybanner</login-banner>
      </config>
openconfig2:
      <config xmlns=\"http://openconfig.net/yang/system\">
+        <login-banner>Mybanner</login-banner>
      </config>
OK"

new "Commit configuration"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

# Show the configuration on the devices using SSH
for container in $CONTAINERS; do
    new "Verify configuration on $container"
    expectpart "$(ssh ${SSHID} -l $USER $container clixon_cli -1 show configuration cli)" 0 "system config login-banner Mybanner"
done

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
