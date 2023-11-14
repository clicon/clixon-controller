#!/bin/sh

(cd docker; git clone ../ clixon-controller)
docker-compose -f docker/docker-compose-demo.yml up -d

