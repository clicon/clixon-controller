#!/usr/bin/env bash
# Stop clixon example container devices
set -eu

if [ -f ./site.sh ]; then
    . ./site.sh
    if [ $? -ne 0 ]; then
        return -1 # skip
    fi
fi

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    echo "Kill device $NAME"
    sudo docker kill $NAME || true
done

echo "stop-devices OK"
