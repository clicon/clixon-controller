# Clixon controller docker image

This directory contains code for running, building and pushing the clixon
controller docker container. 

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
## Example run```
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
