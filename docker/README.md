# Clixon controller docker image

This directory contains code for running, building and pushing the clixon
controller docker container. 

## Requirements

docker

## Build

Perform the build by `make docker`. This copies the latest committed clixon code into the container.

## Example run

First, the container is started with a backend and a restconf listening on port 8082:
```
  $ sudo docker run --rm -p 8082:80 --name controller -d clixon/controller
```

You can start a CLI with some example commands:
```
  $ sudo docker exec -it controller clixon_cli
cli> ...
cli> q   
```

You can also use netconf via stdin/stdout:
```
  $ sudo docker exec -it controller clixon_netconf
  <?xml version="1.0" encoding="UTF-8"?>
  <hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
     <capabilities><capability>urn:ietf:params:netconf:base:1.1</capability></capabilities>
   </hello>]]>]]>
  <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><get-config><source><running/></source></get-config></rpc>]]>]]>
  <rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><data>...
```

Or using restconf using curl on exposed port 8082:
## Example run
```
  $ curl -X GET http://localhost:8082/restconf/data/
{
}
```

## Cleanup

```
  $ ./cleanup.sh # kill and remove container
  $ make clean   # reset build system caches
```

## Push

You may also do `make push` if you want to push the image, but you may then consider changing the image name (in the Makefile:s).

(You may have to login for push with sudo docker login -u <username>)

# Docker Compose Environment

A `docker-compose.yml` is provided that will launch two instances of the Clixon openconfig example (r1 & r2), then the controller.

The Controller's entrypoint is overridden to create a keypair and install the public key on each of r1 and r2 such that the controller can perform passwordless authentication as user "noc".

The docker-compose.yml assumes that the clixon/openconfig and clixon/controller images have alreay been built per instructions found elsewhere, or published to Dockerhub; that is, `docker compose build` will not build them.

## Startup

```
  $ docker compose up
```

Once the containers are running, the controller CLI can be run and devices added:

```
  $ docker compose up -d
  Creating network "docker_default" with the default driver
  Creating r2 ... done
  Creating r1 ... done
  Creating controller ... done
  $ docker exec -it controller clixon_cli -f /usr/local/etc/clixon/controller.xml
  nobody@d18823315355> configure
  nobody@d18823315355[/]# set devices device r1 addr r1
  nobody@d18823315355[/]# set devices device r1 user noc
  nobody@d18823315355[/]# set devices device r1 enabled true
  nobody@d18823315355[/]# set devices device r1 conn-type NETCONF_SSH
  nobody@d18823315355[/]# set devices device r1 yang-config BIND
  nobody@d18823315355[/]# set devices device r2 addr r2
  nobody@d18823315355[/]# set devices device r2 user noc
  nobody@d18823315355[/]# set devices device r2 enabled true
  nobody@d18823315355[/]# set devices device r2 conn-type NETCONF_SSH
  nobody@d18823315355[/]# set devices device r2 yang-config BIND
  nobody@d18823315355[/]# commit local
  nobody@d18823315355[/]# exit
  nobody@d18823315355> show devices
  Name                    State      Time                   Logmsg
  =======================================================================================
  r1                      CLOSED
  r2                      CLOSED
  nobody@d18823315355> connection open
  nobody@d18823315355> show devices
  Name                    State      Time                   Logmsg
  =======================================================================================
  r1                      OPEN       2023-08-17T14:56:34
  r2                      OPEN       2023-08-17T14:56:34
```
