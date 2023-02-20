<div align="center">
  <img src="https://www.clicon.org/Clixon_logga_liggande_med-ikon.png" width="400">
</div>

[![Build Status](https://github.com/clicon/clixon-controller/actions/workflows/test.yml/badge.svg)](https://github.com/clicon/clixon-controller/actions/workflows/test.yml)

# clixon-controller
Clixon network controller

## Installation

### Install required packages
sudo apt install flex bison git make gcc libnghttp2-dev libssl-dev

### Clone the Git repositories

```console
$ git clone https://github.com/clicon/cligen.git
$ git clone https://github.com/clicon/clixon.git
$ git clone https://github.com/clicon/clixon-controller.git
```

### Build the components

Cligen:
```console
$ cd cligen
$ ./configure
$ make
$ sudo make install
```

Clixon:
```console
cd clixon
./configure
make
sudo make install
```

Clixon controller
```console
cd clixon-controller
./configure
make
sudo make install
sudo mkdir /usr/local/share/clixon/mounts/
```

### Start devices

Example using clixon-example device::
```console
cd test
./start-devices.sh
```

### Start controller

Kill old backend and start a new:
```console
sudo clixon_backend -f /usr/local/etc/controller.xml -z
sudo clixon_backend -f /usr/local/etc/controller.xml

Start the CLI and configure devices

```console
clixon_cli -f /usr/local/etc/controller.xml

configure
set devices device crpd1 addr 172.20.20.2
set devices device crpd1 user root
set devices device crpd1 conn-type NETCONF_SSH
set devices device crpd1 root
commit
```
