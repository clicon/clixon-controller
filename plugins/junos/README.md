# Clixon controller junos plugins

These plugins are special purpose plugins that should be compiled and
installed in $(libdir)/controller/backend or cli.

The directory contains the following plugins:

- controller_junos_native.be.so Backend plugin for Junos native for non rpc- and yang-compliant
                                Juniper PTX,MX and QFX (possibly others). The plugin rewrites
                                the XML config on pull/sync and push/commit

To build and install a plugin, for example controller_junos_native.be.so:

make controller_junos_native.so
make install # XXX This installs all
