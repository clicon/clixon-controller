#!/bin/sh

# Check if any of the containers controller-demo, r1 or r2 are running
if [ "$(docker ps -q -f name=demo-controller)" ] || [ "$(docker ps -q -f name=r1)" ] || [ "$(docker ps -q -f name=r2)" ]; then
	echo "Error: Containers controller-demo, r1 or r2 are already running. Please stop them before starting the demo."
	exit 1
fi

docker compose -f docker/docker-compose-demo.yml up -d

sleep 5

# Echo the SSH key to the console
echo ""
echo "Public SSH key for controller:"
docker exec -t demo-controller bash -c "cat /root/.ssh/id_rsa.pub"

# Echo the CLI command to the console
echo ""
echo "To start Controller CLI run:"
echo "docker exec -it controller-demo clixon_cli -f /usr/local/etc/clixon/controller.xml"
