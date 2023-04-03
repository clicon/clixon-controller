#!/usr/bin/env bash

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

# Controller config file
: ${CFG:=/usr/local/etc/controller.xml}

# If set to true, start containers and controller, otherwise they are assumed to be already running
: ${INIT:=false}

# Prefix to add in front of all client commands.
# Eg to force all client to run as root if there is problem with group assignment (see github actions)
: ${PREFIX:=}

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
DEMSLEEP=0.2

: ${clixon_cli:=clixon_cli}

: ${clixon_netconf:=$(which clixon_netconf)}

: ${clixon_restconf:=clixon_restconf}

: ${clixon_backend:=clixon_backend}

: ${clixon_snmp:=$(type -p clixon_snmp)}

# If set to false, override starting of clixon_backend in test (you bring your own) 
: ${BE:=true}

if $INIT; then
    # Start devices
    nr=$nr ./stop-devices.sh
    sleep $sleep
    nr=$nr ./start-devices.sh

    if $BE; then
        echo "Kill old backend"
        sudo clixon_backend -s init -f $CFG -z

        echo "Start new backend"
        sudo clixon_backend -s init  -f $CFG -D $DBG
    fi
fi

#----------------------- Functions

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
    reply=$(echo "$freq" | $PREFIX ${clixon_netconf} -q1ef $CFG)
#    freply=$(chunked_framing "<rpc-reply $DEFAULTNS><ok/></rpc-reply>")
#    chunked_equal "$reply" "$freply"
    let i=0 || true
    while [[ $reply != *"<rpc-reply"* ]]; do
       echo "sleep $DEMSLEEP"
        sleep $DEMSLEEP
        reply=$(echo "<rpc $ÐEFAULTSNS $LIBNS><ping/></rpc>]]>]]>" | $PREFIX ${clixon_netconf} -qef $CFG 2> /dev/null)
       echo "reply:$reply"
        let i++;
       echo "wait_backend  $i"
        if [ $i -ge $DEMLOOP ]; then
            err "backend timeout $DEMWAIT seconds"
        fi
    done
}
