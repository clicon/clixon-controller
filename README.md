<div align="center">
  <img src="https://www.clicon.org/Clixon_logga_liggande_med-ikon.png" width="400">
</div>

[![Build Status](https://github.com/clicon/clixon-controller/actions/workflows/test.yml/badge.svg)](https://github.com/clicon/clixon-controller/actions/workflows/test.yml) [![Documentation Status](https://readthedocs.org/projects/clixon-controller-docs/badge/?version=latest)](https://clixon-controller-docs.readthedocs.io/en/latest/?badge=latest) [![codecov](https://codecov.io/gh/clicon/clixon-controller/graph/badge.svg?token=4WUSKL7IQC)](https://codecov.io/gh/clicon/clixon-controller)
<a href="https://scan.coverity.com/projects/clicon-clixon-controller">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/29844/badge.svg"/>
</a>

Clixon controller:
The Clixon controller is an open-source manager of network devices based on NETCONF and YANG.

See [Install](INSTALL.md), [User guide](https://clixon-controller-docs.readthedocs.io/en/latest), [project page](https://www.clicon.org/#controller) and [FAQ](https://clixon-controller-docs.readthedocs.io/en/latest/faq.html)

Clixon interaction is best done posting issues, pull requests, or joining the
[Matrix clixon forum](https://matrix.to/#/#clixonforum:matrix.org).

Other Clixon projects include [CLIgen](https://github.com/clicon/cligen), [Clixon](https://github.com/clicon/clixon), and others.

Python API:
The Clixon controller uses a [Python API](https://github.com/clicon/clixon-pyapi) for services.

Additional features:
* Add, delete and validate device configuration
* Multi vendor support, can use any device capable of using NETCONF and YANG.
* Templates support
* Local configuration datastore
* Loadable device profiles

Demo:
![Demo GIF](https://media.githubusercontent.com/media/clicon/clixon-controller/main/demo.gif)

To launch a container with the controller and two fake routers based on OpenConfig:
```
$ ./start-demo-containers.sh
$ docker exec -it demo-controller clixon_cli
nobody@3e29b6e15c34>
```

The Clixon controller is open-source Apache License, Version 2.0, see [LICENSE](LICENSE).

The controller has a main branch continuously tested with CI.

Clixon controller is sponsored by [SUNET](https://www.sunet.se)
