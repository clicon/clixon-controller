#!/bin/sh

if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

# Get all IP addresses from running containers
for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' $NAME
done
