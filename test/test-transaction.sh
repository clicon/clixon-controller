# Controller transaction tests focused on UX behavior across device states.
# Based on table.xlsx which defines expected behavior for:
#   3 device states (columns) x 6 operations (rows)
#
# One devices:
# - NAME2: test object - transitions between states
#
# Device states:
#   State 1 DISABLED: enabled=false, conn-state=CLOSED  (administratively disabled)
#   State 2 CLOSED: enabled=true,  conn-state=CLOSED
#   State 3 OPEN: enabled=true,  conn-state=OPEN
#
# Operations:
#   A: Connect           connection open
#   B: Disconnect        connection close
#   C: Pull              pull)
#   D: Show devices diff show devices <name> diff
#   E: Commit push/diff  commit push/diff in configure mode, with no edits
#   F: Commit push       commit push in configure mode, with prior local edit
#   G: Commit diff       commit diff in configure mode, with prior localedit

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr >= 2"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

ip2=$(echo $CONTAINERS | awk '{print $2}')
NAME2="${IMG}2"

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
modules=$dir/modules
fyang=$dir/transtest.yang
pycode=$modules/transtest.py
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
test -d $modules || mkdir -p $modules

cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
</clixon-config>
EOF

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_server.py -f $CFG</CONTROLLER_ACTION_COMMAND>
  <CONTROLLER_PYAPI_MODULE_PATH xmlns="http://clicon.org/controller-config">$modules</CONTROLLER_PYAPI_MODULE_PATH>
  <CONTROLLER_PYAPI_MODULE_FILTER xmlns="http://clicon.org/controller-config"></CONTROLLER_PYAPI_MODULE_FILTER>
  <CONTROLLER_PYAPI_PIDFILE xmlns="http://clicon.org/controller-config">/tmp/clixon_pyapi_transtest.pid</CONTROLLER_PYAPI_PIDFILE>
</clixon-config>
EOF

# YANG service module: one-leaf service that writes login-banner to a target device
cat <<EOF > $fyang
module transtest {
    namespace "http://clicon.org/transtest";
    prefix tt;
    import clixon-controller {
        prefix ctrl;
    }
    revision 2026-06-05 {
        description "Transaction test service";
    }
    augment "/ctrl:services" {
        list transtest {
            description "Transaction test service";
            key service-name;
            leaf service-name {
                type string;
            }
            leaf device {
                type string;
                description "Target device name";
            }
            leaf value {
                type string;
                description "Value to set as login-banner; empty means no edit";
            }
            uses ctrl:created-by-service;
        }
    }
}
EOF

# Python service: if value is set, write login-banner to the named device
cat <<EOF > $pycode
SERVICE = "transtest"

def setup(root, log, **kwargs):
    try:
        _ = root.services
    except Exception:
        return
    for instance in root.services.transtest:
        if instance.service_name != kwargs["instance"]:
            continue
        try:
            target = instance.device.get_data()
        except Exception:
            return
        try:
            value = instance.value.get_data()
        except Exception:
            return
        if not value:
            return
        for device in root.devices.device:
            if device.name.get_data() == target:
                device.config.system.config.create("login-banner", data=value)
EOF
cp ../src/autocli.xml $CFD/

# Configure device from scratch (nc:operation=replace).
# Args: name ip port enabled
function init_device(){
    local name=$1
    local ip=$2
    local enabled=${3:-true}
    local ret
    new "init_device $name: edit-config"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller">
        <device nc:operation="replace">
          <name>$name</name>
          <enabled>$enabled</enabled>
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
    if [ $? -ne 0 ]; then err1 "init_device edit-config" "$ret"; fi
    new "init_device $name: commit"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
    )
    if [ $? -ne 0 ]; then err1 "init_device commit" "$ret"; fi
}

# Update device meta-settings without replacing existing device config.
# Args: name ip port enabled
# Port=1 for unreachable
function update_device(){
    local name=$1
    local ip=$2
    local port=${3:-830}
    local enabled=${4:-true}
    local ret
    new "update_device $name: enabled:$enabled"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>merge</default-operation>
    <config>
      <devices xmlns="http://clicon.org/controller">
        <device>
          <name>$name</name>
          <enabled>$enabled</enabled>
          <port>$port</port>
          <addr>$ip</addr>
        </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
    )

    if [ $? -ne 0 ]; then err1 "update_device edit-config" "$ret"; fi

# XXX Här rensas all config så man kan inte editera
    new "update_device $name: commit"
    ret=$(${clixon_netconf} -qe0 -f $CFG -E $CFD <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
  <commit/>
</rpc>]]>]]>
EOF
    )
    if [ $? -ne 0 ]; then err1 "update_device commit" "$ret"; fi
}

# Connect device and check open
# Args: name
function connect_device()
{
    local name=$1
    
    new "Connect device $name"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $name 2>&1)" 0 "^$"

    new "Verify $name OPEN"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $name)" 0 "OPEN"
}

# Add a simple interface config edit in configure mode.
# Args: devname value commit(local)
function edit_device_config(){
    local name=$1
    local val=$2
    local commit=$3
    local cmd="set devices device $name config system config login-banner $val"

    new "CLI set login-banner"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    if $commit; then
        new "CLI commit local"
        expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"
    fi
}

# Delete a simple device config edit in configure mode.
# Args: devname value commit(local)
function delete_device_config(){
    local name=$1
    local val=$2
    local commit=$3
    local cmd="delete devices device $name config system config login-banner $val"

    new "CLI delete login-banner"
    expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD $cmd)" 0 "^$"

    if $commit; then
        new "CLI commit local"
        expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit local)" 0 "^$"
    fi
}

# Composite transaction test helper.
# Performs three checks:
#   1. Runs CLI command and checks output type (silent|warning|error|diff)
#   2. Runs "show transactions"        - checks overall result (success|failed)
#   3. Runs "show transactions detail" - checks per-device result (success|skip|failed|absent)
# Args:
#   $1 dev    - device name for per-device detail check               (or "" to skip check)
#   $2 desc   - test description prefix (used for "new" labels)
#   $3 mode   - CLI mode: oper|configure
#   $4 cmd    - CLI command arguments (e.g. "connection open '*'" or "commit push")
#   $5 expect - expected CLI output: silent|warning|error|diff
#   $6 tx     - expected overall transaction result: success|failed  (or "" to skip check)
#   $7 devr   - expected per-device result: success|skip|failed|absent (or "" to skip check)
function check_tx(){
    local dev="$1"
    local desc="$2"
    local mode="$3"
    local cmd="$4"
    local expect="$5"
    local tx="$6"
    local devr="$7"
    local flags
    local out
    local exitcode
    local devblock

    case "$mode" in
        configure) flags="-1 -m configure -f $CFG -E $CFD" ;;
        *)         flags="-1 -f $CFG -E $CFD" ;;
    esac

    # 1. CLI command: run and check output type
    new "$desc: CLI ($expect)"
#    echo "$clixon_cli $flags $cmd"
    out=$(eval "$clixon_cli $flags $cmd" 2>&1)
    exitcode=$?
    case "$expect" in
        silent)
            if [ $exitcode -ne 0 ]; then err1 "exit 0" "exit $exitcode: $out"; fi
            if [ -n "$out" ]; then err "empty output" "$out"; fi
            ;;
        warning)
            if [ $exitcode -ne 0 ]; then err1 "exit 0 with Warning" "exit $exitcode: $out"; fi
            if ! echo "$out" | grep -q "Warning"; then err "Warning in output" "$out"; fi
            ;;
        error)
            if [ $exitcode -eq 0 ]; then err1 "non-zero exit" "exit 0: $out"; fi
            ;;
        diff)
            if [ $exitcode -ne 0 ]; then err1 "exit 0 with diff output" "exit $exitcode: $out"; fi
            if ! echo "$out" | grep -qE "^\+|^-"; then err "diff output (+/-)" "$out"; fi
            ;;
    esac

    # 2. show transactions: check overall result
    if [ -n "$tx" ]; then
        new "$desc: show transactions ($tx)"
#        echo "$clixon_cli -1 -f $CFG -E $CFD show transactions"
        case "$tx" in
            success) expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "SUCCESS" ;;
            failed)  expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "FAILED" ;;
        esac
    fi

    # 3. show transactions detail: check per-device result
    if [ -n "$dev" ] && [ -n "$devr" ]; then
        new "$desc: show transactions detail $dev ($devr)"
#        echo "$clixon_cli -1 -f $CFG -E $CFD show transactions detail"
        out=$($clixon_cli -1 -f $CFG -E $CFD show transactions detail 2>&1)
        case "$devr" in
            absent)
                if echo "$out" | grep -q "<name>$dev</name>"; then
                    err1 "$dev absent from transaction detail" "$out"
                fi
                ;;
            success)
                devblock=$(echo "$out" | grep -A 5 "<name>$dev</name>")
                if [ -z "$devblock" ]; then err1 "$dev in transaction detail" "$out"; fi
                if echo "$devblock" | grep -q "SKIPPED\|FAILED"; then
                    err1 "$dev result SUCCESS (not SKIPPED/FAILED)" "$out"
                fi
                ;;
            skip)
                devblock=$(echo "$out" | grep -A 5 "<name>$dev</name>")
                if ! echo "$devblock" | grep -q "SKIPPED"; then
                    err1 "$dev result SKIPPED in transaction detail" "$out"
                fi
                ;;
            failed)
                devblock=$(echo "$out" | grep -A 5 "<name>$dev</name>")
                if ! echo "$devblock" | grep -q "FAILED"; then
                    err1 "$dev result FAILED in transaction detail" "$out"
                fi
                ;;
        esac
    fi
}

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -E $CFD -z

    new "Start new backend"
    start_backend -s init -f $CFG -E $CFD
fi

new "Wait backend"
wait_backend

# ============================================================
# Setup: Init device connect + pull
# ============================================================
init_device "$NAME2" "$ip2" "true"

new "Setup: Connect device $NAME2"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $NAME2 2>&1)" 0 "^$"

# ============================================================
# A: Connection OPEN
# ============================================================
update_device "$NAME2" "$ip2" "830" "false" # DISABLE

# A1: Connect all with NAME2 disabled -> CLI warning (return 0), NAME2 skipped
check_tx $NAME2 "A1 connect disabled" oper "connection open '*'" silent success skip

update_device "$NAME2" "$ip2" "830" "true" # CLOSED

# A2: Connect NAME2
check_tx $NAME2 "A2 connect closed" oper "connection open '*'" silent success success

new "A2: Verify $NAME2 OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $NAME2)" 0 "OPEN"

# A3: Reconnect OPEN devices
check_tx $NAME2 "A3 connect open" oper "connection open '*'" silent success skipped

# ============================================================
# B: Connection CLOSE
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

# B1: Disconnect disabled
check_tx $NAME2 "B1 disconnect disabled" oper "connection close '*'" silent success skip

update_device "$NAME2" "$ip2" "830" "true" # CLOSED
# B2: Disconnect closed
check_tx $NAME2 "B2 disconnect closed" oper "connection close '*'" silent success skip

connect_device $NAME2

# B3: Disconnect open
check_tx $NAME2 "B2 disconnect open" oper "connection close '*'" silent success success

new "Verify $NAME2 CLOSED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $NAME2)" 0 "CLOSED"

# ============================================================
# C: PULL
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

# C1: Pull disabled
check_tx $NAME2 "C1 pull disabled" oper "pull '*'" silent success skip

update_device "$NAME2" "$ip2" "830" "true" # CLOSED

# C2: Pull closed
check_tx $NAME2 "C2 pull closed" oper "pull '*'" warning success skip

connect_device $NAME2

# C3: Pull open
check_tx $NAME2 "C3 pull open" oper "pull '*'" silent success success

# ============================================================
# D: Show devices diff (with local committed change)
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

edit_device_config "$NAME2" "test" true

# D1: Show devices diff disabled - shows diff of stored config vs candidate
check_tx $NAME2 "D1 show devices diff disabled" oper "show devices $NAME2 diff" diff success success

update_device "$NAME2" "$ip2" "830" "true" # CLOSED
edit_device_config "$NAME2" "test" true

# D2: Show devices diff closed
check_tx $NAME2 "D2 show devices diff closed" oper "show devices $NAME2 diff" warning success skip

connect_device $NAME2

edit_device_config "$NAME2" "test" true

# D3: Show devices diff open
check_tx $NAME2 "D3 show devices diff open" oper "show devices $NAME2 diff" diff success success

delete_device_config "$NAME2" "test" true

# ============================================================
# E: Commit push manual changed
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE
edit_device_config "$NAME2" "test" false

# E1: Commit push manual changed disabled XXX FAIL
check_tx $NAME2 "E1 commit manual changed disabled" configure "commit push" warning success skip

delete_device_config "$NAME2" "test" true

update_device "$NAME2" "$ip2" "830" "true" # CLOSED
edit_device_config "$NAME2" "test" false

# E2: Commit push manual changed closed
check_tx $NAME2 "E2 commit manual changed closed" configure "commit push" error failed failed

delete_device_config "$NAME2" "test" true

connect_device $NAME2
edit_device_config "$NAME2" "test" false

# E3: Commit push manual changed open
check_tx $NAME2 "E3 commit manual changed open" configure "commit push" silent success success

delete_device_config "$NAME2" "test" true

# ============================================================
# F: Commit diff manual change
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE
edit_device_config "$NAME2" "test" false

# F1: Commit diff manual change disabled
check_tx $NAME2 "F1 commit diff manual changed disabled" configure "commit diff" diff success success

delete_device_config "$NAME2" "test" true

update_device "$NAME2" "$ip2" "830" "true" # CLOSED
edit_device_config "$NAME2" "test" false

# F2: Commit diff manual change close
check_tx $NAME2 "F2 commit diff manual changed close" configure "commit diff" skip warning skipped

connect_device $NAME2

delete_device_config "$NAME2" "test" true
edit_device_config "$NAME2" "test" false

# F3: Commit diff manual change open
check_tx $NAME2 "F3 commit diff manual changed open" configure "commit diff" diff success success

delete_device_config "$NAME2" "test" false
check_tx $NAME2 "commit reset" configure "commit push" silent success success

# ============================================================
# X: No local edits:
#   Dx: show devices diff
#   Xb: Commit push manual
#   Xc: Commit diff manual
# ============================================================
# Dx: No edits show devices diff
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

# Dx1: No edits show devices diff disabled
check_tx $NAME2 "Dx1 show devices diff disabled" oper "show devices $NAME2 diff" silent success success

# Dx2: Show devices diff closed
update_device "$NAME2" "$ip2" "830" "true"  # CLOSED (enabled but not connected)
check_tx $NAME2 "Dx2 show devices diff closed" oper "show devices $NAME2 diff" warning success skip

connect_device $NAME2

# Dx3: Show devices diff open
check_tx $NAME2 "Dx3 show devices diff open" oper "show devices $NAME2 diff" silent success success

# ============================================================
# Xb: No edits commit push manual
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

# Xb1: No edits commit push manual disabled
check_tx $NAME2 "Xb1 commit manual changed disabled" configure "commit push" silent success absent

update_device "$NAME2" "$ip2" "830" "true" # CLOSED

# Xb2: No edits commit push manual no-edits closed
check_tx $NAME2 "Xb2 commit manual changed closed" configure "commit push" silent success absent

connect_device $NAME2

# Xb3: No edits Commit push manual no-edits
check_tx $NAME2 "Xb3 commit manual changed open" configure "commit push" silent success absent

# ============================================================
# Xc: No edits Commit diff manual
# ============================================================
update_device "$NAME2" "$ip2" "830" "false"  # DISABLE

# Xc1: No edits commit diff manual disabled
check_tx $NAME2 "Xc1 commit diff manual changed disabled" configure "commit diff" silent success absent

update_device "$NAME2" "$ip2" "830" "true" # CLOSED

# Xc2: No edits commit push manual closed
check_tx $NAME2 "Xc2 commit diff manual changed closed" configure "commit diff" silent success absent

connect_device $NAME2

# Xc3: No edits commit push manual open
check_tx $NAME2 "Xc3 commit diff manual changed closed" configure "commit diff" silent success absent

# ============================================================
# Cleanup
# ============================================================
if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
