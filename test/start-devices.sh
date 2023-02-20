#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
set -eux

# Number of device containers to start
: ${nr:=2}

IMG=clixon-example

SSHKEY=/root/.ssh/id_rsa.pub
sudo test -f $SSHKEY
if [ $? -ne 0 ]; then
    sudo ssh-keygen -t rsa -N "" -f /root/.ssh/id_rsa
fi

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME || true
    sudo docker run --name $NAME --rm -td clixon/$IMG #|| err "Error starting clixon-example"
    sudo docker exec -t $NAME mkdir -m 700 /root/.ssh
    sudo docker cp $SSHKEY $NAME:/root/.ssh/authorized_keys
    sudo docker exec -t $NAME chown root /root/.ssh/authorized_keys
    sudo docker exec -t $NAME chgrp root /root/.ssh/authorized_keys
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    sudo ssh-keygen -f "/root/.ssh/known_hosts" -R "$ip" || true
done

sleep 5 # need time to spin up backend in containers

# Add parameters x and y
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    sudo ssh $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><edit-config><target><candidate/></target><config>
<table xmlns="urn:example:clixon">
   <parameter>
      <name>x</name>
      <value>11</value>
   </parameter>
   <parameter>
      <name>y</name>
      <value>22</value>
   </parameter>
</table>
</config></edit-config></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><commit/></rpc>]]>]]>
EOF

done
echo
