# Clixon controller extension plugins

These plugins are special purpose plugins that should be compiled and
installed in $(libdir)/controller/backend or cli.

Plugins are not tested by the controller regression tests.

The following plugins exist:

- junos_native   Backend plugin for Junos native for NON rpc- and yang-compliant
                 Juniper PTX,MX and QFX (possibly others). The plugin rewrites
                 the XML config on pull/sync and push/commit

- cli-command   CLI plugin to run arbitrary shell commands on the server side.
                The plugin maps CLI commands to shell commands and returns
                the output as CLI output.

To build and install a plugin, for example junos_native, do:

cd junos-native
make
make install
