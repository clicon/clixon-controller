# Security Policy

## Supported Versions

The Clixon controller's `main` branch is continuously tested by CI. Releases
are cut from `main` (see [CHANGELOG.md](CHANGELOG.md) and
[Releases](https://github.com/clicon/clixon-controller/releases)). Security
fixes are made against `main` and included in the next release; only the
latest release and `main` are supported.

The controller is built on [Clixon](https://github.com/clicon/clixon),
[CLIgen](https://github.com/clicon/cligen) and
[Clixon PyAPI](https://github.com/clicon/clixon-pyapi). Vulnerabilities in
those components may be reported the same way.

## Reporting a Vulnerability

Please report security vulnerabilities privately by email to
**info@clicon.org**, rather than filing a public GitHub issue.

Include, if possible:
- A description of the vulnerability and its potential impact.
- Steps to reproduce, or a minimal example (eg a NETCONF/RESTCONF request, CLI
  command, service definition, device profile, or device reply) that
  triggers it.
- The affected component (backend, CLI, NETCONF, RESTCONF, PyAPI services,
  southbound NETCONF/SSH device handling, plugins, etc.) and version or
  commit.

We will acknowledge your report, investigate, and work with you on a fix and
disclosure timeline before any public announcement.
