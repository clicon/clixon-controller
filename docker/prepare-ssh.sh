#!/bin/bash

# Generate SSH key
ssh-keygen -q -t rsa -f /root/.ssh/id_rsa -N ""

# Copy SSH key to r1 and r2
for i in `seq 3`; do
    sshpass -p noc ssh-copy-id -o StrictHostKeyChecking=accept-new noc@r1
    if [ $? == 0 ]; then
	break
    fi

    sleep 3
done

for i in `seq 3`; do
    sshpass -p noc ssh-copy-id -o StrictHostKeyChecking=accept-new noc@r2
    if [ $? == 0 ]; then
	break
    fi

    sleep 3
done

# Execute the original entrypoint
# exec "$@"
/usr/local/bin/startsystem.sh
