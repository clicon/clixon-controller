#!/usr/bin/env bash
# Copy keys from homedir to clixon-example containers
# Assume rsa key exists in homedir
# HOSTHOMEDIR made explicit since ~ / $HOME not always works with sudo calls
# Which means you have two kinds of calls:
#   HOSTHOMEDIR=/root sudo ./copy-keys.sh # root
#   HOSTHOMEDIR=~ ./copy-keys.sh # user
# The latter is already done by start-devices.sh (if there is where you came from)

set -u

if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

# REAL image must be prepended by clixon/
: ${REALIMG:=clixon/$IMG}

# Native homedir (~ is treachorous)
: ${HOSTHOMEDIR:=/root}

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker exec -t $NAME sh -c "test -d ${HOMEDIR}/.ssh || mkdir -m 700 ${HOMEDIR}/.ssh"
    sudo docker exec -t $NAME sh -c "chown $USER ${HOMEDIR}/.ssh"
    sudo docker cp $HOSTHOMEDIR/.ssh/id_rsa.pub $NAME:${HOMEDIR}/.ssh/authorized_keys2
    sudo docker exec -t $NAME sh -c "cat ${HOMEDIR}/.ssh/authorized_keys2 >> ${HOMEDIR}/.ssh/authorized_keys"

    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    echo "ssh-keygen -f ${HOSTHOMEDIR}/.ssh/known_hosts -R \"$ip\""
    ssh-keygen -f ${HOSTHOMEDIR}/.ssh/known_hosts -R "$ip" || true # Remove previous entry
    # Populate known hosts
    ssh $ip -l $USER -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
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
