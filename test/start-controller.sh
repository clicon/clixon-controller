#!/usr/bin/env bash
# Start clixon example container devices and initiate with config x=11, y=22
set -e

: ${NAME:=clixon-controller}

: ${sleep:=5}

sudo docker kill $NAME || true
sudo docker run --name $NAME --rm -td $IMG

sleep $sleep # need time to spin up backend in containers

# Check if container is started
sudo docker ps | grep $NAME || exit 1

echo "controller started"
