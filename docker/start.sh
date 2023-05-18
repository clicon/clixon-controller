#!/usr/bin/env bash
# Setup env before test for native, start container
# Called from make test

set -ex

: ${IMG:=clixon-example}
: ${NAME:=clixon-controller}
# Number of device containers to start
: ${nr:=2}

testdir=../test
(cd $testdir; nr=$nr ./start-devices.sh)

echo "start controller"
CONTAINERS="$(./containers.sh)" ./start-controller.sh

# Create controller key and copy public key to example dockers
echo "sudo docker exec -t $NAME bash -c 'ssh-keygen -t rsa -f /root/.ssh/id_rsa -N ""'"
sudo docker exec -t $NAME bash -c 'ssh-keygen -t rsa -f /root/.ssh/id_rsa -N ""'

echo "sudo docker cp $NAME:/root/.ssh/id_rsa.pub ./tmpkey"
sudo docker cp $NAME:/root/.ssh/id_rsa.pub ./tmpkey

for i in $(seq 1 $nr); do
    NAME2=$IMG$i

    echo "sudo docker cp ./tmpkey $NAME2:/root/.ssh/authorized_keys2"
    sudo docker cp ./tmpkey $NAME2:/root/.ssh/authorized_keys2

    echo "sudo docker exec -t $NAME2 sh -c 'cat .ssh/authorized_keys2 >> .ssh/authorized_keys'"
    sudo docker exec -t $NAME2 sh -c 'cat /root/.ssh/authorized_keys2 >> /root/.ssh/authorized_keys'
done

rm -f ./tmpkey

echo "start OK"
