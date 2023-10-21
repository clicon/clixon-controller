#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
# Note: dont use with sudo, you need proper ~/ $HOME set for this script to work
set -eu

if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

# REAL image must be prepended by clixon/
: ${REALIMG:=clixon/$IMG}

: ${SSHKEY:=~/.ssh/id_rsa}

# Generate rsa key if not exists

if [ ! -f "${SSHKEY}" ]; then
    echo "id_rsa not fund"
fi

if [ ! -f "${SSHKEY}.pub" ]; then
    echo "id_rsa.pub not found"
fi

if [ ! -f "${SSHKEY}" ] && [ ! -f "${SSHKEY}.pub" ]; then
    echo "Keys not found"
    ssh-keygen -t rsa -N "" -f $SSHKEY
fi

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME 2> /dev/null || true 
    sudo docker run --name $NAME --rm -td $REALIMG #|| err "Error starting clixon-example"
    sudo docker exec -t $NAME sh -c "test -d ${HOMEDIR}/.ssh || mkdir -m 700 ${HOMEDIR}/.ssh"
    sudo docker exec -t $NAME sh -c "chown $USER ${HOMEDIR}/.ssh"
    sudo docker cp ~/.ssh/id_rsa.pub $NAME:$HOMEDIR/.ssh/authorized_keys
    sudo docker exec -t $NAME chown $USER $HOMEDIR/.ssh/authorized_keys
    sudo docker exec -t $NAME chgrp $USER $HOMEDIR/.ssh/authorized_keys
    sudo docker exec -t $NAME chmod 700 $HOMEDIR/.ssh/authorized_keys
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    ssh-keygen -f ~/.ssh/known_hosts -R "$ip" || true
done

sleep $sleep # need time to spin up backend in containers

echo
echo "start-devices OK"
