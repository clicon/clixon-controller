# NACM for controller CLI
# XXX show config does not work in mountpoint
# See https://github.com/clicon/clixon/issues/468

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
test -d $dir || mkdir -p $dir
mkdir -p $dir
CFD=$dir/conf.d
test -d $CFD || mkdir -p $CFD
fyang=$dir/clixon-test@2023-03-22.yang

# Common NACM scripts
. ./nacm.sh

# Specialize controller.xml
cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_CLISPEC_DIR>$dir</CLICON_CLISPEC_DIR>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_NACM_CREDENTIALS>none</CLICON_NACM_CREDENTIALS>
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
     <rule>
       <name>exclude ietf interfaces</name>
       <module-name>ietf-interfaces</module-name>
       <operation>disable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-test {
    namespace "http://clicon.org/test";
    prefix test;
    import clixon-controller {
       prefix ctrl; 
    }
    import ietf-netconf-acm {
       prefix nacm;
    } 
    revision 2023-03-22{
        description "Initial prototype";
    }
}
EOF

# The groups are slightly modified from RFC8341 A.1 ($USER added in admin group)
RULES=$(cat <<EOF
   <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
     <enable-nacm>true</enable-nacm>
     <read-default>deny</read-default>
     <write-default>deny</write-default>
     <exec-default>permit</exec-default>

     $NGROUPS

     $NADMIN

     <rule-list>
       <name>limited permit</name>
       <group>limited</group>
       <rule>
         <name>permit interface x</name>
         <path xmlns:ctrl="http://clicon.org/controller" xmlns:oc-if="http://openconfig.net/yang/interfaces">
            /ctrl:devices/ctrl:device/ctrl:config/oc-if:interfaces/oc-if:interface[oc-if:name="x"]
         </path>
         <access-operations>*</access-operations>
.         <action>permit</action>
         <comment>
           Allow the 'limited' group full access to interface x.
         </comment>
       </rule>
       <rule>
         <name>permit exec</name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
         <comment>
             Allow invocation of the supported server operations.
         </comment>
       </rule>
     </rule-list>
   </nacm>
EOF
)

#            /ctrl:devices/ctrl:device/ctrl:config/oc-if:interfaces/oc-if:interface[oc-if:name="x"]

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
        @datamodelshow, cli_show_auto_devs("running", "xml", false, false, "explicit");
    }
    connections("Show state of connection state of devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_show_connections();{
                 check("Check if device is in sync"), check_device_db("default");
                 diff("Compare remote device config with local"), compare_device_db_dev("default");
                 detail("Show detailed state"), cli_show_connections("detail");
             }
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
commit, cli_commit();{
    local("Local commit, do not push to devices"), cli_commit();
}
quit("Quit"), cli_quit();
load <operation:string choice:replace|merge|create>("Write operation on candidate") <format:string choice:xml|json>("File format") [filename <filename:string>("Filename (local filename)")], cli_auto_load_devs(); 
show("Show a particular state of the system"), @datamodelshow, cli_show_auto_mode("candidate", "text", true, false);{
      @datamodelshow, cli_show_auto_devs("candidate", "xml", false, false, "explicit");
    compare("Compare candidate and running databases"), compare_dbs_rpc("running", "candidate", "xml");

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

new "auth set authentication config and enable"
ret=$(${clixon_netconf} -0 -f $CFG -E $CFD <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>merge</default-operation>
    <config>
       ${RULES}
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)
#echo "ret:$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err1 "OK reply"
fi

new "commit 1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD -m configure commit)" 0 ""

NACMUSER=andy
new "cli show conf as admin"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD show conf devices device openconfig1 config interfaces)" 0 "<interface><name>x</name>" "<interface><name>y</name>"

new "cli set as admin"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD -m configure set devices device openconfig1 config interfaces interface x config mtu 1500)" 0 "^$"

new "commit 2"
expectpart "$($clixon_cli -1f $CFG -E $CFD -m configure commit 2>&1)" 0 "^$"

new "cli show check mtu"
expectpart "$($clixon_cli -1 -U andy -f $CFG -E $CFD show conf devices device openconfig1 config interfaces)" 0 "<interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type><mtu>1500</mtu>"

# XXX show config does not work in mountpoint
NACMUSER=wilma
new "cli show conf as limited"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD show conf devices device openconfig1 config)" 0 "<interface><name>x</name>" --not-- "<interface><name>y</name>"
# expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD show conf devices device openconfig1 config interfaces)" 0 "<interface><name>x</name>" --not-- "<interface><name>y</name>"

if false; then
new "cli set as limited"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD -m configure set devices device openconfig1 config interfaces interface x config mtu 1600)" 0 "^$"
else # XXX workaround with load
    new "workaround using xml instead of cli set as limited"
    ret=$(${clixon_cli} -1 -U $NACMUSER -f $CFG -E $CFD -m configure load merge xml 2>&1 <<'EOF'
      <config>
         <devices xmlns="http://clicon.org/controller">
           <device>
             <name>openconfig1</name>
             <config>
               <interfaces xmlns="http://openconfig.net/yang/interfaces">
                 <interface>
                   <name>x</name>
                   <config>
                     <mtu>1600</mtu>
                   </config>
                 </interface>
               </interfaces>
             </config>
           </device>
         </devices>
     </config>
EOF
)
#echo "ret:$ret"
    
if [ -n "$ret" ]; then
    err1 "$ret"
fi
fi # workaround

new "commit"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD -m configure commit local 2>&1)" 0 "^$"

new "cli show check mtu"
expectpart "$($clixon_cli -1 -U andy -f $CFG -E $CFD show conf devices device openconfig1 config interfaces)" 0 "<interface><name>x</name><config><name>x</name><type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type><mtu>1600</mtu>"

NACMUSER=guest
new "cli show conf as guest"
expectpart "$($clixon_cli -1 -U $NACMUSER -f $CFG -E $CFD show conf devices device openconfig1 config)" 0 --not-- "<interface><name>x</name>" "<interface><name>y</name>"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
