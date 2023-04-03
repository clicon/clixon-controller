Clixon controller test directory

Prereqs:
- devices run, backend runs (use INIT=true) to start them

Tests:
test-change-ctrl-push.sh   Change device config on controller and push to devices
test-change-device-diff.sh Change config on device and check diff
test-change-both.sh        Change config on device and check diff
test-yanglib.sh            Test RFC8528 YANG Schema Mount state

Modifiers:
n=<nr>                Apply on <nr> devices (all)
BE=false              Do not start backend in script, start backend externally instead,
                      ie in a debugger (init-controller.sh only)
push=false            Only change dont sync push (change-push.sh only)
sleep=<s>             Sleep <s> seconds instead of 2 (all)
INIT=false            Only run test, dont start containers/backend (test-*.sh)

Support scripts. Can either be run individually, or are used by the main test- scripts
start-devices.sh      Stop/start clixon-example container devices, initiate with x=11, y=22
stop-devices.sh       Stop clixon-example container devices
reset-devices.sh      Initiate clixon-example devices with config x=11, y=22
change-devices.sh     Change device config: Remove x, change y, and add z
reset-controller.sh   Reset controller by initiaiting with clixon-example devices and a sync pull
