#!/usr/bin/env bash

: ${NAME:=clixon-controller}
: ${IMG:=clixon/controller:latest}

rm -rf src yang
# Kill all controller containers (optionally do `make clean`)
sudo docker kill ${NAME} 2> /dev/null # ignore errors
