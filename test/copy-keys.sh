#!/usr/bin/env bash
# Copy keys from homedir to clixon-example containers
# Assume rsa key exists in homedir
# HOMEDIR made explicit since ~ / $HOME not always works with sudo calls
# Which means you have two kinds of calls:
#   HOMEDIR=/root sudo ./copy-keys.sh # root
#   HOMEDIR=~ ./copy-keys.sh # user
# The latter is already done by start-devices.sh (if there is where you came from)

set -eu

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step
: ${sleep:=2}

: ${IMG:=clixon-example}

# Explicit homedir (~ is treachorous)
: ${HOMEDIR:=/root}

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker cp $HOMEDIR/.ssh/id_rsa.pub $NAME:/root/.ssh/authorized_keys2
    sudo docker exec -t $NAME sh -c 'cat /root/.ssh/authorized_keys2 >> /root/.ssh/authorized_keys'
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    echo "ssh-keygen -f ${HOMEDIR}/.ssh/known_hosts -R \"$ip\""
    ssh-keygen -f ${HOMEDIR}/.ssh/known_hosts -R "$ip" || true # Remove previous entry
    # Populate known hosts
    ssh $ip -l root -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
EOF
done

echo
echo "copy-keys OK"
