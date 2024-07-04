#!/bin/sh

# Check if containers are already running
if [ "$(docker ps -q -f name=controller)" ]; then
	echo "Containers are already running. Please stop them first."
	exit 1
fi

docker-compose -f docker/docker-compose.yml up -d

sleep 5

# Echo the SSH key to the console
echo ""
echo "Public SSH key for controller:"
docker exec -t controller bash -c "cat /root/.ssh/id_rsa.pub"

# Echo the CLI command to the console
echo ""
echo "To start Controller CLI run:"
echo "docker exec -it controller clixon_cli -f /usr/local/etc/clixon/controller.xml"
