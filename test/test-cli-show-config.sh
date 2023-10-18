# Controller test script for cli show commands

set -u

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
else
    rm -rf $dir/*
fi
CFG=$dir/controller.xml
fin=$dir/in

# source IMG/USER etc
. ./site.sh

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>/usr/local/etc/controller.xml</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>/usr/local/share/clixon/controller</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>$dir</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>/usr/local/var/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/controller.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/controller</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_SOCK_GROUP>clicon</CLICON_SOCK_GROUP>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>www-data</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>/usr/local/sbin</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
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
       <name>include example</name>
       <module-name>openconfig-interfaces</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include example</name>
       <module-name>openconfig-system</module-name>
       <operation>enable</operation>
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
    devices("Show state of devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_show_devices();{
                 check("Check if device is in sync"), check_device_db("text");
                 detail("Show detailed state"), cli_show_devices("detail");
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
      @datamodel, cli_auto_del_devs(); 
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

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

# Check backend is running
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
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd xml)" 0 "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name><description>Clixon example container</description><enabled>true</enabled><user>$USER</user><conn-type>NETCONF_SSH</conn-type>" "<yang-config>VALIDATE</yang-config>" "<config><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name>" "</config></device><device><name>${IMG}2</name>"

    new "top: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd json)" 0 "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\",\"description\":\"Clixon example container\",\"enabled\":true,\"user\":\"$USER\",\"conn-type\":\"NETCONF_SSH\"" '"yang-config":"VALIDATE"' '"config":{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}'

    new "top: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd text)" 0 "clixon-controller:devices {" "device ${IMG}1 {" "enabled true;" "conn-type NETCONF_SSH;" "yang-config VALIDATE;" "config {" "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;"

    new "top: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd cli)" 0 "set devices device ${IMG}1 config interfaces interface x" "set devices device ${IMG}1 config interfaces interface y" "set devices device ${IMG}2 config interfaces interface x config name x"
}

# CLI show config sub-tree
# 1: cli command
# 2: cli mode
function show-sub()
{
    cmd=$1
    mode=$2
    
    new "sub: $cmd xml"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd xml devices device ${IMG}1 config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

    new "sub: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd json devices device ${IMG}1 config interfaces)" 0 '{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}}' --not-- "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\""

    new "sub: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd text devices device ${IMG}1 config interfaces)" 0 "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;" "interface y {" --not-- "clixon-controller:devices {" "device ${IMG}1"

    new "sub: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd cli devices device ${IMG}1 config interfaces)" 0 "set interfaces interface x config name x" "set interfaces interface y config name y" --not-- "set devices" "${IMG}1 config"
}

# CLI show config sub-tree using wildcard
# 1: cli command
# 2: cli mode
function show-sub-glob()
{
    cmd=$1
    mode=$2
    
    new "sub-glob: $cmd xml"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd xml devices device ${IMG}* config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:atm</type></config></interface></interfaces>" "${IMG}1:" "${IMG}2:" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

    new "sub-glob: $cmd json"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd json devices device ${IMG}* config interfaces)" 0 '{"openconfig-interfaces:interfaces":{"interface":\[{"name":"x","config":{"name":"x","type":"iana-if-type:ethernetCsmacd"}},{"name":"y","config":{"name":"y","type":"iana-if-type:atm"}}\]}}' --not-- "{\"clixon-controller:devices\":{\"device\":\[{\"name\":\"${IMG}1\""

    new "sub-glob: $cmd text"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd text devices device ${IMG}* config interfaces)" 0 "openconfig-interfaces:interfaces {" "interface x {" "name x;" "type ianaift:ethernetCsmacd;" "interface y {" --not-- "clixon-controller:devices {" "device ${IMG}1"

    new "sub-glob: $cmd cli"
    expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd cli devices device ${IMG}* config interfaces)" 0 "set interfaces interface x config name x" "set interfaces interface y config name y" "${IMG}1:" "${IMG}2:" --not-- "set devices" "${IMG}1 config"
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
expectpart "$($clixon_cli -1 -f $CFG show state)" 0  "<capabilities><capability>urn:ietf:params:netconf:base:1.0</capability>" "<sync-timestamp>" "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"><module-set><name>mount</name><module>" "<name>clixon-lib</name>"

new "show config xml device *"
expectpart "$($clixon_cli -1 -f $CFG show config xml devices device ${IMG}*)" 0 "${IMG}1:" "${IMG}2:" "<device><name>${IMG}1</name><description>Clixon example container</description><enabled>true</enabled><user>${USER}</user><conn-type>NETCONF_SSH</conn-type><yang-config>VALIDATE</yang-config>" "</config></device>" "<device><name>${IMG}2</name><description>Clixon example container</description><enabled>true</enabled><user>${USER}</user><conn-type>NETCONF_SSH</conn-type>" "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>"

new "Configure edit modes"
mode=configure
cmd=show

new "show config xml"
expectpart "$($clixon_cli -1 -m $mode -f $CFG $cmd xml devices device ${IMG}1 config interfaces)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>${IMG}1</name>"

cat <<EOF > $fin
edit devices device ${IMG}1
show xml
EOF
new "edit device; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<name>${IMG}1</name><description>Clixon example container</description><enabled>true</enabled><user>${USER}</user><conn-type>NETCONF_SSH</conn-type><yang-config>VALIDATE</yang-config>" "<config><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>"

cat <<EOF > $fin
edit devices device ${IMG}1 config
show xml
EOF
new "edit device config; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<name>${IMG}1</name>"

cat <<EOF > $fin
edit devices device ${IMG}1 config interfaces
show xml
EOF
new "edit device config interfaces; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type></config></interface>" --not-- "<name>${IMG}1</name>" "<interfaces xmlns=\"http://openconfig.net/yang/interfaces\">"

if false; then # XXX DOES NOT WORK
cat <<EOF > $fin
edit devices device ${IMG}1 config table parameter x
show xml
EOF
new "edit device config table x; show xml"
#expectpart "$(cat $fin | $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<name>x</name><value>11</value>" --not-- "<name>${IMG}1</name>" "<table xmlns=\"urn:example:clixon\">" "<parameter>"

cat <<EOF > $fin
edit devices device ${IMG}1 config table
up
show xml
EOF
new "edit device config table; show xml"
expectpart "$(cat $fin | $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter>" --not-- "<name>${IMG}1</name>" "<table xmlns=\"urn:example:clixon\">"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
