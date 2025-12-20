# Controller test script for cli set/delete multiple, using glob '*'

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

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
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
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
      @datamodel, @add:leafref-no-refer, cli_auto_del_devs();
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


cat<<EOF > $dir/controller_operation.cli
CLICON_MODE="operation";
CLICON_PROMPT="%U@%H> ";
CLICON_PLUGIN="controller_cli";

configure("Change to configure mode"), cli_set_mode("configure");
exit("Quit"), cli_quit();
quit("Quit"), cli_quit();
show("Show a particular state of the system"){
    connections("Show state of connection to devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_show_connections();
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

    new "Start new backend -s init -f $CFG -E $CFD"
    start_backend -s init -f $CFG -E $CFD

fi

new "Wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

new "verify no z"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD show cli devices device ${IMG}* config interfaces interface z)" 0 "${IMG}1:" "${IMG}2:" --not-- "set interface interface z"

new "set * z"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device ${IMG}* config interfaces interface z config type usb)" 0 "^$"
#expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device ${IMG}* config interfaces interface z config type ianaift:usb)" 0 "^$" # XXX namespace does not work

new "verify z usb"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD show cli devices device ${IMG}* config interfaces interface z)" 0 "${IMG}1:" "${IMG}2:" "set interface z config type usb"

new "set *1 z atm"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device ${IMG}1 config interfaces interface z config type atm)" 0 "^$"

for i in $(seq 2 $nr); do
    new "set *$i z eth"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD set devices device ${IMG}$i config interfaces interface z config type eth)" 0 "^$"
done

new "verify z atm + eth"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD show cli devices device ${IMG}* config interfaces interface z)" 0 "set interface z config type atm" "set interface z config type eth" --not-- "set interface z config type usb"

new "del * z"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD del devices device ${IMG}* config interfaces interface z)" 0 "^$"

new "verify no z"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD show cli devices device ${IMG}* config interface interface z)" 0 "${IMG}1:" "${IMG}2:" --not-- "set interface z"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
