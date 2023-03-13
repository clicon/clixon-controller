#!/usr/bin/env bash

set -eux

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step                                      
: ${sleep:=2}

: ${IMG:=clixon-example}

# Controller config file
: ${CFG:=/usr/local/etc/controller.xml}

# If set to true, start containers and controller, otherwise they are assumed to be already running
: ${INIT:=false}

: ${DBG:=0}

if $INIT; then
    # Start devices
    nr=$nr ./stop-devices.sh
    sleep $sleep
    nr=$nr ./start-devices.sh

    # If set to false, override starting of clixon_backend in test (you bring your own) 
    : ${BE:=true}

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
    reply=$(echo "$freq" | $clixon_netconf -q1ef $cfg) 
#    freply=$(chunked_framing "<rpc-reply $DEFAULTNS><ok/></rpc-reply>")
#    chunked_equal "$reply" "$freply"
    let i=0;
    while [[ $reply != *"<rpc-reply"* ]]; do
#       echo "sleep $DEMSLEEP"
        sleep $DEMSLEEP
        reply=$(echo "<rpc $ÐEFAULTSNS $LIBNS><ping/></rpc>]]>]]>" | clixon_netconf -qef $cfg 2> /dev/null)
#       echo "reply:$reply"
        let i++;
#       echo "wait_backend  $i"
        if [ $i -ge $DEMLOOP ]; then
            err "backend timeout $DEMWAIT seconds"
        fi
    done
}
