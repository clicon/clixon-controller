#!/usr/bin/env bash
# Test isolated yang domains
# Two devices: openconfig1 and openconfig2
# 1. Create an isolated domain, connect openconfig2 to that
# 2. Connect to openconfig1, populate default mount-dir
# 3. Copy from default to isolated, but modify system yang (ONLY)
#    But keep it enough equal so we can do a sync with the hostname
# 4. Check that yang content of system yang is different
#    Old has following under config/system/config:
#             domain-name, hostname, login-banner, motd-banner
#    New has only: hostname, isolated

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Set if also push, not only change (useful for manually doing push)
: ${push:=true}

: ${check:=false}

CFG=$dir/controller.xml

mounts=$dir/mounts
test -d $mounts || mkdir $mounts
test -d $mounts/default || mkdir $mounts/default
test -d $mounts/isolated || mkdir $mounts/isolated

# Use this file and modify it for the isolated case
yangfile=openconfig-system@2023-06-16.yang

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <!-- See also src/Makefile.in and yang/Makefile.in -->
  <CLICON_CONFIGFILE>/usr/local/etc/clixon/controller.xml</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>/usr/local/etc/clixon/controller</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>${DATADIR}/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/common</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>  
  <CLICON_YANG_MAIN_DIR>${DATADIR}/controller/main</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$mounts</CLICON_YANG_DOMAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>/usr/local/var/run/controller/controller.sock</CLICON_SOCK>
  <!-- NB Backend socket prioritixed over SB devices -->
  <CLICON_SOCK_PRIO>true</CLICON_SOCK_PRIO>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/run/controller/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/controller</CLICON_XMLDB_DIR>
  <!-- Split datsatores into multiple files -->
  <CLICON_XMLDB_MULTI>true</CLICON_XMLDB_MULTI>
  <CLICON_STARTUP_MODE>running</CLICON_STARTUP_MODE>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_RESTCONF_USER>clicon</CLICON_RESTCONF_USER>  
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <!-- Cannot use drop because XMLDB_MULTI creates new files -->
  <CLICON_BACKEND_PRIVILEGES>none</CLICON_BACKEND_PRIVILEGES>
  <CLICON_VALIDATE_STATE_XML>false</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <!-- Yang schema mount is enabled for controller -->
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <!-- Optimization to share YANGs omong schema-mounts -->
  <CLICON_YANG_SCHEMA_MOUNT_SHARE>true</CLICON_YANG_SCHEMA_MOUNT_SHARE>
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <!-- Log to syslog and stderr. no log length limitation -->
  <CLICON_LOG_DESTINATION>syslog stderr</CLICON_LOG_DESTINATION>
  <CLICON_LOG_STRING_LIMIT>0</CLICON_LOG_STRING_LIMIT>
  <!-- Enable for exclusive lock on edit -->
  <CLICON_AUTOLOCK>false</CLICON_AUTOLOCK>
  <!-- NACM is inlined in configuration -->
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  <!-- Default output format for show config etc -->
  <CLICON_CLI_OUTPUT_FORMAT>xml</CLICON_CLI_OUTPUT_FORMAT>
</clixon-config>
EOF

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "wait backend"
wait_backend

# Modified from reset-controller.sh

# Send edit-config to controller with initial device meta-config
function init_device_config2()
{
    NAME=$1
    ip=$2

    new "reset-controller: Init device $NAME edit-config"
    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
  message-id="42">
  <edit-config>
    <target>
      <candidate/>
    </target>
    <default-operation>none</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller">
	<device nc:operation="replace">
	  <name>$NAME</name>
	  <enabled>true</enabled>
          <device-domain>default</device-domain>
	  <description>Clixon example container</description>
	  <conn-type>NETCONF_SSH</conn-type>
	  <user>$USER</user>
	  <addr>$ip</addr>
	  <yang-config>VALIDATE</yang-config>
	  <config/>
	</device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
       )
}

i=1
# Loop adding top-level device meta info
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    i=$((i+1))
    jmax=5
    for j in $(seq 1 $jmax); do
	init_device_config2 $NAME $ip
#	echo "$ret"
	match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
	if [ -z "$match" ]; then
	    break
	fi
	match=$(echo "$ret" | grep --null -Eo "<error-tag>lock-denied</error-tag") || true
	if [ -z "$match" ]; then
	    err1 "netconf rpc-error detected"
	fi
	new "reset-controller: retry after sleep"
	sleep $sleep
    done
done

new "Set openconfig2 to isolated domain"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig2 device-domain isolated)" 0 ""

new "reset-controller: Controller commit"
ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
      )
#echo "$ret"

new "Open connection to openconfig1"
expectpart "$($clixon_cli -1 -f $CFG connection open openconfig1)" 0 ""

new "Find original $yangfile"
# Find openconfig-system@2023-06-16.yang
if [ ! -f $mounts/default/$yangfile ]; then
    err "$yangfile" "Not found"
fi

new "Find isolated $yangfile"
cat <<EOF > $mounts/isolated/$yangfile
module openconfig-system {
   yang-version "1";
   namespace "http://openconfig.net/yang/system";
   prefix "oc-sys";
   revision "2023-06-16" {
      description
         "Reordered imports to be alphabetical.";
      reference "0.17.1";
   }
   grouping system-top {
      description
         "Top level system data containers";
      container system {
          description
              "Enclosing container for system-related configuration and
               operational state data";
          container config {
              description "Global configuration data for the system";
              leaf hostname {
                  type string;
                  description
                      "The hostname of the device -- should be a single domain
                       label, without the domain.";
              }
              leaf isolated {
                  type string;
              }
          }
          container state {
              config false;
              description "Global operational state data for the system";
              leaf current-datetime {
                  type string;
                  description
                      "The current system date and time.";
              }
          }
      }
   }
   uses system-top;  
}
EOF

new "Open connection to openconfig2 in isolated domain"
expectpart "$($clixon_cli -1 -f $CFG connection open openconfig2)" 0 ""

# 4. Check that yang content is different

new "Set banner on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config login-banner xyz)" 0 ""

new "Set banner on openconfig2, expect fail"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig2 config system config login-banner xyz)" 255 ""

new "Set isolated on openconfig1 expect fail"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config isolated xyz)" 255 ""

new "Set isolated on openconfig2"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig2 config system config isolated xyz)" 0 ""

new "Validate"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit local)" 0 ""

new "Show config"
expectpart "$($clixon_cli -1 -f $CFG show config devices device)" 0 "<login-banner>xyz</login-banner>" "<isolated>xyz</isolated>"

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -f $CFG -z
fi

endtest
