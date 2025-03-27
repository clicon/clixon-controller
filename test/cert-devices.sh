#!/usr/bin/env bash
# Start clixon example container devices and initiate with config
# This script uses SSH certificates (see also start-devices.sh)
set -ex

if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

# REAL image must be prepended by clixon/
: ${REALIMG:=clixon/$IMG}

: ${ADDRS:=172.17.0.*} # Hardcoded
: ${HOSTCA:=./host_ca}
: ${USERCA:=./user_ca}
: ${USERKEY:=./user-key}
: ${ROOTKEY:=./root-key}

if [ ! -f ${HOSTCA} ]; then
    # Generate host CA
    ssh-keygen -t rsa -b 4096 -f ${HOSTCA} -C ${HOSTCA} -N ''
    # Issue host certificate (to authenticate hosts to users)
    ssh-keygen -f ssh_host_rsa_key -N '' -b 4096 -t rsa
fi
if [ ! -f ${USERCA} ]; then
    # Generate user CA
    ssh-keygen -t rsa -b 4096 -f ${USERCA} -C ${USERCA} -N ''
fi

# Issue user certificate
rm -f ${USERKEY} ${USERKEY}.pub ${USERKEY}-cert.pub
ssh-keygen -f ${USERKEY} -b 4096 -t rsa -N ''
ssh-keygen -s user_ca -I $(whoami) -n ${USER} -V +1h ${USERKEY}.pub

# Issue root certificate
rm -f ${ROOTKEY} ${ROOTKEY}.pub ${ROOTKEY}-cert.pub
ssh-keygen -f ${ROOTKEY} -b 4096 -t rsa -N ''
ssh-keygen -s user_ca -I root -n ${USER} -V +1h ${ROOTKEY}.pub

# Add the CA's public key to client known_hosts file.
if ! grep -q "@cert-authority ${ADDRS}" ~/.ssh/known_hosts; then
    entry="@cert-authority ${ADDRS} $(cat ${HOSTCA}.pub)"
    echo "$entry" >> ~/.ssh/known_hosts
fi

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME 2> /dev/null || true 
    sudo docker run --name $NAME --rm -td $REALIMG #|| err "Error starting clixon-example"

    sudo docker cp ${USERCA}.pub $NAME:$HOMEDIR/
    sudo docker exec -t $NAME sh -c "cp ${HOMEDIR}/${USERCA}.pub /etc/ssh/"

    sudo docker exec -t $NAME sh -c "ssh-keygen -t rsa -b 4096 -f ${HOSTCA} -C ${HOSTCA} -N ''"
    sudo docker exec -t $NAME sh -c "ssh-keygen -f ssh_host_rsa_key -N '' -b 4096 -t rsa"

    IP=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    # Maybe use ip instead of name
    sudo docker exec -t $NAME sh -c "ssh-keygen -s host_ca -I ${IP} -h -n ${IP} -V +1w ssh_host_rsa_key.pub"
    sudo docker exec -t $NAME sh -c "cp ./ssh_host_rsa_key /etc/ssh/"
    sudo docker exec -t $NAME sh -c "cp ./ssh_host_rsa_key.pub /etc/ssh/"
    sudo docker exec -t $NAME sh -c "cp ./ssh_host_rsa_key-cert.pub /etc/ssh/"
    sudo docker exec -t $NAME sh -c "sed -i '2i\
HostCertificate /etc/ssh/ssh_host_rsa_key-cert.pub
' /etc/ssh/sshd_config"
    sudo docker exec -t $NAME sh -c "sed -i '3i\
TrustedUserCAKeys /etc/ssh/user_ca.pub
' /etc/ssh/sshd_config"
    sudo docker exec -t $NAME sh -c "ps aux| grep /usr/sbin/sshd | grep -v grep | awk '{print \$1}' | xargs kill -HUP" || true
    ssh-keygen -f '/home/olof/.ssh/known_hosts' -R "${IP}"
#    if ssh-keygen -H -F ${IP}; then
#        ssh-keygen -H -R ${IP}
#    fi
    # To check: ssh -vv ${IP} 2>&1 | grep "Server host certificate"
    # user
    ssh $USER@$IP -i ${USERKEY} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
EOF
    # root
    sudo ssh-keygen -f '/root/.ssh/known_hosts' -R "${IP}"
    sudo ssh $USER@$IP -i ${ROOTKEY} -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
EOF
done
    
sleep $sleep # need time to spin up backend in containers

# Cleanup
if false; then
    rm -f ${HOSTCA} ${HOSTCA}.pub
    rm -f ${USERCA} ${USERCA}.pub
    rm -f ${USERKEY} ${USERKEY}.pub ${USERKEY}-cert.pub
    rm -f ${ROOTKEY} ${ROOTKEY}.pub ${ROOTKEY}-cert.pub
fi

echo
echo "cert-devices OK"
