# Clixon controller test directory

## Docker

You can run the tests via top-level `make test` which sets up all in a container. This is also done by
github actions regression tests.

## Native

You can also run test natively, in which case you need to set up some things manually

### Native

Start clixon-example containers:
```
    start-devices.sh      
```

If run natively, the controller runs as root, and a root public key needs to be installed in the clixon-example containers::
```
    sudo ./copy-keys.sh
```

Bind the `CONTAINERS` env variable to the IP:s of the clixon-example containers:
```
    CONTAINERS=$(./containers.sh)
```

Run tests with CONTAINERS bound:
```
    CONTAINERS=$CONTAINERS ./sum.sh
```

## Tests

* test-change-both.sh          Change config on device and check diff
* test-change-ctrl-push.sh     Change device config on controller and push to devices
* test-change-device-diff.sh   Change config on device and check diff
* test-cli-edit-commit-push.sh Baseline edit/commit/push using pyapi
* test-cli-edit-multiple.sh    cli set/delete using glob '*'
* test-cli-show-config.sh      cli show config tests
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

* nr=<nr>               Apply on <nr> devices (all)
* BE=false              Do not start backend in script, start backend externally instead,
                        ie in a debugger (init-controller.sh only)
* SA=false              Do not start service-action daemon (test-service only)
* push=false            Only change dont sync push (change-push.sh only)
* sleep=<s>             Sleep <s> seconds instead of 2 (all)
* PREFIX=sudo           Generate keys for root (device scripts only)
