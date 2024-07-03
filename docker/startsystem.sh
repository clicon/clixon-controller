#!/bin/sh

# ***** BEGIN LICENSE BLOCK *****
# 
# Copyright (C) 2023 Olof Hagsand
#
# This file is part of CLIXON
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ***** END LICENSE BLOCK *****

# This script is copied into the container on build time and runs
# _inside_ the container at start in runtime. It gets environment variables
# from the start.sh script.
# It starts a backend, a restconf daemon and a nginx daemon and exposes ports
# for restconf.
# See also Dockerfile of the example
# Log msg, see with docker logs

set -ux # e but clixon_backend may fail if test is run in parallell

>&2 echo "$0"

# If set, enable debugging (of backend and restconf daemons)
DBG=${DBG:-0}

# Workaround for this error output:
# sudo: setrlimit(RLIMIT_CORE): Operation not permitted
echo "Set disable_coredump false" > /etc/sudo.conf

cat <<EOF > /clixon/clixon-controller/test/site.sh
IMG=openconfig
USER=noc
HOMEDIR=/home/noc
nr=2
sleep=2
CONTAINERS="$CONTAINERS"
EOF

# Generate SSH keys if they don't exist
if [ ! -f /root/.ssh/id_rsa ]; then
    >&2 echo "Generating SSH keys:"
    ssh-keygen -t rsa -b 4096 -f /root/.ssh/id_rsa -N ""
fi

# Echo the public SSH key
>&2 echo "Public SSH key generated:"
cat /root/.ssh/id_rsa.pub


# Start clixon backend
>&2 echo "Starting clixon_backend:"

if [ -z ${USE_STARTUPDB+x} ]; then
    /usr/local/sbin/clixon_backend -FD $DBG -f /usr/local/etc/clixon/controller.xml -l e
else
    echo "Using startup database"
    /usr/local/sbin/clixon_backend -FD $DBG -f /usr/local/etc/clixon/controller.xml -l e -s startup
fi

# Alt: let backend be in foreground, but then you cannot restart
/bin/sleep infinity
