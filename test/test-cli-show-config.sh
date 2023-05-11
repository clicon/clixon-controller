# Controller test script for cli show commands

set -u

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
fi
CFG=$dir/controller.xml
fin=$dir/in

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
     <module-default>true</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include example</name>
       <module-name>clixon-example</module-name>
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
      	   @datamodelshow, cli_show_auto_devs("running", "xml", false, false, "report-all");
      }
      text, cli_show_auto_mode("running", "text", false, false);{
           @datamodelshow, cli_show_auto_devs("running", "text", false, false, "report-all");
      }
      json, cli_show_auto_mode("running", "json", false, false);{
            @datamodelshow, cli_show_auto_devs("running", "json", false, false, "report-all");
      }
      cli, cli_show_auto_mode("running", "cli", false, false, "report-all", "set ");{
           @datamodelshow, cli_show_auto_devs("running", "cli", false, false, "report-all", "set ");
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
set @datamodel, cli_auto_set();
merge @datamodel, cli_auto_merge();
create @datamodel, cli_auto_create();
delete("Delete a configuration item") {
      @datamodel, cli_auto_del(); 
      all("Delete whole candidate configuration"), delete_all("candidate");
}
quit("Quit"), cli_quit();
delete("Delete a configuration item") {
      @datamodel, cli_auto_del(); 
      all("Delete whole candidate configuration"), delete_all("candidate");
}
show("Show a particular state of the system"), @datamodelshow, cli_show_auto_mode("candidate", "text", true, false);{
      xml, cli_show_auto_mode("candidate", "xml", false, false);{
      	   @datamodelshow, cli_show_auto_devs("candidate", "xml", false, false, "report-all");
      }
      text, cli_show_auto_mode("candidate", "text", false, false);{
           @datamodelshow, cli_show_auto_devs("candidate", "text", false, false, "report-all");
      }
      json, cli_show_auto_mode("candidate", "json", false, false);{
            @datamodelshow, cli_show_auto_devs("candidate", "json", false, false, "report-all");
      }
      cli, cli_show_auto_mode("candidate", "cli", true, false, "report-all", "set ");{
           @datamodelshow, cli_show_auto_devs("candidate", "cli", false, false, "report-all", "set ");
      }
}
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    echo "Start new backend -s init  -f $CFG -D $DBG"
    sudo clixon_backend -s init -f $CFG -D $DBG
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

    new "$cmd xml"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd xml)" 0 "<devices xmlns=\"http://clicon.org/controller\"><device><name>clixon-example1</name><description>Clixon example container</description><enabled>true</enabled><conn-type>NETCONF_SSH</conn-type><user>root</user>" "<yang-config>VALIDATE</yang-config><config><table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table></config></device><device><name>clixon-example2</name>"

    new "$cmd json"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd json)" 0 '{"clixon-controller:devices":{"device":\[{"name":"clixon-example1","description":"Clixon example container","enabled":true,"conn-type":"NETCONF_SSH","user":"root"' '"yang-config":"VALIDATE","config":{"clixon-example:table":{"parameter":\[{"name":"x","value":"11"},{"name":"y","value":"22"}]}}'

    # XXX only pretty for text
    new "$cmd text"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd text)" 0 "clixon-controller:devices {" "device clixon-example1 {" "enabled true;" "conn-type NETCONF_SSH;" "yang-config VALIDATE;" "config {" "clixon-example:table {" "parameter x {" "value 11;" "parameter y {" "value 22;"

    new "$cmd cli"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd cli)" 0 "set devices device clixon-example1 config table parameter x value 11" "set devices device clixon-example1 config table parameter y value 22" "set devices device clixon-example2 config table parameter x value 11" "set devices device clixon-example2 config table parameter y value 22"
    
}

# CLI show config sub-tree
# 1: cli command
# 2: cli mode
function show-sub()
{
    cmd=$1
    mode=$2
    
    new "$cmd xml"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd xml devices device clixon-example1 config table )" 0 "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>clixon-example1</name>"

    new "$cmd json"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd json devices device clixon-example1 config table)" 0 '{"clixon-example:table":{"parameter":\[{"name":"x","value":"11"},{"name":"y","value":"22"}]}}' --not-- '{"clixon-controller:devices":{"device":\[{"name":"clixon-example1"'

    new "$cmd text"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd text devices device clixon-example1 config table)" 0 "clixon-example:table {" "parameter x {" "value 11;" "parameter y {" "value 22;" --not-- "clixon-controller:devices {" "device clixon-example1"

    new "$cmd cli"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd cli devices device clixon-example1 config table)" 0  "set table parameter x value 11" "set table parameter y value 22" --not-- "set devices" "clixon-example1 config"
}

# CLI show config sub-tree using wildcard
# 1: cli command
# 2: cli mode
function show-sub-glob()
{
    cmd=$1
    mode=$2
    
    new "$cmd xml"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd xml devices device clixon* config table )" 0 "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" "clixon-example1:" "clixon-example2:" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>clixon-example1</name>"

    new "$cmd json"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd json devices device clixon* config table)" 0 '{"clixon-example:table":{"parameter":\[{"name":"x","value":"11"},{"name":"y","value":"22"}]}}'  "clixon-example1:" "clixon-example2:" --not-- '{"clixon-controller:devices":{"device":\[{"name":"clixon-example1"'

    new "$cmd text"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd text devices device clixon* config table)" 0 "clixon-example:table {" "parameter x {" "value 11;" "parameter y {" "value 22;"  "clixon-example1:" "clixon-example2:" --not-- "clixon-controller:devices {" "device clixon-example1"

    new "$cmd cli"
    expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd cli devices device clixon* config table)" 0  "set table parameter x value 11" "set table parameter y value 22"  "clixon-example1:" "clixon-example2:" --not-- "set devices" "clixon-example1 config"
}

# Tests
new "syncronize devices"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG pull)" 0 ""

new "Operation top"
show-top "show config" operation 

new "Operation sub"
show-sub "show config" operation

new "show state xml"
expectpart "$(${PREFIX} $clixon_cli -1 -f $CFG show state)" 0  "<capabilities><capability>urn:ietf:params:netconf:base:1.0</capability>" "<sync-timestamp>" "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"><module-set><name>mount</name><module><name>clixon-autocli</name>"

new "Configure top"
show-top "show" configure

new "Configure sub"
show-sub "show" configure

new "Operation sub glob"
show-sub-glob "show config" operation

new "Configure sub glob"
show-sub-glob "show" configure

new "show config xml * table"
expectpart "$(${PREFIX} $clixon_cli -1 -m operation -f $CFG show config xml devices device clixon* config table)" 0 "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" --not-- "<devices xmlns=\"http://clicon.org/controller\">"

new "Configure edit modes"
mode=configure
cmd=show
new "show config xml"
expectpart "$(${PREFIX} $clixon_cli -1 -m $mode -f $CFG $cmd xml devices device clixon-example1 config table )" 0 "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" --not-- "<devices xmlns=\"http://clicon.org/controller\"><device><name>clixon-example1</name>"

cat <<EOF > $fin
edit devices device clixon-example1
show xml
EOF
new "edit device; show xml"
expectpart "$(cat $fin | ${PREFIX} $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<name>clixon-example1</name><description>Clixon example container</description><enabled>true</enabled><conn-type>NETCONF_SSH</conn-type><user>root</user>" "<yang-config>VALIDATE</yang-config><config><table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table></config>"

cat <<EOF > $fin
edit devices device clixon-example1 config
show xml
EOF
new "edit device config; show xml"
expectpart "$(cat $fin | ${PREFIX} $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<table xmlns=\"urn:example:clixon\"><parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter></table>" --not-- "<name>clixon-example1</name>"

cat <<EOF > $fin
edit devices device clixon-example1 config table
show xml
EOF
new "edit device config table; show xml"
expectpart "$(cat $fin | ${PREFIX} $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter>" --not-- "<name>clixon-example1</name>" "<table xmlns=\"urn:example:clixon\">"

if false; then # XXX DOES NOT WORK
cat <<EOF > $fin
edit devices device clixon-example1 config table parameter x
show xml
EOF
new "edit device config table x; show xml"
#expectpart "$(cat $fin | ${PREFIX} $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<name>x</name><value>11</value>" --not-- "<name>clixon-example1</name>" "<table xmlns=\"urn:example:clixon\">" "<parameter>"

cat <<EOF > $fin
edit devices device clixon-example1 config table
up
show xml
EOF
new "edit device config table; show xml"
expectpart "$(cat $fin | ${PREFIX} $clixon_cli -f $CFG -m $mode 2>&1)" 0 "<parameter><name>x</name><value>11</value></parameter><parameter><name>y</name><value>22</value></parameter>" --not-- "<name>clixon-example1</name>" "<table xmlns=\"urn:example:clixon\">"
fi

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi


echo OK
