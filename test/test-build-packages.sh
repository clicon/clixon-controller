#!/usr/bin/env bash

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

dependency_packages="git make gcc bison libnghttp2-dev libssl-dev flex python3 python3-pip sudo debhelper-compat python3-all dh-python python3-yaml python3-xmltodict debhelper procps"

new "Kill the old buildtest container"
sudo docker kill buildtest || true

# Verify that the container is not running
new "Verify that the container is not running"
sleep 5
expectpart "$(sudo docker ps -q -f name=buildtest)" 0 ""

new "Remove the old buildtest container"
expectpart "$(sudo docker rm buildtest || true)" 0 ""

new "Run the buildtest container"
expectpart "$(sudo docker run -d -it --name buildtest debian:latest)" 0 ""
sleep 5

new "Install the dependencies"
expectpart "$(sudo docker exec -it buildtest apt update)" 0 ""
expectpart "$(sudo docker exec -it buildtest apt install -y $dependency_packages)" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'useradd clicon')" 0 ""

new "Build and install the Clicon PyAPI package"
expectpart "$(sudo docker exec -it buildtest git clone https://github.com/clicon/clixon-pyapi.git)" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c '(cd clixon-pyapi; ./scripts/build_deb.sh)')" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'dpkg -i clixon-pyapi/python-clixon-pyapi_*.deb')" 0 ""

new "Build and install the Cligen package"
expectpart "$(sudo docker exec -it buildtest git clone https://github.com/clicon/cligen.git)" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c '(cd cligen; ./scripts/build_deb.sh)')" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'dpkg -i cligen/libcligen_*.deb')" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'dpkg -i cligen/libcligen-dev_*.deb')" 0 ""

new "Build and install the Clixon package"
expectpart "$(sudo docker exec -it buildtest git clone https://github.com/clicon/clixon.git)" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c '(cd clixon; ./scripts/build_deb.sh)')" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'dpkg -i clixon/clixon_*.deb')" 0 ""

new "Build and install the Clixon controller package"
expectpart "$(sudo docker exec -it buildtest git clone https://github.com/clicon/clixon-controller.git)" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c '(cd clixon-controller; ./scripts/build_deb.sh)')" 0 ""
expectpart "$(sudo docker exec -it buildtest bash -c 'dpkg -i clixon-controller/clixon-controller_*.deb')" 0 ""

new "Start the Clixon controller"
expectpart "$(sudo docker exec -it buildtest bash -c 'clixon_backend -f /usr/local/etc/clixon/controller.xml')" 0 ""

new "Verify that the Clixon controller is running"
expectpart "$(sudo docker exec -it buildtest clixon_cli -f /usr/local/etc/clixon/controller.xml -1 show version)" 0 "CLIgen" "Clixon" "Controller" "Build"
