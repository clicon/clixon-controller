#!/usr/bin/env bash

set -u

# Testfile (not including path)
: ${testfile:=$(basename $0)}

>&2 echo "Running $testfile"

# Generated config file from autotools / configure
if [ -f ./config.sh ]; then
    . ./config.sh
    if [ $? -ne 0 ]; then
        return -1 # error
    fi
fi

# Source the site/test-specific definitions for test script variables
if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step
: ${sleep:=4}

: ${IMG:=openconfig}

# container user
: ${USER:=noc}

# Controller config file
: ${CFG:=${SYSCONFDIR}/clixon/controller.xml}

: ${DBG:=0}

# Namespace: netconf base
BASENS='urn:ietf:params:xml:ns:netconf:base:1.0'

# Namespace: Clixon lib
LIBNS='xmlns="http://clicon.org/lib"'

# Default netconf namespace statement, typically as placed on top-level <hello xmlns=""
DEFAULTONLY="xmlns=\"$BASENS\""

# Default netconf namespace + message-id, ie for <rpc xmlns="" message-id="", but NOT for hello
DEFAULTNS="$DEFAULTONLY message-id=\"42\""

# Multiplication factor to sleep less than whole seconds
DEMSLEEP=1

DEMLOOP=10

# Temp directory where all tests write their data to
dir=/var/tmp/$0
if [ ! -d $dir ]; then
    mkdir $dir
fi

: ${clixon_cli:=clixon_cli}

: ${clixon_netconf:=$(which clixon_netconf)}

: ${clixon_restconf:=clixon_restconf}

: ${clixon_backend:=clixon_backend}

: ${clixon_snmp:=$(type -p clixon_snmp)}

# If set to false, override starting of clixon_backend in test (you bring your own)
: ${BE:=true}

# Test number from start
: ${testnr:=0}

# Test number in this test
testi=0

# Single test. Set by "new"
testname=

# Valgind memory leak check.
# The values are:
# 0: No valgrind check
# 1: Start valgrind at every new testcase. Check result every next new
# 2: Start valgrind every new backend start. Check when backend stops
# 3: Start valgrind every new restconf start. Check when restconf stops
# 4: Start valgrind every new snmp start. Check when snmp stops
#
: ${valgrindtest=0}

#----------------------- Functions

# Test is previous test had valgrind errors if so quit
function checkvalgrind(){
    echo "checkvalgrind $valgrindfile"
    if [ -f $valgrindfile ]; then
        res=$(cat $valgrindfile | grep -e "Invalid" | awk '{print  $4}' | grep -v '^0$')
        if [ -n "$res" ]; then
            >&2 cat $valgrindfile
            sudo rm -f $valgrindfile
            exit -1         
        fi
        res=$(cat $valgrindfile | grep -e "reachable" -e "lost:"| awk '{print  $4}' | grep -v '^0$')
        if [ -n "$res" ]; then
            >&2 cat $valgrindfile
            sudo rm -f $valgrindfile
            exit -1         
        fi
        sudo rm -f $valgrindfile
    fi
}

# Given a string, add RFC6242 chunked franing around it
# Args:
# 0: string
function chunked_framing()
{
    str=$1
    length=$(echo -n "$str"|wc -c)

    printf "\n#%s\n%s\n##\n" ${length} "${str}"
}

# Wait for restconf to stop sending  502 Bad Gateway
function wait_backend(){
    freq=$(chunked_framing "<rpc $DEFAULTNS><ping $LIBNS/></rpc>")
    reply=$(echo "$freq" | ${clixon_netconf} -q1ef $CFG) || true
    #    freply=$(chunked_framing "<rpc-reply $DEFAULTNS><ok/></rpc-reply>")
    #    chunked_equal "$reply" "$freply"
    let i=0 || true
    while [[ $reply != *"<rpc-reply"* ]]; do
#	echo "sleep $DEMSLEEP"
	sleep $DEMSLEEP
        freq=$(chunked_framing "<rpc $DEFAULTNS><ping $LIBNS/></rpc>")
        reply=$(echo "$freq" | ${clixon_netconf} -q1ef $CFG) || true
	let i++ || true
#	echo "wait_backend  $i"
	if [ $i -ge $DEMLOOP ]; then
	    err "backend timeout $DEMLOOP loops" ""
	fi
    done
}

# Increment test number and print a nice string
function new(){
    if [ $valgrindtest -eq 1 ]; then
	checkvalgrind
    fi
    testnr=`expr $testnr + 1`
    testi=`expr $testi + 1`
    testname=$1
    >&2 echo "Test $testi($testnr) [$1]"
}

# Start backend with all varargs.
# If valgrindtest == 2, start valgrind
function start_backend(){
    if [ $valgrindtest -eq 2 ]; then
        # Start in background since daemon version creates two traces: parent,
        # child. If background then only the single relevant.
        echo "$clixon_backend -F -D $DBG $* &"
        sudo $clixon_backend -F -D $DBG $* &
    else
        echo "sudo $clixon_backend -D $DBG $*"
        sudo $clixon_backend -D $DBG $*
    fi
    if [ $? -ne 0 ]; then
        err1 backend 
    fi
}

function stop_backend(){
    if [ $valgrindtest -eq 2 ]; then
        sleep 1         # This is to give backend time to settle into idle
    fi
    sudo clixon_backend -z $*
    if [ $? -ne 0 ]; then
        err "kill backend"
    fi
    if [ $valgrindtest -eq 2 ]; then 
        sleep 5         # This is to give valgrind time to dump log file
        checkvalgrind
    fi
#    sudo pkill -f clixon_backend # extra ($BUSER?)
}

function endtest()
{
    # Commented from now, it is unclear what destroys the tty, if something does the original
    # problem should be fixed at the origin.
    #    stty $STTYSETTINGS >/dev/null
    if [ $valgrindtest -eq 1 ]; then 
        checkvalgrind
    fi
    >&2 echo "End $testfile"
    >&2 echo "OK"
}

# Evaluate and return
# Example: expectpart $(fn arg) 0 "my return" -- "foo"
# - evaluated expression
# - expected command return value (0 if OK) or list of values, eg "55 56"
# - expected stdout outcome*
# - the token "--not--"
# - not expected stdout outcome*
# Example:
# expectpart "$(a-shell-cmd arg)" 0 'expected match 1' 'expected match 2' --not-- 'not expected 1'
# @note need to escape \[\]
function expectpart(){
    r=$?
    ret=$1
    retval=$2
    expect=$3

#    echo "r:$r"
#    echo "ret:\"$ret\""
#    echo "retval:$retval"
#    echo "expect:\"$expect\""
    if [ "$retval" -eq "$retval" 2> /dev/null ] ; then # single retval
	if [ $r != $retval ]; then
	    echo -e "\e[31m\nError ($r != $retval) in Test$testnr [$testname]:"
	    echo -e "\e[0m:"
	    exit -1
	fi
    else # List of retvals
	found=0
	for rv in $retval; do
	    if [ $r == $rv ]; then
		found=1
	    fi
	done
	if [ $found -eq 0 ]; then
	    echo -e "\e[31m\nError ($r != $retval) in Test$testnr [$testname]:"
	    echo -e "\e[0m:"
	    exit -1
	fi
    fi
    if [ -z "$ret" -a -z "$expect" ]; then
	return
    fi
    # Loop over all variable args expect strings (skip first two args)
    # note that "expect" var is never actually used
    # Then test positive for strings, if the token --not-- is detected, then test negative for the rest
    positive=true;
    let i=0 || true;
    for exp in "$@"; do
	if [ $i -gt 1 ]; then
	    if [ "$exp" == "--not--" ]; then
		positive=false;
	    else
		# echo "echo \"$ret\" | grep --null -o \"$exp"\"
		match=$(echo "$ret" | grep --null -i -o "$exp") #-i ignore case XXX -EZo: -E cant handle {}
		r=$?
		if $positive; then
		    if [ $r != 0 ]; then
			err "$exp" "$ret"
		    fi
		else
		    if [ $r == 0 ]; then
			err "not $exp" "$ret"
		    fi
		fi
	    fi
	fi
	let i++ || true;
    done
    #  if [[ "$ret" != "$expect" ]]; then
    #      err "$expect" "$ret"
    #  fi
}

# error and exit,
# arg1: expected
# arg2: errmsg[optional]
# Assumes: $dir and $expect are set
# see err1
function err(){
    expect=$1
    ret=$2
    echo -e "\e[31m\nError in Test$testnr [$testname]:"
    if [ $# -gt 0 ]; then
	echo "Expected"
	echo "$1"
	echo
    fi
    if [ $# -gt 1 ]; then
	echo "Received: $2"
    fi
    echo -e "\e[0m"
    echo "Diff between Expected and Received:"
    diff <(echo "$ret"| od -t c) <(echo "$expect"| od -t c)

    exit -1 #$testnr
}

# Dont print diffs
function err1(){
    echo -e "\e[31m\nError in Test$testnr [$testname]:"
    if [ $# -gt 0 ]; then
	echo "Expected: $1"
	echo
    fi
    if [ $# -gt 1 ]; then
	echo "Received: $2"
    fi
    echo -e "\e[0m"
    exit -1 #$testnr
}
