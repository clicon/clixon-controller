#
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
#

FROM alpine
MAINTAINER Olof Hagsand <olof@hagsand.se>

# For clixon and cligen
RUN apk add --update git make build-base gcc flex bison curl-dev

# nghttp2 dependencies
RUN apk add --update nghttp2

# Create a directory to hold source-code, dependencies etc
RUN mkdir /clixon

RUN mkdir /clixon/build
WORKDIR /clixon

# Clone cligen
RUN git clone https://github.com/clicon/cligen.git

# Build cligen
WORKDIR /clixon/cligen
RUN ./configure --prefix=/clixon/build
RUN make
RUN make install

# Need to add www user manually
RUN adduser -D -H -G www-data www-data

# Clone clixon
WORKDIR /clixon
RUN git clone https://github.com/clicon/clixon.git
WORKDIR /clixon/clixon

# Configure, build and install clixon
RUN ./configure --prefix=/clixon/build --with-cligen=/clixon/build --with-restconf=native --enable-nghttp2

RUN make
RUN make install
RUN make install-include

# Clone openconfig YANGs from git repo
RUN mkdir /clixon/build/share/openconfig
WORKDIR /clixon/build/share/openconfig
RUN git clone https://github.com/openconfig/public

# Create paths locally (copied to /usr/local in stage 2)
# Paths are according to controller.xml config file.
# To make this adaptive, an installation script inside the container can be made, but now
# the hardcoded paths are used.
RUN mkdir /clixon/build/var
RUN mkdir /clixon/build/var/controller
RUN mkdir /clixon/build/lib/controller
RUN mkdir /clixon/build/lib/controller/clispec
RUN mkdir /clixon/build/lib/controller/backend

# Copy src file from local: config file and standard clispec file
COPY src/controller.xml /clixon/build/etc/clixon.xml
COPY src/controller_cli.cli /clixon/build/lib/controller/clispec/
COPY yang/clixon-*.yang /clixon/build/share/clixon/

# XXX BUILD CONTROLLER, NEED TO CONFIGURE IN ALPINE?
RUN mkdir /clixon/controller
COPY src/*.c /clixon/controller/
COPY src/*.h /clixon/controller/
COPY src/Makefile /clixon/controller/

# XXX Build and install the controller application
WORKDIR /clixon/clixon/example/main
RUN make
RUN make install
RUN install example.xml /clixon/build/etc/clixon.xml

# Copy startscript for starting backend and restconf
COPY startsystem.sh /clixon/build/bin/

#
# Stage 2
# The second step skips the development environment and builds a runtime system
FROM alpine
MAINTAINER Olof Hagsand <olof@hagsand.se>

# For clixon and cligen
RUN apk add --update flex bison

# need to add www user manually
RUN adduser -D -H -G www-data www-data

RUN apk add --update openssl

# nghttp2 dependencies
RUN apk add --update nghttp2

# Expose http port for restconf
EXPOSE 80/tcp
# EXPOSE 443/tcp

# Create clicon user and group
RUN adduser -D -H clicon

COPY --from=0 /clixon/build/ /usr/local/

# Start the backend and restconf deamons
CMD /usr/local/bin/startsystem.sh
