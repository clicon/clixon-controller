<div align="center">
  <img src="https://www.clicon.org/Clixon_logga_liggande_med-ikon.png" width="400">
</div>

[![Build Status](https://github.com/clicon/clixon-controller/actions/workflows/test.yml/badge.svg)](https://github.com/clicon/clixon-controller/actions/workflows/test.yml)

# Clixon Controller
Clixon network controller is an open-source manager of network devices based on NETCONF and YANG.

See [Install](INSTALL.md), [User guide](https://clixon-controller-docs.readthedocs.io/en/latest/controller.html) and [FAQ](FAQ.md)

The controller has a main branch continuously tested with CI.

Clixon interaction is best done posting issues, pull requests, or joining the
Matrix clixon forum https://matrix.to/#/#clixonforum:matrix.org.

Other Clixon projects include [CLIgen](https://github.com/clicon/cligen), [Clixon](https://github.com/clicon/clixon), and others.

# Features
* Add, delete and validate device configuration
* Multi vendor support, can use any device capable of using NETCONF and YANG.
* Services which can be implemented using an Python API [Clixon PyAPI](https://github.com/clicon/clixon-pyapi)
* Templates support, [demo](templates-min.gif)
* Local configration database
* Loadable device profiles
* And much more...

# Demo

To launch a container with the controller and two fake routers based on OpenConfig:
```
$ docker-compose -f docker/docker-compose-demo.yml up
$ docker exec -it demo-controller clixon_cli
nobody@3e29b6e15c34>
```

![Demo](demo-min.gif)

# License
The clixon controller is open-source Apache License, Version 2.0, see [LICENSE](LICENSE).
