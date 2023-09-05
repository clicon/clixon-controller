# Controller test script using cli, pyapi, backend and two example devices
# 0. Install package with py and yang file
# 1a. edit a service with (local) syntax error
# 1b. edit a service: set x=1.2.3.4
# 2. Commit (push) service
# 3. Check device config on controller and devices

set -u

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

exit 0 # Openconfig NYI

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
fi
CFG=$dir/controller.xml
INSTALLDIR=$dir/install
PYINSTALLDIR=${INSTALLDIR}/usr/local/share/clixon/controller/modules
YANGINSTALLDIR=${INSTALLDIR}/usr/local/share/clixon/controller/yang

pydir=$dir/py
if [ ! -d $pydir ]; then
    mkdir $pydir
fi
# Place yang in pydir to co-locate with python for install script
fyang=$pydir/clixon-test@2023-03-22.yang

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>/usr/local/etc/controller.xml</CLICON_CONFIGFILE>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>${YANGINSTALLDIR}</CLICON_YANG_MAIN_DIR>
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
  <CLICON_BACKEND_USER>clicon</CLICON_BACKEND_USER>
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">/usr/local/bin/clixon_server.py -f $CFG -F -d</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">${PYINSTALLDIR}</CONTROLLER_PYAPI_MODULE_PATH>
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
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-test {
    namespace "http://clicon.org/test";
    prefix test;
    import iana-if-type {
        prefix ianaift;
    }
    import ietf-interfaces { prefix ietf-if; }
    import clixon-controller { prefix ctrl; }
    revision 2023-03-22{
	description "Initial prototype";
    }
    augment "/ctrl:services" {
	list test {
	    key service-name;
	    leaf service-name {
		type string;
	    }
	    description "Test service";
	    list interface {
		key name;
		leaf name{
		    type string;
		}
		leaf type{
                  type identityref {
                     base ietf-if:interface-type;
                  }
		}
	    }
	}
    }
}
EOF

cat <<EOF > $pydir/clixon_example.py
from clixon.clixon import rpc

SERVICE = "test"

@rpc()
def setup(root, log, **kwargs):
    for device in root.devices.device:
        for service in root.services.test:
            interface = service.interface
            device.config.add("interfaces")
            device.config.interfaces.add(interface)

if __name__ == "__main__":
    setup()
EOF

cat<<EOF > $dir/controller_operation.cli
CLICON_MODE="operation";
CLICON_PROMPT="%U@%H> ";
CLICON_PLUGIN="controller_cli";
CLICON_PIPETREE="|controller_pipe";

pull("Pull config from one or multiple devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_rpc_pull("replace");{
                    replace, cli_rpc_pull("replace");
                    merge, cli_rpc_pull("merge");
}
show("Show a particular state of the system"){
    configuration("Show configuration"), cli_show_auto_mode("running", "text", true, false);{
      xml, cli_show_auto_mode("running", "xml", true, false);{
           @datamodelshow, cli_show_auto_devs("running", "xml", true, false, "report-all");
      }
      cli, cli_show_auto_mode("running", "cli", true, false, "report-all", "set ");{
           @datamodelshow, cli_show_auto_devs("running", "cli", true, false, "report-all", "s
et ");
      }
    }
}
EOF

cat<<EOF > $dir/controller_configure.cli
CLICON_MODE="configure";
CLICON_PROMPT="%U@%H[%W]# ";
CLICON_PLUGIN="controller_cli";
CLICON_PIPETREE="|controller_pipe";

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
commit("Commit the changes, trigger services scripts"), cli_rpc_controller_commit("candidate", "CHANGE", "COMMIT");
show("Show a particular state of the system"), @datamodelshow, cli_show_auto_mode("candidate", "xml", true, false);{
    @datamodelshow, cli_show_auto_devs("candidate", "xml", false, false);
    # old syntax, but left here because | display cli  does not work properly
    cli, cli_show_auto_mode("candidate", "cli", true, false, "report-all", "set ");{
         @datamodelshow, cli_show_auto_devs("candidate", "cli", false, false, "report-all", "set ");
    }
}
EOF

cat<<EOF > $dir/controller_pipe.cli
CLICON_MODE="|controller_pipe";

\| { 
   grep <arg:string>, pipe_grep_fn("-e", "arg");
   except <arg:string>, pipe_grep_fn("-v", "arg");
   tail, pipe_tail_fn();
   count, pipe_wc_fn("-l");
   display {
     xml, pipe_showas_fn("xml", false);
     curly, pipe_showas_fn("text", true);
     json, pipe_showas_fn("json", false);
     cli, pipe_showas_fn("cli", true, "set ");
   }
}
EOF

new "Install YANG and Python modules"
rm -f ${PYINSTALLDIR}/*
rm -f ${YANGINSTALLDIR}/*
sudo clixon_controller_packages.sh -s $pydir -m ${PYINSTALLDIR} -y ${YANGINSTALLDIR}
if [ ! -f ${PYINSTALLDIR}/clixon_example.py ]; then
    err1 " ${PYINSTALLDIR}/clixon_example.py not found"
fi
if [ ! -f ${YANGINSTALLDIR}/clixon-test@2023-03-22.yang ]; then
    err1 " ${YANGINSTALLDIR}/clixon-test@2023-03-22.yang not found"
fi

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    # Make sure the Python server is dead
    if [ -f "/tmp/clixon_server.pid" ]; then
	pid=`cat /tmp/clixon_server.pid`
	kill -9 $pid
    fi

    new "Start new backend -s init  -f $CFG -D $DBG"
    sudo clixon_backend -s init -f $CFG -D $DBG
fi

# Check backend is running
wait_backend

# Reset controller
new "reset controller"
. ./reset-controller.sh

# Tests
new "CLI: syncronize devices"
expectpart "$($clixon_cli -1 -f $CFG pull)" 0 ""

#new "CLI: Check pre-controller devices configuration"
#expectpart "$($clixon_cli -1 -f $CFG show configuration xml devices)" 0 "<name>y</name>" --not-- "<value>1.2.3.4</value>"

new "CLI: Configure service"
expectpart "$($clixon_cli -1 -f $CFG -m configure set services test cli_test)" 0 ""

# XXX move to error tests
#new "CLI: Set invalid type"
#expectpart "$($clixon_cli -1 -f $CFG -l o -m configure set services test cli_test interface x type y)" 255 "CLI syntax error: \"set services test cli_test interface x type y\": \"y\" is invalid input for cli command: value"

new "CLI: Set valid value type"
expectpart "$($clixon_cli -1 -f $CFG -m configure set services test cli_test interface x type ianaift:atm)" 0 ""

sleep 2

new "CLI: Commit"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit)" 0 ""

new "CLI: Check controller services configuration"
expectpart "$($clixon_cli -1 -f $CFG show configuration cli)" 0 "^set services test cli_test" "^set services test cli_test parameter x" "^set services test cli_test parameter x value 1.2.3.4"

new "CLI: Check controller devices configuration"
expectpart "$($clixon_cli -1 -f $CFG show configuration xml)" 0 "<name>y</name>" "<value>1.2.3.4</value>"

new "Verify containers"

i=1;

for ip in $CONTAINERS; do
    NAME=$IMG$i
    ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>$NAME</name>
	  <config>
	    <table xmlns="urn:example:clixon">
	      <parameter>
		 <name>x</name>
	      </parameter>
	    </table>
	  </config>
	</device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )

    i=$((i+1))

    echo "ret:$ret"
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	echo "netconf rpc-error detected"
	exit 1
    fi
    match=$(echo $ret | grep --null -Eo '<config><table xmlns="urn:example:clixon"><parameter><name>x</name><value>1.2.3.4</value></parameter></table></config>') || true
    if [ -z "$match" ]; then
	echo "netconf rpc get-config failed"
	exit 1
    fi
done

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

echo "test-cli-edit-commit-push"
echo OK
