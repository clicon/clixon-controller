#!/bin/sh

set -e

user=`whoami`
versions_file="`pwd`/clixon_versions.txt"

# Check if root
if [ "$user" = "root" ]; then
    sudo_cmd=""
else
    sudo_cmd="sudo"
fi

system_deps() {

    ${sudo_cmd} apt update
    ${sudo_cmd} apt install -y emacs-nox git make gcc bison libnghttp2-dev libssl-dev flex sudo
}

git_repos() {
    for i in cligen clixon clixon-controller clixon-pyapi; do
	echo "Git: $i"
	if [ ! -d "$i" ]; then
	    git clone https://github.com/clicon/${i}.git
	else
	    (cd $i; git pull)
	fi

	if [ $? != 0 ]; then
	    echo "Git: $i failed"
	    exit
	fi
    done
}

build() {
    if [ "$(getent passwd clicon)" = "" ]; then
	${sudo_cmd} useradd clicon -N
    fi

    if [ "$(getent group clicon)" = "" ]; then
	${sudo_cmd} groupadd clicon
	${sudo_cmd} usermod -a -G clicon $user
    fi

    echo "Build `date`:" >> $versions_file

    for i in cligen clixon clixon-controller; do
	echo "Building $i"

	if [ -f "${i}/Makefile" ]; then
	    (cd $i; ${sudo_cmd} make distclean)
	fi

	(cd $i; echo "${i}: `git rev-parse HEAD`" >> $versions_file; ./configure --enable-debug; make -j $cores; ${sudo_cmd} make install; ${sudo_cmd} ldconfig)
    done

    echo "Building PyAPI"
    (cd clixon-pyapi; echo "clixon-pyapi: `git rev-parse HEAD`" >> $versions_file; ${sudo_cmd} ./install.sh)

    if [ ! -d /usr/local/share/clixon/mounts/ ]; then
	${sudo_cmd} mkdir /usr/local/share/clixon/mounts/
    fi

    echo "" >> $versions_file
}

usage() {
	echo "Usage: $0 [-d] [-b] [-g]"
	echo "  -d: Install system dependencies"
	echo "  -g: Get git repos"
	echo "  -b: Build Clixon controller"
	echo "  -a (default): All of the above"
	exit 0
}

# Read command line options
while getopts "h?bdg" opt; do
	case "$opt" in
	    h|\?)
		usage
		;;
	    b)  build=1
		;;
	    d)  system_deps=1
		;;
	    g)  git_repos=1
		;;
	    *)  usage
		;;
	esac
done

if [ "$system_deps" = "1" ]; then
	system_deps
fi

if [ "$git_repos" = "1" ]; then
	git_repos
fi

if [ "$build" = "1" ]; then
	build
fi
