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

# Start PyAPI
python3 /usr/local/bin/clixon_server.py -m /clixon/clixon-pyapi/modules/ -d -F &

# Start clixon backend
>&2 echo "start clixon_backend:"
/usr/local/sbin/clixon_backend -FD $DBG -f /usr/local/etc/controller.xml -l e # logs on docker logs

(cd clixon-controller; git switch docker)

# Alt: let backend be in foreground, but then you cannot restart
/bin/sleep 100000000
