# Clixon controller native setup

  * [Content](#content)
  * [Compile and run](#compile)
  * [Using the CLI](#using-the-cli)
  * [Netconf](#netconf)	
  * [Restconf](#restconf)
  
## Content

This directory contains the Clixon controller. It contains the following files:
* `controller.xml` the XML configuration file
* `controller_backend_plugin.c`: a backend plugin
* `controller_cli.cli`: the CLIgen spec
* `Makefile.in`: where plugins are built and installed

## Compile and run

Before you start,
* Install [cligen and clixon](https://clixon-docs.readthedocs.io/en/latest/install.html)
* Create groups [groups](https://github.com/clicon/clixon/blob/master/doc/FAQ.md#do-i-need-to-setup-anything)

```
    make && sudo make install
```
Start backend in the background:
```
    sudo clixon_backend -f /usr/local/etc/clixon/controller.xml
```
Note: use `-s init` instead if you want to start Clixon without the preconfigured restconf daemon

Start cli:
```
    clixon_cli -f /usr/local/etc/clixon/controller.xml
```

## Using the CLI

The example CLI allows you to modify and view the data model using `set`, `delete` and `show` via generated code.

The following example shows how to add a very simple configuration `hello world` using the generated CLI. The config is added to the candidate database, shown, committed to running, and then deleted.
```
   olof@vandal> clixon_cli -f /usr/local/etc/clixon/controller.xml
cli> q   
```

## Netconf

Clixon also provides a Netconf interface. The following example starts a netconf client form the shell, adds the hello world config, commits it, and shows it:
```
olof@vandal> clixon_netconf -qf /usr/local/etc/clixon/controller.xml
  <?xml version="1.0" encoding="UTF-8"?>
  <hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
     <capabilities><capability>urn:ietf:params:netconf:base:1.1</capability></capabilities>
   </hello>]]>]]>
  <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><get-config><source><running/></source></get-config></rpc>]]>]]>
  <rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><data>...
olof@vandal> 
```

## Restconf

Clixon also provides a Restconf interface. See [documentation on RESTCONF](https://clixon-docs.readthedocs.io/en/latest/restconf.html).

Send restconf commands (using Curl):
```
  $ curl -X GET http://localhost:8080/restconf/data/
{
  ...
}
```


