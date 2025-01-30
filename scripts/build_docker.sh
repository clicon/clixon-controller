#!/bin/sh

image_name="clixon-controller:latest"

if [ ! -x "$(command -v docker)" ]; then
	echo "Docker not installed."
	exit
fi

if id -nG "$USER" | grep -qw "docker"; then
	sudo_cmd="sudo"
else
	sudo_cmd=""
fi

(cd docker; ${sudo_cmd} docker build -t $image_name -f Dockerfile .)

if [ $? -eq 0 ]; then
	echo "Docker image ${image_name} was built."
else
	echo "Failed to build docker image."
fi