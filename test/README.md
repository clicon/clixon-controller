# Clixon controller test directory

## Docker

You can run the tests via top-level `make test` which sets up all in a container. This is also done by
github actions regression tests.

## Native

You can also run test natively, in which case you need to set up some things manually

### Native prerequisites

You need to install the following:

- cligen
  - bison
  - flex
- clixon
- clixon-util
- clixon-pyapi
- curl
- expect
- docker

### Devices

Create site.sh, for example:
```
IMG=openconfig
USER=noc
HOMEDIR=/home/noc
nr=2
sleep=2
```

Start device containers. Default is `clixon/clixon-example` but you can also start `clixon/openconfig`:
```
    ./start-devices.sh
```

If run natively, the controller runs as root, and a root public key needs to be installed in the clixon-example containers:
```
    sudo ./copy-keys.sh
```

Bind the `CONTAINERS` env variable to the IP:s of the clixon-example containers, either manually or for example:
```
    echo "CONTAINERS=\"$(IMG=openconfig ./containers.sh)\"" >> ./site.sh
    ./sum.sh
```

To stop the device containers:
```
    ./stop-devices.sh
```

### SSH certs

There is some support for SSH certs when testing natively. Instead of `start-devices.sh` and `copy-keys-sh` do:
```
    ./cert-keys.sh
```

and in site.sh:
```
   SSHID="-i <dir>/user-key"
```

Also, one needs to change the root identityfile, either directly in /root/.ssh or:
```
   <CONTROLLER_SSH_IDENTITYFILE xmlns="http://clicon.org/controller-config"><dir>/root-key</CONTROLLER_SSH_IDENTITYFILE>
```

## Tests

* test-change-both.sh          Change config on device and check diff
* test-change-ctrl-push.sh     Change device config on controller and push to devices
* test-change-device-diff.sh   Change config on device and check diff
* test-cli-edit-config.sh      CLI set/show
* test-cli-edit-multiple.sh    CLI set/delete using glob '*'
* test-cli-show-config.sh      CLI show config tests
* test-local-commit.sh         Connect/commit/push
* test-service.sh              Non pyapi service test 
* test-yanglib.sh              Test RFC8528 YANG Schema Mount state

Tests names without `cli` indicates a netconf test.

### Support scripts. Can either be run individually, or are used by the main test- scripts

* all.sh                Verbosely run all tests, stop on error, use pattern=<glob> ./all.sh for subset
* sum.sh                Summary tests, continue on error
* lib.sh                Support script, all tests include this
* containers.sh         Get IP addresses of all started clixon-example containers
* copy-keys.sh          Copy user or root public rsa key to clixon-example containers
* reset-controller.sh   Reset controller by initiating with clixon-example devices and a sync pull
* start-devices.sh      Stop/start clixon-example container devices, initiate with x=11, y=22
* stop-devices.sh       Stop clixon-example container devices
* reset-devices.sh      Initiate clixon-example devices with config x=11, y=22
* change-devices.sh     Change device config: Remove x, change y, and add z

### Modifiers

The following modifiers apply to some tests:

* nr=<nr>               Number of devices
* nrif=<nr>             Number of configured interfaces (size of datastore). 2 is min
* BE=false              Do not start backend in script, start backend externally instead,
                        ie in a debugger (init-controller.sh only)
* push=false            Only change dont sync push (change-push.sh only)
* sleep=``<s>``         Sleep `s` seconds instead of 2 (all)
* PREFIX=sudo           Generate keys for root (device scripts only)

### RESTCONF

To test with fcgi restconf:

* Configure clixon: ./configure --with-restconf=fcgi
* In controller test/config.sh.in:
  WITH_RESTCONF=fcgi
  HAVE_LIBNGHTTP2=false
* Re-configure: ./config.status

To test with native restconf:

* Configure clixon: ./configure
* In controller test/config.sh.in:
  WITH_RESTCONF=native
  HAVE_LIBNGHTTP2=true
* Re-configure: ./config.status
