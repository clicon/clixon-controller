# Controller transaction tests focused on UX behavior across device states.
# Based on table.xlsx which defines expected behavior for:
#   3 device states (columns) x 6 operations (rows)
#
# Two devices:
# - NAME1: reference device - always kept in state 3 (open)
# - NAME2: test object - transitions between states
#
# Device states:
#   State 1 DISABLED: enabled=false, conn-state=CLOSED  (administratively disabled)
#   State 2 CLOSED: enabled=true,  conn-state=CLOSED
#   State 3 OPEN: enabled=true,  conn-state=OPEN
#
# Operations:
#   A: Connect           (connection open)
#   B: Disconnect        (connection close)
#   C: Pull              (pull)
#   D: Show devices diff (show devices <name> diff)
#   E: Commit push       (commit push in configure mode, with prior edit)
#   F: Commit diff       (commit diff in configure mode, with prior edit)

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr >= 2"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

ip1=$(echo $CONTAINERS | awk '{print $1}')
ip2=$(echo $CONTAINERS | awk '{print $2}')
NAME1="${IMG}1"
NAME2="${IMG}2"

CFG=${SYSCONFDIR}/clixon/controller.xml
dir=/var/tmp/$0
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

cat<<EOF > $CFD/diff.xml
<?xml version="1.0" encoding="utf-8"?>
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
</clixon-config>
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
    new "update_device $name: edit-config"
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

# Check the result field in the most recent transaction.
# Args: expected-result  (SUCCESS|FAILED)
function check_last_transaction(){
    local expected=$1
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions)" 0 "$expected"
}

# Reconnect and pull the reference device 1 so it stays in state 3.
function ensure_ref_open(){
    new "ensure_ref_open: reconnect $NAME1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $NAME1 2>&1)" 0 "^$"
    sleep $sleep
    new "ensure_ref_open: pull $NAME1"
    expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull $NAME1 2>&1)" 0 "^$"
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
# Setup: Init both devices connect and pull
# ============================================================
init_device "$NAME1" "$ip1" "true"
init_device "$NAME2" "$ip2" "true"

new "Setup: Connect reference device $NAME1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $NAME1 2>&1)" 0 "^$"

sleep $sleep

new "Setup: Pull $NAME1 config"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull $NAME1 2>&1)" 0 "^$"

new "Setup: Connect reference device $NAME2"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $NAME2 2>&1)" 0 "^$"

sleep $sleep

new "Setup: Pull $NAME2 config"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull $NAME1 2>&1)" 0 "^$"

# ============================================================
# STATE 1 DISABLE
# Pre-amble: close NAME1 disable NAME2
# ============================================================

# Close NAME1
new "Disconnect $NAME1"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close $NAME1 2>&1)" 0 "^$"

# Disable NAME2
update_device "$NAME2" "$ip2" "830" "false"

# A1: Connect all with NAME2 disabled -> CLI warning (return 0), NAME2 skipped
new "A1: Connect all (NAME2 disabled) -> CLI warning, transaction SUCCESS"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open '*' 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "A1: Verify transaction SUCCESS (NAME2 skipped)"
check_last_transaction "SUCCESS"

new "A1: Verify transaction Warning"
check_last_transaction "Warning: devices skipped"

new "A1: Verify $NAME2 still DISABLED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $NAME2)" 0 "$NAME2 \ * DISABLED"

ensure_ref_open

# B1: Disconnect all with NAME2 disabled -> NAME2 skipped with warning
new "B1: Disconnect all (NAME2 disabled) -> CLI warning, NAME2 skipped"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close '*' 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "B1: Verify transaction WARNING"
check_last_transaction "Warning: devices skipped"

ensure_ref_open

# C1: Pull all with NAME2 disabled -> CLI warning (return 0), NAME2 skipped
new "C1: Pull all (NAME2 disabled) -> CLI warning, transaction SUCCESS"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull '*' 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "C1: Verify transaction SUCCESS (NAME2 skipped)"
check_last_transaction "SUCCESS"

# D1: Show diff for disabled NAME2 -> CLI error, transaction FAILED
new "D1: Show devices diff (NAME2 disabled) -> CLI error"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices $NAME2 diff 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "D1: Verify transaction SUCCESS"
check_last_transaction "SUCCESS"

# E1: Commit push with NAME2 disabled -> CLI error
new "E1: Add device config"
edit_device_config "$NAME2" "test" false

new "E1: Commit push (NAME2 disabled) -> CLI error" # <---
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "F1: Commit diff (NAME2 disabled) -> CLI warning (return 0)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit diff 2>&1)" 0 "Warning: device '$NAME2' skipped"

# ============================================================
# STATE 2: CLOSED
# Preamble: enable NAME2
# ============================================================
update_device "$NAME2" "$ip2" "830" "true"

new "Setup: Connect reference device $NAME2"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open $NAME2 2>&1)" 0 "^$"

# B2: Disconnect all when NAME2 never connected -> silent
new "B2: Disconnect all (NAME2 enabled+closed) -> CLI silent"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close '*' 2>&1)" 0 "^$"

ensure_ref_open

# C2: Pull all with NAME2 CLOSED -> transaction SUCCESS with warning
new "C2: Pull all (NAME2 enabled+closed) -> CLI OK warning"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull '*' 2>&1)" 0 "Warning: device" "${NAME2}" "skipped"

new "C2: Verify transaction SUCCESS"
check_last_transaction "SUCCESS"

new "C2: Verify transaction Warning"
check_last_transaction "Warning: devices skipped"

new "D2: Edit device config of reference"
edit_device_config "$NAME1" "test" true

# D2: Show diff for NAME2 CLOSED -> CLI error, transaction SUCCESS with warning
new "D2: Show devices diff (NAME2 enabled+closed) -> CLI OK Warning"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices diff 2>&1)" 0 "Warning: device '$NAME2' skipped" "\+.*login-banner"

new "D2: Delete device config of reference"
delete_device_config "$NAME1" "test" true

new "D2: Verify transaction SUCCESS"
check_last_transaction "SUCCESS"

new "D2: Verify transaction Warning"
check_last_transaction "Warning: devices skipped"

# E20: # E2: Commit push with NAME2 CLOSED and NO edits -> skip altogether
new "E20: Commit push (NAME2 enabled+closed+noedits)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "E20: Verify transaction Success"
check_last_transaction "SUCCESS"

new "E20: Verify $NAME1 (no change) not in transaction device list"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions detail 2>&1)" 0 --not-- "$NAME1"

# E2: Commit push with NAME2 CLOSED -> CLI error
new "E2: Edit device config"
edit_device_config "$NAME2" "test" false

new "E2: Commit push (NAME2 enabled+closed) -> CLI error"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 255 "Device is closed" "$NAME2"

new "E2: Verify transaction Failure"
check_last_transaction "FAILED"

new "E2: Discard"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD rollback)" 0 "^$"

# F20: Commit diff with NAME2 CLOSED -> CLI warning (return 0)
new "F20: Commit diff (NAME2 enabled+closed+noedit) -> OK"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit diff 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "F20: Verify transaction Success"
check_last_transaction "SUCCESS"

# F2: Commit diff with NAME2 CLOSED edit -> CLI warning
new "F2: Edit device config"
edit_device_config "$NAME2" "test" false

new "F2: Commit diff (NAME2 enabled+closed) -> CLI warning (return 0)"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit diff 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "F2: Discard change"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD rollback)" 0 "^$"

# A2: Connect all -> NAME1 reconnects, NAME2 opens -> both OPEN, CLI silent
new "A2: Connect all (NAME2 enabled+closed) -> CLI silent, OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open '*' 2>&1)" 0 "^$"

sleep $sleep

new "A2: Verify $NAME2 OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "OPEN"

# ============================================================
# STATE 3: OPEN
# ============================================================
new "State3: Pull all device config"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull '*' 2>&1)" 0 "^$"

# C3: Pull all OPEN devices -> success, CLI silent
new "C3: Pull all (both open) -> CLI silent"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD pull '*' 2>&1)" 0 "^$"

# D3: Show diff for NAME2 OPEN -> return 0
new "D3: Show devices diff (NAME2 enabled+open) -> return 0"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show devices $NAME2 diff 2>&1)" 0 "^$"

# E3: Commit push to all OPEN devices -> success, CLI silent
new "E3: Edit device config"
edit_device_config "$NAME2" "test" false

new "E3: Commit push (both open) -> CLI silent"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit push 2>&1)" 0 "^$"

new "E3: Verify transaction SUCCESS"
check_last_transaction "SUCCESS"

new "E3: Verify only changed device ($NAME2) appears in transaction"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show transactions detail 2>&1)" 0 "$NAME2" --not-- "$NAME1"

# F3: Commit diff with all OPEN devices -> return 0
new "F3: Edit device config"
edit_device_config "$NAME2" "test" false

new "F3: Commit diff (both open) -> return 0"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD commit diff 2>&1)" 0 "^$"

new "F3: Rollback candidate"
expectpart "$($clixon_cli -1 -m configure -f $CFG -E $CFD rollback)" 0 "^$"

# A3: Reconnect all already-OPEN devices -> still OPEN, CLI silent
new "A3: Reconnect all (both open) -> CLI silent"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection reconnect '*' 2>&1)" 0 "^$"

sleep $sleep

new "A3: Verify still OPEN"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "OPEN"

# B3: Disconnect all OPEN devices -> CLOSED, CLI silent
new "B3: Disconnect all (both open) -> CLI silent"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close '*' 2>&1)" 0 "^$"

new "B3: Verify $NAME2 CLOSED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections)" 0 "CLOSED"

ensure_ref_open

# ============================================================
# STATE 2 CLOSED special case: unreachable: port=1
# ============================================================
update_device "$NAME2" "$ip2" "1" "true"

# A2: Connect all with NAME2 unreachable -> CLI error (one device fails)
new "A2: Connect all (NAME2 enabled+unreachable) -> CLI error"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection open '*' 2>&1)" 0 "Connection refused"

new "A2: Verify transaction FAILED"
check_last_transaction "FAILED"

new "A2: Verify $NAME2 still CLOSED"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD show connections $NAME2)" 0 "$NAME2 \ * CLOSED"

ensure_ref_open

# B2: Disconnect all with NAME2 already CLOSED -> NAME2 skipped with warning
new "B2: Disconnect all (NAME2 enabled+unreachable+closed) -> CLI warning, NAME2 skipped"
expectpart "$($clixon_cli -1 -f $CFG -E $CFD connection close '*' 2>&1)" 0 "Warning: device '$NAME2' skipped"

new "B2: Verify transaction WARNING"
check_last_transaction "Warning: devices skipped"

ensure_ref_open

# ============================================================
# Cleanup
# ============================================================
if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG -E $CFD
fi

endtest
