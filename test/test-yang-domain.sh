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
: ${check:=false}

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
test -d $dir || mkdir -p $dir
CFD=$dir/conf.d
test -d $CFD || mkdir -p $CFD
mounts=$dir/mounts
test -d $mounts || mkdir $mounts
test -d $mounts/default || mkdir $mounts/default
test -d $mounts/isolated || mkdir $mounts/isolated
# Openconfig system revision. This may need to be updated now and then
REVISION="2025-07-08"
REVISION2="2024-09-24" # Old version

# Use this file and modify it for the isolated case
yangfile=openconfig-system@${REVISION}.yang
yangfile2=openconfig-system@${REVISION2}.yang
# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <!-- Error in validation check of STATE + mandatory variable
   CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML-->
  <CLICON_YANG_DOMAIN_DIR>$mounts</CLICON_YANG_DOMAIN_DIR>
</clixon-config>
EOF

cp ../src/autocli.xml $CFD/

# Reset devices with initial config
(. ./reset-devices.sh)

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
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
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
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
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig2 device-domain isolated)" 0 ""

new "reset-controller: Controller commit"
ret=$(${clixon_netconf} -q0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
      )
#echo "$ret"

new "Open connection to openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open openconfig1)" 0 ""

new "Find original $yangfile"
# Find openconfig-system@${REVISION}.yang
if [ ! -f $mounts/default/$yangfile ]; then
    if [ ! -f $mounts/default/$yangfile2 ]; then
        err "$yangfile2" "Not found"
    fi
    yangfile=$yangfile2
fi

new "check cli memory openconfig1"
expectpart "$($clixon_cli -1f $CFG -E $CFD show mem cli -- -g 2>&1)" 0 "^top" "^default" --not-- "^isolated"

new "check backend memory openconfig1"
expectpart "$($clixon_cli -1f $CFG -E $CFD show mem backend 2>&1)" 0 "^top" "^default" --not-- "^isolated"

new "Find isolated $yangfile"
cat <<EOF > $mounts/isolated/$yangfile
module openconfig-system {
   yang-version "1";
   namespace "http://openconfig.net/yang/system";
   prefix "oc-sys";
   revision "${REVISION}" {
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
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open openconfig2)" 0 ""

new "check cli memory with isolated"
expectpart "$($clixon_cli -1f $CFG -E $CFD show mem cli -- -g 2>&1)" 0 "^top" "^default" "^isolated"

new "check backend memory openconfig1"
expectpart "$($clixon_cli -1f $CFG -E $CFD show mem backend 2>&1)" 0 "^top" "^default" "^isolated"

# 4. Check that yang content is different

new "Set banner on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config login-banner xyz)" 0 ""

new "Set banner on openconfig2, expect fail"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig2 config system config login-banner xyz 2> /dev/null)" 255 ""

new "Set isolated on openconfig1 expect fail"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig1 config system config isolated xyz 2> /dev/null)" 255 ""

new "Set isolated on openconfig2"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig2 config system config isolated xyz)" 0 ""

new "Commit local"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""

new "Show config"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show config devices device)" 0 "<login-banner>xyz</login-banner>" "<isolated>xyz</isolated>"

new "Remove other domain"
if [ -d $mounts/other ]; then
    rm -rf $mounts/other
fi
new "Move openconfig2 to other domain"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure set devices device openconfig2 device-domain other)" 0 ""

new "Commit local"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit local)" 0 ""

new "Reconnect connection"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection reconnect openconfig2)" 0 ""

new "Check $mounts/other exists"
if [ ! -d $mounts/other ]; then
    err "$mounts/other" "$mounts/other not found"
fi

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -f $CFG -E $CFD -z
fi

endtest
