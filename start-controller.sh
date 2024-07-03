#!/bin/sh

docker-compose -f docker/docker-compose.yml up -d

# Echo the SSH key to the console
echo "SSH key for controller:"
docker exec -it controller cat /root/.ssh/id_rsa.pub

# Echo the CLI command to the console
echo "To start Controller CLI run:"
echo "docker exec -it controller clixon_cli -f /usr/local/etc/clixon/controller.xml"
