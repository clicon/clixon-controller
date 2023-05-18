#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
# To use root as login to containers from root, use: 
# PREFIX=sudo start-devices.sh
set -eu

# Number of device containers to start
: ${nr:=2}

# Sleep delay in seconds between each step
: ${sleep:=5}

: ${IMG:=clixon-example}

# sudo if ssh login from root
: ${PREFIX:=} 

SSHKEY=~/.ssh/id_rsa.pub

${PREFIX} test -f $SSHKEY || ${PREFIX} ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME || true
    sudo docker run --name $NAME --rm -td clixon/$IMG #|| err "Error starting clixon-example"
    sudo docker exec -t $NAME mkdir -m 700 /root/.ssh
    sudo docker cp $SSHKEY $NAME:/root/.ssh/authorized_keys
    sudo docker exec -t $NAME chown root /root/.ssh/authorized_keys
    sudo docker exec -t $NAME chgrp root /root/.ssh/authorized_keys
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    ssh-keygen -f ~/.ssh/known_hosts -R "$ip" || true
done

sleep $sleep # need time to spin up backend in containers

# Add parameters x and y
for i in $(seq 1 $nr); do
    NAME=$IMG$i

    echo "get ip of $NAME"
    ip=$(sudo docker inspect $NAME -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
    
    echo "Configure $NAME w ip:$ip"
    ssh $ip -l root -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <edit-config>
    <target><candidate/></target>
    <config>
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
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42"><commit/></rpc>]]>]]>
EOF

done

echo
echo "start-devices OK"
