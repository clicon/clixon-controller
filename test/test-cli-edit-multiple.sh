# Controller test script for cli set/delete multiple, using glob '*'

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
else
    rm -rf $dir/*
fi
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $CFD || mkdir -p $CFD
fin=$dir/in

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>/usr/local/share/clixon/controller/main</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>configure</CLICON_CLI_MODE>
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
</clixon-config>
EOF

cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
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
       <module-name>openconfig*</module-name>
       <operation>enable</operation>
     </rule>
  </autocli>
</clixon-config>
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

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    echo "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG -D $DBG"
    start_backend -s init -f $CFG

fi

# Check backend is running
wait_backend

# Reset controller
. ./reset-controller.sh

new "verify no z"
expectpart "$($clixon_cli -1 -m configure -f $CFG show cli devices device ${IMG}* config interfaces interface z)" 0 "${IMG}1:" "${IMG}2:" --not-- "set interface interface z"

new "set * z"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device ${IMG}* config interfaces interface z config type usb)" 0 "^$"
#expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device ${IMG}* config interfaces interface z config type ianaift:usb)" 0 "^$" # XXX namespace does not work

new "verify z usb"
expectpart "$($clixon_cli -1 -m configure -f $CFG show cli devices device ${IMG}* config interfaces interface z)" 0 "${IMG}1:" "${IMG}2:" "set interface z config type usb"

new "set *1 z atm"
expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device ${IMG}1 config interfaces interface z config type atm)" 0 "^$"

for i in $(seq 2 $nr); do
    new "set *$i z eth"
    expectpart "$($clixon_cli -1 -m configure -f $CFG set devices device ${IMG}$i config interfaces interface z config type eth)" 0 "^$"
done

new "verify z atm + eth"
expectpart "$($clixon_cli -1 -m configure -f $CFG show cli devices device ${IMG}* config interfaces interface z)" 0 "set interface z config type atm" "set interface z config type eth" --not-- "set interface z config type usb"

new "del * z"
expectpart "$($clixon_cli -1 -m configure -f $CFG del devices device ${IMG}* config interfaces interface z)" 0 "^$"

new "verify no z"
expectpart "$($clixon_cli -1 -m configure -f $CFG show cli devices device ${IMG}* config interface interface z)" 0 "${IMG}1:" "${IMG}2:" --not-- "set interface z"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
