# Controller test script for cli show commands

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
fin=$dir/in

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_CLISPEC_DIR>${dir}</CLICON_CLISPEC_DIR>
  <CLICON_VALIDATE_STATE_XML>false</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
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

cat<<EOF > $dir/controller_operation.cli
CLICON_MODE="operation";
CLICON_PROMPT="%U@%H> ";
CLICON_PLUGIN="controller_cli";
# Al11: Auto edit mode
# Autocli syntax tree operations
configure("Change to configure mode"), cli_set_mode("configure");
exit("Quit"), cli_quit();
quit("Quit"), cli_quit();
connection("Change connection state of one or several devices") {
   close("Close open connections"), cli_connection_change("CLOSE", false);{
      (<name:string>("device pattern")|
       <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern")), cli_connection_change("CLOSE", false);
   }
   open("Open closed connections"), cli_connection_change("OPEN", false);{
      wait("Block until completion"), cli_connection_change("OPEN", true);
      (<name:string>("device pattern")|
       <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern")), cli_connection_change("OPEN", false);{
         wait("Block until completion"), cli_connection_change("OPEN", true);
      }
   }
   reconnect("Close all open connections and open all connections"), cli_connection_change("RECONNECT", false);{
      wait("Block until completion"), cli_connection_change("RECONNECT", true);
      (<name:string>("device pattern")|
       <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern")), cli_connection_change("RECONNECT", false);{
                wait("Block until completion"), cli_connection_change("RECONNECT", true);
      }
   }
}
show("Show a particular state of the system"){
    configuration("Show configuration"), cli_show_auto_mode("running", "text", true, false);{
      xml, cli_show_auto_mode("running", "xml", false, false);{
      	   @datamodelshow, cli_show_auto_devs("running", "xml", false, false, "explicit");
      }
      text, cli_show_auto_mode("running", "text", false, false);{
           @datamodelshow, cli_show_auto_devs("running", "text", false, false, "explicit");
      }
      json, cli_show_auto_mode("running", "json", false, false);{
            @datamodelshow, cli_show_auto_devs("running", "json", false, false, "explicit");
      }
      cli, cli_show_auto_mode("running", "cli", false, false, "explicit", "set ");{
           @datamodelshow, cli_show_auto_devs("running", "cli", false, false, "explicit", "set ");
      }
    }
    detail("Show details about a configured node: yang, namespaces, etc"){
        @basemodel, @remove:act-list, cli_show_config_detail();
    }
    connections("Show state of connection state of devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_show_connections();{
                 check("Check if device is in sync"), check_device_db("default");
                 diff("Compare remote device config with local"), compare_device_db_dev("default");
                 detail("Show detailed state"), cli_show_connections("detail");
             }
    state("Show configuration and state"), cli_show_auto_mode("running", "xml", false, true);
}
pull("sync config from one or multiple devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_rpc_pull("replace");{
                    replace, cli_rpc_pull("replace");
                    merge, cli_rpc_pull("merge");
}
EOF

cat<<EOF > $dir/controller_configure.cli
CLICON_MODE="configure";
CLICON_PROMPT="%U@%H[%W]# ";
CLICON_PLUGIN="controller_cli";

exit("Change to operation mode"), cli_set_mode("operation");
operation("run operational commands") @operation;

# Auto edit mode
# Autocli syntax tree operations
edit @datamodelmode, cli_auto_edit("basemodel");
up, cli_auto_up("basemodel");
top, cli_auto_top("basemodel");
set @datamodel, cli_auto_set_devs();
merge @datamodel, cli_auto_merge_devs();
delete("Delete a configuration item") {
      @datamodel, @add:leafref-no-refer, cli_auto_del_devs();
      all("Delete whole candidate configuration"), delete_all("candidate");
}
quit("Quit"), cli_quit();
show("Show a particular state of the system"), @datamodelshow, cli_show_auto_mode("candidate", "text", true, false);{
      xml, cli_show_auto_mode("candidate", "xml", false, false);{
      	   @datamodelshow, cli_show_auto_devs("candidate", "xml", false, false, "explicit");
      }
      text, cli_show_auto_mode("candidate", "text", false, false);{
           @datamodelshow, cli_show_auto_devs("candidate", "text", false, false, "explicit");
      }
      json, cli_show_auto_mode("candidate", "json", false, false);{
            @datamodelshow, cli_show_auto_devs("candidate", "json", false, false, "explicit");
      }
      cli, cli_show_auto_mode("candidate", "cli", true, false, "explicit", "set ");{
           @datamodelshow, cli_show_auto_devs("candidate", "cli", false, false, "explicit", "set ");
      }
}
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

# CLI show config top-level
# 1: cli command ie "show config"
# 2: cli mode
function show-top()
{
    cmd=$1
    mode=$2

    new "top: $cmd xml"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd xml)" 0 "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name><enabled>true</enabled><description>Clixon example container</description><user>$USER</user><conn-type>NETCONF_SSH</conn-type>" "<yang-config>VALIDATE</yang-config>" "<config><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name>" "</config></device><device><name>${IMG}2</name>"

    new "top: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd json)" 0 "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\",\"enabled\":true,\"description\":\"Clixon example container\",\"user\":\"$USER\",\"conn-type\":\"NETCONF_SSH\"" '"yang-config":"VALIDATE"' '"config":{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}'

    new "top: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd text)" 0 "clixon-controller:devices {" "device ${IMG}1 {" "enabled true;" "conn-type NETCONF_SSH;" "yang-config VALIDATE;" "config {" "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;"

    new "top: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd cli)" 0 "set devices device ${IMG}1 config interfaces interface x" "set devices device ${IMG}1 config interfaces interface y" "set devices device ${IMG}2 config interfaces interface x config name x"
}

# CLI show config sub-tree
# 1: cli command
# 2: cli mode
function show-sub()
{
    cmd=$1
    mode=$2
    
    new "sub: $cmd xml"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd xml devices device ${IMG}1 config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

    new "sub: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd json devices device ${IMG}1 config interfaces)" 0 '{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}}' --not-- "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\""

    new "sub: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd text devices device ${IMG}1 config interfaces)" 0 "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;" "interface y {" --not-- "clixon-controller:devices {" "device ${IMG}1"

    new "sub: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd cli devices device ${IMG}1 config interfaces)" 0 "set interfaces interface x config name x" "set interfaces interface y config name y" --not-- "set devices" "${IMG}1 config"
}

# CLI show config sub-tree using wildcard
# 1: cli command
# 2: cli mode
function show-sub-glob()
{
    cmd=$1
    mode=$2
    
    new "sub-glob: $cmd xml"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd xml devices device ${IMG}* config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:atm</type></config></interface></interfaces>" "${IMG}1:" "${IMG}2:" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

    new "sub-glob: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd json devices device ${IMG}* config interfaces)" 0 '{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}}' --not-- "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\""

    new "sub-glob: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd text devices device ${IMG}* config interfaces)" 0 "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;" "interface y {" --not-- "clixon-controller:devices {" "device ${IMG}1"

    new "sub-glob: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd cli devices device ${IMG}* config interfaces)" 0 "set interfaces interface x config name x" "set interfaces interface y config name y" "${IMG}1:" "${IMG}2:" --not-- "set devices" "${IMG}1 config"
}

# Tests
show-top "show config" operation 

new "Operation sub"
show-sub "show config" operation

new "Configure top"
show-top "show" configure

new "Configure sub"
show-sub "show" configure

new "Operation sub glob"
show-sub-glob "show config" operation

new "Configure sub glob"
show-sub-glob "show" configure

# Special cases
new "show state xml"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show state)" 0  "<capabilities><capability>urn:ietf:params:netconf:base:1.0</capability>" "<sync-timestamp>" "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"><module-set><name>default</name><module>" "<name>clixon-lib</name>"

new "show config xml device *"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show config xml devices device ${IMG}*)" 0 "${IMG}1:" "${IMG}2:" "<device><name>${IMG}1</name><enabled>true</enabled><description>Clixon example container</description><user>${USER}</user><conn-type>NETCONF_SSH</conn-type><yang-config>VALIDATE</yang-config>" "</config></device>" "<device><name>${IMG}2</name><enabled>true</enabled><description>Clixon example container</description><user>${USER}</user><conn-type>NETCONF_SSH</conn-type>" "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>"

new "Configure edit modes"
mode=configure
cmd=show

new "show config xml"
expectpart "$($clixon_cli -1 -m $mode -f $CFG -E $CFD $cmd xml devices device ${IMG}1 config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

cat <<EOF > $fin
edit devices device ${IMG}1
show xml
EOF
new "edit device; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD -m $mode 2>&1)" 0 "<name>${IMG}1</name><enabled>true</enabled><description>Clixon example container</description><user>${USER}</user><conn-type>NETCONF_SSH</conn-type><yang-config>VALIDATE</yang-config>" "<config><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>"

cat <<EOF > $fin
edit devices device ${IMG}1 config
show xml
EOF
new "edit device config; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD -m $mode 2>&1)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<name>${IMG}1</name>"

cat <<EOF > $fin
edit devices device ${IMG}1 config interfaces
show xml
EOF
new "edit device config interfaces; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD -m $mode 2>&1)" 0 "<interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<name>${IMG}1</name>" "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\">"

if false; then # XXX DOES NOT WORK
cat <<EOF > $fin
edit devices device ${IMG}1 config table parameter x
show xml
EOF
new "edit device config table x; show xml"
#expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD -m $mode 2>&1)" 0 "<name>x</name><value>11</value>" --not-- "<name>${IMG}1</name>" "<table xmlns=\"urn:example:clixon\">" "<parameter>"

cat <<EOF > $fin
edit devices device ${IMG}1 config table
up
show xml
EOF
new "edit device config table; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD -m $mode 2>&1)" 0 "<parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter>" --not-- "<name>${IMG}1</name>" "<table xmlns=\"urn:example:clixon\">"
fi

# See https://github.com/clicon/clixon-controller/issues/145
cat <<EOF > $fin
show configuration xml devices device openconfig1 config interfaces interface lo0 subinterfaces
show configuration xml devices device openconfig2 config interfaces interface lo0 subinterfaces
EOF
new "Deep uses/grouping on two devices"
expectpart "$(cat $fin | $clixon_cli -f $CFG -E $CFD 2>&1)" 0 --not-- "CLI syntax error:" "Unknown command"

new "Test show detail"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show detail devices device \* config system config hostname)" 0 "Symbol:     hostname
Module:     openconfig-system
File:       /usr/local/share/controller/mounts/default/openconfig-system@2024-09-24.yang
Namespace:  http://openconfig.net/yang/system
Prefix:     oc-sys
XPath:      /ctrl:devices/ctrl:device[ctrl:name='*']/ctrl:config/oc-sys:system/oc-sys:config/oc-sys:hostname
APIpath:    /clixon-controller:devices/device=%2A/config/openconfig-system:system/config/hostname"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
