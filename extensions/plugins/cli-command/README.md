# CLI command plugin

The CLI command plugin allows mapping of CLI commands to arbitrary shell commands on the server side.
To build and install the CLI command plugin, follow these steps:

```bash
cd cli-command
make
make install
```

You have to create a CLI specification file that maps CLI commands to shell commands.
An example specification named example.cli is provided in the cli-command directory.
The CLI specification file should be installed in the Clixon CLI specification directory:

```bash
cp example.cli /usr/local/lib/controller/clispec/
```
