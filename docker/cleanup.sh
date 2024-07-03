#!/usr/bin/env bash

: ${NAME:=controller-test}
: ${IMG:=clixon/controller-test:latest}

rm -rf src yang
# Kill all controller containers (optionally do `make clean`)
sudo docker kill ${NAME} 2> /dev/null # ignore errors
