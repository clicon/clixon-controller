#!/bin/bash

# Generate SSH key
ssh-keygen -q -t rsa -f /root/.ssh/id_rsa -N ""

# Copy SSH key to r1 and r2
sshpass -p noc ssh-copy-id -o StrictHostKeyChecking=accept-new noc@r1
sshpass -p noc ssh-copy-id -o StrictHostKeyChecking=accept-new noc@r2

# Execute the original entrypoint
# exec "$@"
/usr/local/bin/startsystem.sh

