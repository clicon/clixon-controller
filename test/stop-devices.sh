#!/usr/bin/env bash
# Stop clixon example container devices and initiate with config x=11, y=22
set -eux

# Number of device containers to start
: ${nr:=2}

: ${IMG:=clixon-example}

for i in $(seq 1 $nr); do
    NAME=$IMG$i
    sudo docker kill $NAME || true
done

echo OK
