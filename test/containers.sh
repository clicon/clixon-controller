#!/bin/sh

# Default container name, postfixed with 1,2,..,<nr>
: ${IMG:=clixon-example}

# Number of devices to get IP from
: ${nr:=2}

# Get all IP addresses from running containers
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' $NAME
done
