#!/usr/bin/env bash
# Stop clixon example container devices
set -eux

# Number of device containers to start
: ${nr:=2}

: ${IMG:=clixon-example}

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME || true
done

echo "stop-devices"
echo OK
