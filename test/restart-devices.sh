#!/usr/bin/env bash

set -u

./stop-devices.sh
sudo pkill clixon_backend
sudo pkill clixon_restconf
./start-devices.sh
sudo ./copy-keys.sh
