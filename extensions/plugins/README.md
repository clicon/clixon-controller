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

- multiple-announced-yangs
                Extend device configuration with option to select among multiple announced yangs.
                That is, same YANG name but multiple revisions.
                By default the latest YANG is selected, but some devices (old junos-qfx) may announce a
                later yang which is broken and one may want select the older.

To build and install a plugin, for example junos_native, do:

cd junos-native
make
make install
