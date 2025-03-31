#!/usr/bin/env bash

set -u

# Testfile (not including path)
: ${testfile:=$(basename $0)}

>&2 echo "Running $testfile"

# Save stty
STTYSETTINGS=$(stty -g)

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
: ${CFD:=${SYSCONFDIR}/clixon/controller}

: ${DBG:=0}

# SSH identityfile
: ${SSHID:=}

# Namespace: netconf base
BASENS='urn:ietf:params:xml:ns:netconf:base:1.0'

# Namespace: Clixon lib
LIBNS='xmlns="http://clicon.org/lib"'

# Namespace: Controller
CONTROLLERNS='xmlns="http://clicon.org/controller"'

# Default netconf namespace statement, typically as placed on top-level <hello xmlns=""
DEFAULTONLY="xmlns=\"$BASENS\""

# Default netconf namespace + message-id, ie for <rpc xmlns="" message-id="", but NOT for hello
DEFAULTNS="$DEFAULTONLY message-id=\"42\""

# Minimal hello message as a prelude to netconf rpcs
DEFAULTHELLO="<?xml version=\"1.0\" encoding=\"UTF-8\"?><hello $DEFAULTONLY><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability><capability>urn:ietf:params:netconf:base:1.1</capability></capabilities></hello>]]>]]>"

# SSL serv cert common name XXX should use DNS resolve
: ${SSLCN:="localhost"}

# Options passed to curl calls
# -s : silent
# -S : show error
# -i : Include HTTP response headers
# -k : insecure
: ${CURLOPTS:="-Ssik"}
# Set HTTP version 1.1 or 2
if ${HAVE_LIBNGHTTP2}; then
    : ${HVER:=2}
else
    : ${HVER:=1.1}
fi

if [ ${HVER} = 2 ]; then
    if ${HAVE_HTTP1}; then
        # This is if http/1 is enabled (unset proto=HTTP_2 in restconf_accept_client)
        CURLOPTS="${CURLOPTS} --http2"
    else
        # This is if http/1 is disabled (set proto=HTTP_2 in restconf_accept_client)
        CURLOPTS="${CURLOPTS} --http2-prior-knowledge"
    fi
else
    CURLOPTS="${CURLOPTS} --http1.1"
fi

# RESTCONF protocol, eg http or https
if [ "${WITH_RESTCONF}" = "fcgi" ]; then
    : ${RCPROTO:=http}
else
    : ${RCPROTO:=https}
fi

# www user (on linux typically www-data, freebsd www)
# Start restconf user, can be root which is dropped to wwwuser
: ${wwwstartuser:=root}

# Multiplication factor to sleep less than whole seconds
DEMSLEEP=1

DEMLOOP=10

DEMWAIT=8

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

# If set to false, override starting of clixon_restconf in test (you bring your own)
: ${RC:=true}

# Where to log restconf. Some systems may not have syslog,
# eg logging to a file: RCLOG="-l f/www-data/restconf.log"
: ${RCLOG:=}

# RESTCONF PID
RCPID=0

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

# Wait for backend
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

# Start restconf daemon
# @see wait_restconf
function start_restconf(){
    # remove -g
    local clixon_restconf_="${clixon_restconf#sudo -g * }"
    # Start in background
    echo "sudo -u $wwwstartuser ${clixon_restconf_} $RCLOG -D $DBG $*"
    sudo -u $wwwstartuser $clixon_restconf_ $RCLOG -D $DBG $* </dev/null &>/dev/null &
    if [ $? -ne 0 ]; then
        err1 "expected 0" "$?"
    fi
    RCPID=$!
}

# Stop restconf daemon before test
# pkill restconf dont work here since there are restconf processes in the restconf container
function stop_restconf_pre(){
    true || sudo kill $RCPID || true # in case RCPID is unbound
}

# Stop restconf daemon after test
# Some problems with pkill:
# 1) Dont use $clixon_restconf (dont work in valgrind)
# 2) Dont use -u $WWWUSER since clixon_restconf may drop privileges.
# 3) After fork, it seems to take some time before name is right
function stop_restconf(){
    if [ $valgrindtest -eq 3 ]; then
        sudo kill ${RCPID}
        sleep 1
        checkvalgrind
    else
        sudo kill $RCPID
    fi
}

# Wait for restconf to stop sending  502 Bad Gateway
# @see start_restconf
# Reasons for not working: if you run native is nginx running?
# @note assumes port=80 if RCPROTO=http and port=443 if RCPROTO=https
# Args:
# 1: (optional) override RCPROTO with http or https
function wait_restconf(){
    if [ $# = 1 ]; then
        myproto=$1
    else
        myproto=${RCPROTO}
    fi
#    echo "curl $CURLOPTS -X GET $myproto://localhost/restconf"
    hdr=$(curl $CURLOPTS -X GET $myproto://localhost/restconf 2> /dev/null)
    stty $STTYSETTINGS >/dev/null
#    echo "hdr:\"$hdr\""
    let i=0;
    while [[ "$hdr" != *"200"* ]]; do
#       echo "wait_restconf $i"
        if [ $i -ge $DEMLOOP ]; then
            err1 "restconf timeout $DEMWAIT seconds"
        fi
        sleep $DEMSLEEP
#       echo "curl $CURLOPTS -X GET $myproto://localhost/restconf"
        hdr=$(curl $CURLOPTS -X GET $myproto://localhost/restconf 2> /dev/null)
#       echo "hdr:\"$hdr\""
        let i++;
    done
    if [ $valgrindtest -eq 3 ]; then
        sleep 2 # some problems with valgrind
    fi
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
        err1 "kill backend"
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

# Default restconf configuration: IPv4
# Can be placed in clixon-config
# Note that https clause assumes there exists certs and keys in /etc/ssl,...
# Args:
# 1: auth-type (one of none, client-cert, user)
# 2: pretty (if true pretty-print restconf return values)
# 3: debug (decimal)
# 4: log-destination (syslog/file/...)
# 5: timeout
# 6: httpdata (false/true)
# Note, if AUTH=none then FEATURE clixon-restconf:allow-auth-none must be enabled
# Note if https, check if server cert/key exists, if not generate them
function restconf_config()
{
    AUTH=$1
    PRETTY=$2
    DEBUG=$3
    LOGDEST=$4
    TIMEOUT=$5
    HTTPDATA=$6

    certdir=$dir/certs
    if [ ! -f ${dir}/clixon-server-crt.pem ]; then
        certdir=$dir/certs
        test -d $certdir || mkdir $certdir
        srvcert=${certdir}/clixon-server-crt.pem
        srvkey=${certdir}/clixon-server-key.pem
        cacert=${certdir}/clixon-ca-crt.pem
        cakey=${certdir}/clixon-ca-key.pem
        cacerts $cakey $cacert
        servercerts $cakey $cacert $srvkey $srvcert
    fi
    echo -n "<restconf xmlns=\"http://clicon.org/restconf\">"
    echo -n "<enable>true</enable>"
    echo -n "<debug>$DEBUG</debug>"
    echo -n "<timeout>$TIMEOUT</timeout>"
    if ${HTTPDATA}; then
        echo -n "<enable-http-data>true</enable-http-data>"
    fi
    echo "<auth-type>$AUTH</auth-type><pretty>$PRETTY</pretty><server-cert-path>${certdir}/clixon-server-crt.pem</server-cert-path><server-key-path>${certdir}/clixon-server-key.pem</server-key-path><server-ca-cert-path>${certdir}/clixon-ca-crt.pem</server-ca-cert-path><socket><namespace>default</namespace><address>0.0.0.0</address><port>443</port><ssl>true</ssl></socket></restconf>"
}

# Create CA certs
# Output variables set as filenames on entry, set as cert/keys on exit:
# Vars:
# 1: cakey   filename
# 2: cacert  filename
function cacerts()
{
    if [ $# -ne 2 ]; then
        echo "cacerts function: Expected: cakey cacert"
        exit 1
    fi
    local cakey=$1
    local cacert=$2

    tmpdir=$dir/tmpcertdir

    test -d $tmpdir || mkdir $tmpdir

    # 1. CA
    cat<<EOF > $tmpdir/ca.cnf
[ ca ]
default_ca      = CA_default

[ CA_default ]
serial = ca-serial
crl = ca-crl.pem
database = ca-database.txt
name_opt = CA_default
cert_opt = CA_default
default_crl_days = 9999
default_md = md5

[ req ]
default_bits           = ${CERTKEYLEN}
days                   = 1
distinguished_name     = req_distinguished_name
attributes             = req_attributes
prompt                 = no
output_password        = password

[ req_distinguished_name ]
C                      = SE
L                      = Stockholm
O                      = Clixon
OU                     = clixon
CN                     = ca
emailAddress           = olof@hagsand.se

[ req_attributes ]
challengePassword      = test

[ x509v3_extensions ]
basicConstraints = critical,CA:true
EOF

    # Generate CA cert
    openssl req -batch -new -x509 -extensions x509v3_extensions -days 1 -config $tmpdir/ca.cnf -keyout $cakey -out $cacert || err1 "Generate CA cert"
    rm -rf $tmpdir
}

# Create server certs
# Output variables set as filenames on entry, set as cert/keys on exit:
# Vars:
# 1: cakey   filename (input)
# 2: cacert  filename (input)
# 3: srvkey  filename (output)
# 4: srvcert filename (output)
function servercerts()
{
    if [ $# -ne 4 ]; then
        echo "servercerts function: Expected: cakey cacert srvkey srvcert"
        exit 1
    fi
    cakey=$1
    cacert=$2
    srvkey=$3
    srvcert=$4

    tmpdir=$dir/tmpcertdir

    test -d $tmpdir || mkdir $tmpdir

    cat<<EOF > $tmpdir/srv.cnf
[req]
prompt = no
distinguished_name = dn
req_extensions = ext
[dn]
CN = ${SSLCN} # localhost
emailAddress = olof@hagsand.se
O = Clixon
L = Stockholm
C = SE
[ext]
subjectAltName = DNS:clicon.org
EOF

    # Generate server key
    openssl genpkey -algorithm RSA -out $srvkey  || err1 "Generate server key"

    # Generate CSR (signing request)
    openssl req -batch -new -config $tmpdir/srv.cnf -key $srvkey -out $tmpdir/srv_csr.pem || err1 "Generate signing request"

    # Sign server cert by CA
    openssl x509 -req -extfile $tmpdir/srv.cnf -days 1 -passin "pass:password" -in $tmpdir/srv_csr.pem -CA $cacert -CAkey $cakey -CAcreateserial -out $srvcert || err1 "Sign server cert"

    rm -rf $tmpdir
}
