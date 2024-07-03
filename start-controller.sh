#!/bin/sh

docker-compose -f docker/docker-compose.yml up -d

echo ""

# Echo the SSH key to the console
echo "Public SSH key for controller:"
docker exec -t controller bash -c "cat /root/.ssh/id_rsa.pub"

echo ""

# Echo the CLI command to the console
echo "To start Controller CLI run:"
echo "docker exec -it controller clixon_cli -f /usr/local/etc/clixon/controller.xml"
