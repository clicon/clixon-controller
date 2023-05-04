#!/bin/sh

# Get all IP addresses from running containers
IP_ADDRESSES=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' $(docker ps -q))

echo $IP_ADDRESSES
