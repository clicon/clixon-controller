<div align="center">
  <img src="https://www.clicon.org/Clixon_logga_liggande_med-ikon.png" width="400">
</div>

[![Build Status](https://github.com/clicon/clixon-controller/actions/workflows/test.yml/badge.svg)](https://github.com/clicon/clixon-controller/actions/workflows/test.yml)

# clixon-controller
Clixon network controller

## Build clixon

See https://clixon-docs.readthedocs.io/en/latest/install.html

Configure clixon as follows:

```
> ./configure --enable-yang-schema-mount
```

## Build and install:

```
> ./configure
> make        # native build
> sudo make install
```

or via Docker (does not work yet)

## Devices

For each device, add an entry in a startup config, example:
```
$ cat /usr/local/var/controller/startup_db
<config>
  <devices xmlns="http://clicon.org/controller">
    <device>
      <name>clixon</name>
      <description>Clixon container</description>
      <conn-type>NETCONF_SSH</conn-type>
      <user>root</user>
      <addr>172.17.0.2</addr>
    </device>
    ...
  <devices xmlns="http://clicon.org/controller">
</config>
```

Start the controller:
```
$ sudo clixon_backend -s startup
```


