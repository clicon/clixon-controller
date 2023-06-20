#!/bin/sh

# This script is used to install Clixon controller packages.  The
# script will take a directory as input and copy all Python files to a
# certain target directory and YANG files to another target directory.

CLIXON_MODULES="/usr/local/share/clixon/controller/modules/"
CLIXON_YANG="/usr/local/share/clixon/controller/"

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "  -s Source path"
    echo "  -m Clixon controller modules install path"
    echo "  -y Clixon controller YANG install path"
    echo "  -r Use with care: Reset Clixon controller modules and YANG paths"
    echo "  -h help"
}

install_yang() {
    echo "Installing YANG files from \"$SOURCE_PATH\" to \"$CLIXON_YANG\":"
    echo `find $SOURCE_PATH -name "*.yang"`
    find $SOURCE_PATH -name "*.yang" -exec cp {} $CLIXON_YANG \;
    echo ""
}

install_modules() {
    echo "Installing Python files from \"$SOURCE_PATH\" to \"$CLIXON_MODULES\":"
    echo `find $SOURCE_PATH -name "*.py"`
    find $SOURCE_PATH -name "*.py" -exec cp {} $CLIXON_MODULES \;
    echo ""
}

if [ $# -eq 0 ]; then
    usage
    exit 1
fi

while getopts "s:m:y:rh" opt; do
    case $opt in
	s)
	    SOURCE_PATH=$OPTARG
	    ;;
	m)
	    CLIXON_MODULES=$OPTARG
	    ;;	
	y)
	    CLIXON_YANG=$OPTARG
	    ;;
	r)
	    RESET=1
	    ;;
	h)
	    usage
	    exit 0
	    ;;
	*)
	    usage
	    exit 1
	    ;;
    esac
done

if [ -n "$RESET" ]; then
    rm -rf $CLIXON_MODULES/*
    rm -rf $CLIXON_YANG/*

    exit 0
fi

if [ -z "$SOURCE_PATH" ]; then
    echo "Source path is not specified"
    exit 1
fi

install_yang
install_modules
