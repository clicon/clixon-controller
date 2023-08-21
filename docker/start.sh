#!/usr/bin/env bash
# Setup env before test for native, start container
# Create ssh-key pair in controller container and copy public key to device containers
# Called from make test
# You may need to run cleanup.sh before

set -ex

# inline site.sh
cat <<EOF > ./site.sh
IMG=openconfig
USER=noc
HOMEDIR=/home/noc
nr=2
sleep=2
EOF
. ./site.sh

: ${NAME:=clixon-controller}

#testdir=../test
#(cd $testdir; nr=$nr ./start-devices.sh)
../test/start-devices.sh

echo "start controller"
CONTAINERS="$(../test/containers.sh)"
echo "CONTAINERS=\"$CONTAINERS\"" >> ./site.sh
echo "CONTAINERS:$CONTAINERS"
CONTAINERS="$CONTAINERS" ./start-controller.sh

# Create controller key and copy public key to example dockers
echo "sudo docker exec -t $NAME bash -c 'ssh-keygen -t rsa -f /root/.ssh/id_rsa -N ""'"
sudo docker exec -t $NAME bash -c 'ssh-keygen -t rsa -f /root/.ssh/id_rsa -N ""'

echo "sudo docker cp $NAME:/root/.ssh/id_rsa.pub ./tmpkey"
sudo docker cp $NAME:/root/.ssh/id_rsa.pub ./tmpkey

for i in $(seq 1 $nr); do
    NAME2=$IMG$i

    echo "sudo docker cp ./tmpkey $NAME2:$HOMEDIR/.ssh/authorized_keys2"
    sudo docker cp ./tmpkey $NAME2:$HOMEDIR/.ssh/authorized_keys2

    echo "sudo docker exec -t $NAME2 sh -c 'cat .ssh/authorized_keys2 >> .ssh/authorized_keys'"
    sudo docker exec -t $NAME2 sh -c "cat $HOMEDIR/.ssh/authorized_keys2 >> $HOMEDIR/.ssh/authorized_keys"
done

rm -f ./tmpkey

echo "start OK"
